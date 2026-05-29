#include "ice_agent.h"
#include "../stun/stun_message.h"
#include "../stun/stun_auth.h"
#include "../session/ice_session.h"
#include "../turn/turn_client.h"
#include "hloop.h"
#include "hlog.h"
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace ice {

static constexpr int MAX_RETRANSMIT = 4;  // ICE check: 4 retransmits (~1.5s max)
static constexpr uint32_t MAX_RTO = 800; // Cap RTO at 800ms
struct StunTransaction {
    TransactionId id;
    StunCallback callback;    // Callback on response or timeout
    // retransmission
    std::vector<uint8_t> msg;  // encoded STUN message for retransmission
    sockaddr_u destAddr;      // destination address for retransmission
    hio_t* io = nullptr;      // IO handle for retransmission
    IceAgent* agent = nullptr;

    uint64_t sentTime = 0;    // ms
    int retransmitCount = 0;
    uint32_t rto = 50;                        // Initial RTO ms (50ms for ICE checks)
    htimer_t* timer = nullptr;                // Retransmit timer (userdata = this StunTransaction*)

    ~StunTransaction() {
        if (timer) {
            htimer_del(timer);
            timer = nullptr;
        }
    }
};

IceAgent::IceAgent(hv::EventLoopPtr loop) {
    if (loop) {
        loop_ = loop;
    } else {
        loop_thread_.reset(new hv::EventLoopThread());
        loop_ = loop_thread_->loop();
    }

    memset(&tcp_unpack_setting_, 0, sizeof(unpack_setting_t));
    tcp_unpack_setting_.mode = UNPACK_BY_LENGTH_FIELD;
    tcp_unpack_setting_.package_max_length = DEFAULT_PACKAGE_MAX_LENGTH;
    tcp_unpack_setting_.body_offset = 2;
    tcp_unpack_setting_.length_field_offset = 0;
    tcp_unpack_setting_.length_field_bytes = 2;
    tcp_unpack_setting_.length_field_coding = ENCODE_BY_BIG_ENDIAN;
    tcp_unpack_setting_.length_adjustment = 0;
}

IceAgent::~IceAgent() {
    stop();
}

void IceAgent::setConfig(const IceConfig& config) {
    config_ = config;
}

int IceAgent::start() {
    if (running_) return 0;

    hloop_t* loop = loop_->loop();
    if (!loop) return -1;

    // Create UDP server
    udp_io_ = hloop_create_udp_server(loop, config_.bindHost.c_str(), config_.udpPort);
    if (!udp_io_) return -1;

    memcpy(&udp_local_addr_, hio_localaddr(udp_io_), SOCKADDR_LEN(hio_localaddr(udp_io_)));
    udp_port_ = sockaddr_port(&udp_local_addr_);

    hio_setcb_read(udp_io_, [](hio_t* io, void* buf, int readbytes) {
        IceAgent* self = (IceAgent*)hio_context(io);
        if (self) {
            auto addr = hio_peeraddr(io);
            self->onRecvPdu((const uint8_t*)buf, readbytes, addr, io);
        }
    });
    hio_set_context(udp_io_, this);
    hio_read(udp_io_);

    // Create TCP server (if enabled)
    if (config_.gatherTcp) {
        tcp_listen_io_ = hloop_create_tcp_server(loop, config_.bindHost.c_str(), config_.tcpPort, onTcpAccept);
        if (!tcp_listen_io_) {
            hio_close(udp_io_);
            udp_io_ = nullptr;
            return -2;
        }
        hevent_set_userdata(tcp_listen_io_, this);
        tcp_port_ = sockaddr_port((sockaddr_u*)hio_localaddr(tcp_listen_io_));
    }

    if (loop_thread_ && !loop_thread_->isRunning()) {
        loop_thread_->start();
    }
    hlogi("IceAgent start udpPort=%d tcpPort=%d", udp_port_, tcp_port_);
    running_ = true;
    return 0;
}

void IceAgent::stop() {
    if (!running_) return;
    running_ = false;
    hlogi("IceAgent stop with %zu sessions", sessions_.size());
    for (auto& session : sessions_) {
        session->close();
    }
    sessions_.clear();

    // Cleanup TURN client
    if (turn_client_) {
        turn_client_.reset();
    }

    // Close TCP connections
    for (auto& kv : tcp_connections_) {
        if (auto io = kv.second) {
            if(nullptr == hio_context(io)) { // Prevent callbacks after close
                hio_close(io);
            }
        }
    }
    tcp_connections_.clear();

    if (tcp_listen_io_) {
        hio_close(tcp_listen_io_);
        tcp_listen_io_ = nullptr;
    }

    if (udp_io_) {
        hio_close(udp_io_);
        udp_io_ = nullptr;
    }
    ufrag_map_.clear();
    pair_map_.clear();

    // Cancel pending transactions
    for (auto& kv : transactions_) {
        delete kv.second; // ~StunTransaction() handles htimer_del
    }
    transactions_.clear();

    if (loop_thread_) {
        loop_thread_->stop(true);
    }
}

IceSessionPtr IceAgent::createSession(IceMode mode) {
    auto session = std::make_shared<IceSession>(mode, this, loop_);
    sessions_.push_back(session);
    return session;
}

void IceAgent::destroySession(const IceSessionPtr& session) {
    session->close();
    sessions_.erase(
        std::remove(sessions_.begin(), sessions_.end(), session),
        sessions_.end());
}

// ---- UDP APIs ----
int IceAgent::send(const void* data, size_t len, const struct sockaddr* addr, hio_t* io) {
    if (!io) {
        // sendViaRelay
        if (turn_client_ && turn_client_->isAllocated())
            return turn_client_->sendData(data, len, addr);
        return -1;
    }
    if (hio_type(io) == HIO_TYPE_TCP) {
        uint8_t header[2];
        header[0] = (uint8_t)((len >> 8) & 0xFF);
        header[1] = (uint8_t)(len & 0xFF);
        hio_write(io, header, 2);
        return hio_write(io, data, len);
    } else {
        return hio_sendto(io, data, len, (struct sockaddr*)addr);
    }
}

void IceAgent::registerSession(const std::string& ufrag, IceSession* session) {
    loop_->runInLoop([this, ufrag, session]() {
        ufrag_map_[ufrag] = session;
    });
}

void IceAgent::unregisterSession(const std::string& ufrag) {
    loop_->runInLoop([this, ufrag]() {
        ufrag_map_.erase(ufrag);
    });
}

void IceAgent::registerPair(const sockaddr_u& addr, IceSession* session) {
    loop_->runInLoop([this, addr, session]() {
        pair_map_[addr] = session;
    });
}

void IceAgent::unregisterPair(const sockaddr_u& addr) {
    loop_->runInLoop([this, addr]() {
        pair_map_.erase(addr);
    });
}

void IceAgent::StunRequest(const StunMessage& req, const struct sockaddr* server, hio_t* io,
    std::function<void(StunMessage* resp, int code)> callback) {
    auto encoded = req.encode();
    send(encoded.data(), encoded.size(), server, io);
    if (callback) {
        auto* txn = new StunTransaction();
        txn->id = req.transactionId();
        txn->msg = std::move(encoded);
        txn->callback = callback;
        txn->agent = this;
        memcpy(&txn->destAddr, server, SOCKADDR_LEN(server));
        txn->io = io;
        txn->sentTime = hloop_now_ms(loop_->loop());
        txn->rto = 50; // RFC 5389 initial RTO (50ms for ICE checks)

        // Start retransmission timer (one-shot, rescheduled on each retransmit)
        // StunTransaction* itself is the htimer userdata
        htimer_t* timer = htimer_add(loop_->loop(), [](htimer_t* t) {
            auto* txn = (StunTransaction*)hevent_userdata(t);
            if (txn && txn->agent) {
                txn->agent->onStunRetransmit(txn);
            }
        }, txn->rto, 0);
        hevent_set_userdata(timer, txn);
        txn->timer = timer;

        transactions_[txn->id] = txn;
    }
}

void IceAgent::onStunRetransmit(StunTransaction* txn) {
    // Defensive: verify txn is still in map
    auto it = transactions_.find(txn->id);
    if (it == transactions_.end() || it->second != txn) return;
    auto id = TransactionIdStr(txn->id);
    // Check if maximum retransmissions exceeded (RFC 5389 Section 7.2.1)
    if (txn->retransmitCount >= MAX_RETRANSMIT) {
        char destStr[SOCKADDR_STRLEN] = {0};
        SOCKADDR_STR((struct sockaddr*)&txn->destAddr, destStr);
        hlogi("IceAgent onStunRetransmit %s: to %s timed out after %d retries", 
          id.c_str(), destStr, txn->retransmitCount);
        // Transaction timed out — notify callback with error
        if (txn->callback) {
            txn->callback(nullptr, -1); // code=-1 indicates timeout
        }
        transactions_.erase(it);
        delete txn; // ~StunTransaction() handles htimer_del
        return;
    }

    // Retransmit the STUN message
    send(txn->msg.data(), txn->msg.size(), &txn->destAddr.sa, txn->io);
    txn->retransmitCount++;
    hlogd("IceAgent onStunRetransmit %s: retransmit #%d rto=%u", id.c_str(), txn->retransmitCount, txn->rto);

    // Exponential backoff: RTO = min(RTO * 2, MAX_RTO)
    txn->rto = (std::min)(txn->rto * 2, MAX_RTO);

    // Delete old timer and schedule a new one-shot timer
    if (txn->timer) {
        htimer_del(txn->timer);
    }
    txn->timer = htimer_add(loop_->loop(), [](htimer_t* t) {
        auto* txn = (StunTransaction*)hevent_userdata(t);
        if (txn && txn->agent) {
            txn->agent->onStunRetransmit(txn);
        }
    }, txn->rto, 0);
    hevent_set_userdata(txn->timer, txn);
}

void IceAgent::processStunMsg(const uint8_t* data, size_t len, const struct sockaddr* addr, hio_t* io) {
    StunMessage msg;
    if (!StunMessage::decode(data, len, &msg)) return;

    char addrStr[SOCKADDR_STRLEN] = {0};
    SOCKADDR_STR(addr, addrStr);

    if (msg.cls() == STUN_CLASS_REQUEST)
    { // stun 请求 —— 必须有 USERNAME
        if (msg.getUsername().empty()) {
            hlogw("IceAgent processStunMsg: request from %s has no USERNAME, drop", addrStr);
            return ;
        }
        std::string ufrag, username = msg.getUsername();
        size_t colon = username.find(':');
        if (colon != std::string::npos) {
            ufrag = username.substr(0, colon);
        }
        if (!ufrag.empty()) {
            auto it = ufrag_map_.find(ufrag);
            if (it != ufrag_map_.end() && it->second) {
                hlogd("IceAgent processStunMsg: request from %s -> session ufrag=%s",
                      addrStr, ufrag.c_str());
                it->second->onStunRequest(msg, addr, io);
            } else {
                hlogw("IceAgent processStunMsg: request from %s ufrag=%s not found",
                      addrStr, ufrag.c_str());
            }
        }
    } else if (msg.cls() == STUN_CLASS_INDICATION) {
        // INDICATION (e.g. TURN DATA indication) —— 路由到 TCP session 或 pair_map_
        if (io && hio_type(io) == HIO_TYPE_TCP) {
            IceSession* session = (IceSession*)hio_context(io);
            if (session) {
                session->onStunRequest(msg, addr, io);
                return;
            }
        }
        // UDP: route via pair_map_
        auto pit = pair_map_.find(*(sockaddr_u*)addr);
        if (pit != pair_map_.end() && pit->second) {
            pit->second->onStunRequest(msg, addr, io);
        } else {
            hlogd("IceAgent processStunMsg: indication from %s no handler", addrStr);
        }
    } else { // stun响应
        auto it = transactions_.find(msg.transactionId());
        if (it != transactions_.end()) {
            hlogd("IceAgent processStunMsg: response from %s matched transaction", addrStr);
            StunTransaction* txn = it->second;
            if (txn->callback) {
                txn->callback(&msg, 0);
            }
            transactions_.erase(it);
            delete txn; // ~StunTransaction() handles htimer_del
        } else {
            hlogw("IceAgent processStunMsg: response from %s no matching transaction %s", 
              addrStr, TransactionIdStr(msg.transactionId()).c_str());
        }
    }
}

void IceAgent::onRecvPdu(const uint8_t* data, size_t len, const sockaddr* addr, hio_t* io) {
    PacketType ptype = classifyPacket(data, len);
    if (ptype == PacketType::STUN) {
        processStunMsg(data, len, addr, io);
    }
    else{
        auto it = pair_map_.find(*(sockaddr_u*)addr);
        if (it != pair_map_.end()) {
            it->second->onRecvData(data, len, addr);
        } else {
            char addrStr[SOCKADDR_STRLEN] = {0};
            SOCKADDR_STR(addr, addrStr);
            hlogw("IceAgent onRecvPdu: no session for data packet from %s len=%zu", addrStr, len);
        }
    }
}

std::string IceAgent::extractLocalUfrag(const uint8_t* data, size_t len) {
    if (len < 20) return "";

    size_t offset = 20; // skip header
    uint16_t msg_len = ((uint16_t)data[2] << 8) | data[3];
    size_t end = 20 + msg_len;
    if (end > len) end = len;

    while (offset + 4 <= end) {
        uint16_t attr_type = ((uint16_t)data[offset] << 8) | data[offset + 1];
        uint16_t attr_len = ((uint16_t)data[offset + 2] << 8) | data[offset + 3];

        if (attr_type == STUN_ATTR_USERNAME) {
            if (offset + 4 + attr_len > end) return "";
            std::string username((const char*)data + offset + 4, attr_len);
            size_t colon = username.find(':');
            if (colon != std::string::npos) {
                return username.substr(0, colon);
            }
            return username;
        }

        offset += 4 + ((attr_len + 3) & ~3);
    }
    return "";
}

// ---- TCP APIs ----

int IceAgent::connectTcp(const struct sockaddr* addr, IceSession* session) {
    hloop_t* loop = loop_->loop();
    if (!loop) return -1;

    char host[SOCKADDR_STRLEN] = {0};
    int port = 0;
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in* addr4 = (const struct sockaddr_in*)addr;
        inet_ntop(AF_INET, &addr4->sin_addr, host, sizeof(host));
        port = ntohs(addr4->sin_port);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6* addr6 = (const struct sockaddr_in6*)addr;
        inet_ntop(AF_INET6, &addr6->sin6_addr, host, sizeof(host));
        port = ntohs(addr6->sin6_port);
    }
    hlogi("IceAgent %s connectTcp %s:%d", session->id(), host, port);
    hio_t* io = hio_create_socket(loop, host, port, HIO_TYPE_TCP, HIO_CLIENT_SIDE);
    if (!io) return -1;

    uint32_t id = hio_id(io);
    hio_set_context(io, session);
    hevent_set_userdata(io, this);
    hio_setcb_connect(io, onTcpConnect);
    hio_setcb_read(io, onTcpRecv);
    hio_setcb_close(io, onTcpClose);
    hio_set_unpack(io, &tcp_unpack_setting_);
    hio_connect(io);

    return (int)id;
}

void IceAgent::closeTcpConnection(hio_t* io) {
    if (io) hio_close(io);
}

void IceAgent::onTcpAccept(hio_t* io) {
    IceAgent* self = (IceAgent*)hevent_userdata(io);
    if (!self) {
        hio_close(io);
        return;
    }

    uint32_t id = hio_id(io);
    self->tcp_connections_[id] = io;
    hio_set_context(io, nullptr);
    hevent_set_userdata(io, self);
    hio_setcb_read(io, onTcpRecv);
    hio_setcb_close(io, onTcpClose);
    hio_set_unpack(io, &self->tcp_unpack_setting_);
    hio_read(io);
}

void IceAgent::onTcpConnect(hio_t* io) {
    if (IceSession* session = (IceSession*)hio_context(io)) {
        session->onTcpConnected(io);
    }
    hio_read(io);
}

void IceAgent::onTcpClose(hio_t* io) {
    IceAgent* self = (IceAgent*)hevent_userdata(io);
    IceSession* session = (IceSession*)hio_context(io);
    if (session) {
        session->onTcpDisconnected(io);
    }
    if (self) {
        uint32_t id = hio_id(io);
        self->tcp_connections_.erase(id);
    }
}

void IceAgent::onTcpRecv(hio_t* io, void* buf, int readbytes) {
    IceAgent* self = (IceAgent*)hevent_userdata(io);
    if (!self) return;
    if (readbytes <= 2) return;
    self->handleTcpRecv(io, (const uint8_t*)buf + 2, readbytes - 2);
}

void IceAgent::handleTcpRecv(hio_t* io, const uint8_t* data, size_t len) {
    IceSession* session = (IceSession*)hio_context(io);    
    if (!session) {
        identifyTcpConnection(io, data, len);
        // identifyTcpConnection may bind this io to a session.
        session = (IceSession*)hio_context(io);
    }

    if (session) {
        PacketType ptype = classifyPacket(data, len);
        if (ptype == PacketType::STUN) {
            processStunMsg(data, len, hio_peeraddr(io), io);
        } else {
            session->onRecvData(data, len, hio_peeraddr(io));
        }
    }
}

void IceAgent::identifyTcpConnection(hio_t* io, const uint8_t* data, size_t len) {
    PacketType ptype = classifyPacket(data, len);
    if (ptype != PacketType::STUN) {
        hlogw("IceAgent identifyTcpConnection: first packet is not STUN, close io");
        hio_close(io);
        return;
    }
    if (len < 20) return;

    std::string local_ufrag = extractLocalUfrag(data, len);
    if (local_ufrag.empty()) {
        hlogw("IceAgent identifyTcpConnection: cannot extract ufrag, close io");
        return;
    }

    auto sit = ufrag_map_.find(local_ufrag);
    if (sit == ufrag_map_.end() || !sit->second) {
        hlogw("IceAgent identifyTcpConnection: ufrag=%s not found, close io", local_ufrag.c_str());
        hio_close(io);
        return;
    }

    if (sit->second->onTcpAccepted(io)){
        hio_set_context(io, sit->second);
        char peerStr[SOCKADDR_STRLEN] = {0};
        SOCKADDR_STR(hio_peeraddr(io), peerStr);
        hlogi("IceAgent identifyTcpConnection: peer=%s accepted -> session ufrag=%s",
               peerStr, local_ufrag.c_str());
    } else {
        hlogw("IceAgent identifyTcpConnection: session rejected TCP connection for ufrag=%s", local_ufrag.c_str());
        hio_close(io);
        return;
    }
}

// addLoacalIceCandidate helper api
void IceAgent::addHostCandidates(IceSession* session, int componentId) {
#ifdef _WIN32
    // Windows: Use GetAdaptersAddresses
    ULONG bufLen = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &bufLen);
    std::vector<uint8_t> buf(bufLen);
    PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)buf.data();
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, addrs, &bufLen) == NO_ERROR) {
        for (auto adapter = addrs; adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) continue;
            if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

            for (auto ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
                struct sockaddr* sa = ua->Address.lpSockaddr;
                if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)
                    continue;

                // Skip link-local IPv6
                if (sa->sa_family == AF_INET6) {
                    struct sockaddr_in6* sin6 = (struct sockaddr_in6*)sa;
                    if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr))
                        continue;
                }
                IceCandidate cand;
                cand.type = CandidateType::Host;
                cand.protocol = TransportProtocol::UDP;
                cand.componentId = 1;
                memcpy(&cand.addr, sa, SOCKADDR_LEN(sa));
                // Set port from agent
                sockaddr_set_port(&cand.addr, udp_port_);
                memcpy(&cand.baseAddr, &cand.addr, sizeof(sockaddr_u));
                cand.update();
                session->addLocalCandidate(cand);

                // TCP passive candidate (if TCP enabled)
                if (tcp_port_ > 0) {
                    IceCandidate tcpCand = cand;
                    tcpCand.protocol = TransportProtocol::TCP;
                    tcpCand.tcpType = TcpType::Passive;
                    sockaddr_set_port(&tcpCand.addr, tcp_port_);
                    memcpy(&tcpCand.baseAddr, &tcpCand.addr, sizeof(sockaddr_u));
                    tcpCand.update();

                    session->addLocalCandidate(tcpCand);
                }
            }
        }
    }
#else
    // Unix/Linux/macOS: Use getifaddrs
    struct ifaddrs* ifaddr;
    if (getifaddrs(&ifaddr) == -1) return;

    for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        struct sockaddr* sa = ifa->ifa_addr;
        if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6) continue;

        // Skip link-local IPv6
        if (sa->sa_family == AF_INET6) {
            struct sockaddr_in6* sin6 = (struct sockaddr_in6*)sa;
            if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) continue;
        }

        IceCandidate cand;
        cand.type = CandidateType::Host;
        cand.protocol = TransportProtocol::UDP;
        cand.componentId = componentId;
        memcpy(&cand.addr, sa, SOCKADDR_LEN(sa));
        sockaddr_set_port(&cand.addr, udp_port_);
        memcpy(&cand.baseAddr, &cand.addr, sizeof(sockaddr_u));
        cand.update();
        session->addLocalCandidate(cand);

        if (tcp_port_ > 0) {
            IceCandidate tcpCand = cand;
            tcpCand.protocol = TransportProtocol::TCP;
            tcpCand.tcpType = TcpType::Passive;
            sockaddr_set_port(&tcpCand.addr, tcp_port_);
            memcpy(&tcpCand.baseAddr, &tcpCand.addr, sizeof(sockaddr_u));
            tcpCand.update();
            session->addLocalCandidate(tcpCand);
        }
    }

    freeifaddrs(ifaddr);
#endif
}

void IceAgent::addTurnCandidates(IceSession* session, int componentId) {
    if (!turn_client_) return;

    auto serverAddr = turn_client_->serverAddr();

    auto addr = turn_client_->serverReflexiveAddr();
    if (sockaddr_port(&addr)) {
        IceCandidate cand;
        cand.type = CandidateType::ServerReflexive;
        cand.protocol = TransportProtocol::UDP;
        cand.componentId = componentId;
        memcpy(&cand.addr, &addr, SOCKADDR_LEN((struct sockaddr*)&addr));
        memcpy(&cand.baseAddr, udpLocalAddr(), SOCKADDR_LEN(udpLocalAddr()));
        memcpy(&cand.relatedAddr, &cand.baseAddr, sizeof(sockaddr_u));
        cand.update(serverAddr);
        session->addLocalCandidate(cand);
    }

    addr = turn_client_->relayAddr();
    if (sockaddr_port(&addr)) {
        IceCandidate cand;
        cand.type = CandidateType::Relay;
        cand.protocol = TransportProtocol::UDP;
        cand.componentId = componentId;
        memcpy(&cand.addr, &addr, sizeof(sockaddr_u));
        memcpy(&cand.baseAddr, udpLocalAddr(), SOCKADDR_LEN(udpLocalAddr()));
        memcpy(&cand.relatedAddr, &cand.baseAddr, sizeof(sockaddr_u));
        cand.update(serverAddr);
        session->addLocalCandidate(cand);
    }
}

// ---- TURN APIs ----

bool IceAgent::isTurnAllocated() const {
    return turn_client_ && turn_client_->isAllocated();
}

bool IceAgent::isTurnAllocating() const {
    return turn_client_ && turn_client_->state() == TurnState::Allocating;
}

void IceAgent::createTurnPermission(const struct sockaddr* peerAddr) {
    if (turn_client_ && turn_client_->isAllocated()) {
        turn_client_->createPermission(peerAddr);
    }
}

void IceAgent::allocateTurn() {
    if (turn_client_) return; // Already created

    if (config_.turnServers.empty()) return;

    for (const auto& server : config_.turnServers) {

        turn_client_ = std::make_shared<TurnClient>(loop_, server);

        // Route peer data received via TURN to the appropriate session
        turn_client_->onData = [this](const void* data, size_t len, const struct sockaddr* peerAddr) {
            onRecvPdu((const uint8_t*)data, len, (const sockaddr*)peerAddr, nullptr);
        };

        // Notify sessions when TURN state changes
        turn_client_->onStateChange = [this](TurnState state) {
            for (auto& session : sessions_) {
                session->onTurnStateChanged(state);
            }
        };

        turn_client_->allocate();
        break; // Only support one TURN server for now
    }
}

} // namespace ice
