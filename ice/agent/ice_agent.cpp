#include "ice_agent.h"
#include "../stun/stun_message.h"
#include "../stun/stun_auth.h"
#include "../session/ice_session.h"
#include "../turn/turn_client.h"
#include "hloop.h"

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
            self->onUdpRecv((const uint8_t*)buf, readbytes, addr);
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

    running_ = true;
    return 0;
}

void IceAgent::stop() {
    if (!running_) return;
    running_ = false;

    if (loop_thread_) {
        loop_thread_->stop(true);
    }

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
        if (kv.second.io) hio_close(kv.second.io);
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
        if (kv.second.timer) {
            htimer_del(kv.second.timer);
        }
    }
    transactions_.clear();

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

int IceAgent::sendTo(const void* data, size_t len, const struct sockaddr* addr) {
    if (!udp_io_) return -1;
    return hio_sendto(udp_io_, data, len, (struct sockaddr*)addr);
}

void IceAgent::registerSession(const std::string& ufrag, IDataRecv* session) {
    loop_->runInLoop([this, ufrag, session]() {
        ufrag_map_[ufrag] = session;
    });
}

void IceAgent::unregisterSession(const std::string& ufrag) {
    loop_->runInLoop([this, ufrag]() {
        ufrag_map_.erase(ufrag);
    });
}

void IceAgent::registerPair(const sockaddr_u& addr, IDataRecv* session) {
    loop_->runInLoop([this, addr, session]() {
        pair_map_[addr] = session;
    });
}

void IceAgent::unregisterPair(const sockaddr_u& addr) {
    loop_->runInLoop([this, addr]() {
        pair_map_.erase(addr);
    });
}

void IceAgent::StunRequest(const StunMessage& req, const struct sockaddr* server, std::function<void(StunMessage* resp, int code)> callback) {
    auto msg = req.encode();
    sendTo(msg.data(), msg.size(), server);
    if (callback) {
        addTransaction(req.transactionId(), callback);
    }
}

void IceAgent::addTransaction(const TransactionId& txnId, StunCallback callback) {
    if (callback) {
        StunTransaction txn;
        txn.sentTime = hloop_now_ms(loop_->loop());
        txn.callback = callback;
        // Start retransmission timer
        htimer_t* timer = htimer_add(loop_->loop(), [](htimer_t* t) {
            IceAgent* self = (IceAgent*)hevent_userdata(t);
            if (self) {
                //self->onStunRequestTimeout(t);
            }
        }, txn.rto, 0);
        hevent_set_userdata(timer, this);
        txn.timer = timer;
        transactions_[txnId] = txn;
    }
}

void IceAgent::processStunMsg(const uint8_t* data, size_t len, const struct sockaddr* addr) {
    StunMessage msg;
    if (!StunMessage::decode(data, len, &msg)) return;

    if (msg.cls() == STUN_CLASS_REQUEST || msg.cls() == STUN_CLASS_INDICATION) 
    { // stun 请求
        if (msg.getUsername().empty()) {
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
                it->second->onRecvData(data, len, addr);
                return;
            }
        }
    } else { // stun响应
        auto it = transactions_.find(msg.transactionId());
        if (it != transactions_.end()) {
            StunTransaction& txn = it->second;
            if (txn.callback) {
                txn.callback(&msg, 0);
            }
            if (txn.timer) {
                htimer_del(txn.timer);
            }
            transactions_.erase(it);
        }
    }
}

void IceAgent::onUdpRecv(const uint8_t* data, size_t len, const sockaddr* addr) {
    PacketType ptype = classifyPacket(data, len);
    auto it = pair_map_.find(*(sockaddr_u*)addr);
    if (it != pair_map_.end()) {
        it->second->onRecvData(data, len, addr);
#if 1
    } else if(ptype == PacketType::STUN) {
        processStunMsg(data, len, addr);
#else
    } else {
        uint16_t msg_type = ((uint16_t)data[0] << 8) | data[1];
        uint16_t cls = stun_get_class(msg_type);
        if (cls == STUN_CLASS_REQUEST || cls == STUN_CLASS_INDICATION) {
            std::string ufrag = extractLocalUfrag(data, len);
            if (!ufrag.empty()) {
                auto it = ufrag_map_.find(ufrag);
                if (it != ufrag_map_.end() && it->second) {
                    it->second->onRecvData(data, len, addr);
                    return;
                }
            }
        }
        // fallback
        for (auto& kv : ufrag_map_) {
            if (kv.second) {
                kv.second->onRecvData(data, len, addr);
                return;
            }
        }
#endif
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

int IceAgent::connectTcp(const struct sockaddr* addr, IDataRecv* session) {
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

    hio_t* io = hio_create_socket(loop, host, port, HIO_TYPE_TCP, HIO_CLIENT_SIDE);
    if (!io) return -1;

    TcpIceConnection conn;
    conn.io = io;
    conn.session = session;
    conn.identified = true;
    uint32_t id = hio_id(io);
    tcp_connections_[id] = conn;

    hevent_set_userdata(io, this);
    hio_setcb_connect(io, onTcpConnect);
    hio_setcb_read(io, onTcpRecv);
    hio_setcb_close(io, onTcpClose);
    hio_set_unpack(io, &tcp_unpack_setting_);
    hio_connect(io);

    return (int)id;
}

int IceAgent::send(hio_t* io, const void* data, size_t len) {
    if (!io) return -1;
    if (hio_type(io) == HIO_TYPE_TCP) {
        uint8_t header[2];
        header[0] = (uint8_t)((len >> 8) & 0xFF);
        header[1] = (uint8_t)(len & 0xFF);
        hio_write(io, header, 2);
    }
    return hio_write(io, data, len);
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

    TcpIceConnection conn;
    conn.io = io;
    conn.session = nullptr;
    conn.identified = false;
    uint32_t id = hio_id(io);
    self->tcp_connections_[id] = conn;

    hevent_set_userdata(io, self);
    hio_setcb_read(io, onTcpRecv);
    hio_setcb_close(io, onTcpClose);
    hio_set_unpack(io, &self->tcp_unpack_setting_);
    hio_read(io);
}

void IceAgent::onTcpConnect(hio_t* io) {
    IceAgent* self = (IceAgent*)hevent_userdata(io);
    if (!self) return;

    hio_read(io);

    uint32_t id = hio_id(io);
    auto it = self->tcp_connections_.find(id);
    if (it != self->tcp_connections_.end() && it->second.session) {
        it->second.session->onTcpConnected(io);
    }
}

void IceAgent::onTcpRecv(hio_t* io, void* buf, int readbytes) {
    IceAgent* self = (IceAgent*)hevent_userdata(io);
    if (!self) return;
    if (readbytes <= 2) return;
    self->handleTcpRecv(io, (const uint8_t*)buf + 2, readbytes - 2);
}

void IceAgent::onTcpClose(hio_t* io) {
    IceAgent* self = (IceAgent*)hevent_userdata(io);
    if (!self) return;

    uint32_t id = hio_id(io);
    auto it = self->tcp_connections_.find(id);
    if (it != self->tcp_connections_.end()) {
        if (it->second.session) {
            it->second.session->onTcpDisconnected(io);
        }
        self->tcp_connections_.erase(it);
    }
}

void IceAgent::handleTcpRecv(hio_t* io, const uint8_t* data, size_t len) {
    uint32_t id = hio_id(io);
    auto it = tcp_connections_.find(id);
    if (it == tcp_connections_.end()) return;

    if (!it->second.identified) {
        identifyTcpConnection(io, data, len);
        return;
    }

    if (it->second.session) {
        it->second.session->onRecvData(data, len, hio_peeraddr(io));
    }
}

void IceAgent::identifyTcpConnection(hio_t* io, const uint8_t* data, size_t len) {
    PacketType ptype = classifyPacket(data, len);
    if (ptype != PacketType::STUN) {
        hio_close(io);
        return;
    }

    if (len < 20) return;

    std::string local_ufrag = extractLocalUfrag(data, len);
    if (local_ufrag.empty()) return;

    auto sit = ufrag_map_.find(local_ufrag);
    if (sit == ufrag_map_.end() || !sit->second) {
        hio_close(io);
        return;
    }

    uint32_t id = hio_id(io);
    auto it = tcp_connections_.find(id);
    if (it != tcp_connections_.end()) {
        it->second.session = sit->second;
        it->second.ufrag = local_ufrag;
        it->second.identified = true;
    }

    sit->second->onRecvData(data, len, (const sockaddr*)hio_peeraddr(io));
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

    auto addr = turn_client_->serverAddr();
    char serverAddr[SOCKADDR_STRLEN] = {0};
    SOCKADDR_STR(&addr, serverAddr);

    addr = turn_client_->serverReflexiveAddr();
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

int IceAgent::sendViaRelay(const void* data, size_t len, const struct sockaddr* peer) {
    int ret = -1;
    if (turn_client_ && turn_client_->isAllocated()) {
        ret = turn_client_->sendData(data, len, peer);
    }
    return ret;
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
        if (server.protocol != TurnServerConfig::UDP) continue;

        sockaddr_u addr;
        if (!server.addr.toSockaddr(&addr)) continue;

        turn_client_ = std::make_shared<TurnClient>(loop_, this);
        turn_client_->setServer(&addr.sa);
        turn_client_->setCredentials(server.username, server.password);

        // Route peer data received via TURN to the appropriate session
        turn_client_->onData = [this](const void* data, size_t len, const struct sockaddr* peerAddr) {
            onUdpRecv((const uint8_t*)data, len, (const sockaddr*)peerAddr);
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
