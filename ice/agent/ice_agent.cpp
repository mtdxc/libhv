#include "ice_agent.h"
#include "../stun/stun_message.h"
#include "../stun/stun_auth.h"
#include "../session/ice_session.h"
#include "../turn/turn_client.h"
#include "hloop.h"
#include "hlog.h"

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

IceAgent::IceAgent() {
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
    loops_.start();
    hloop_t* loop = loops_.loop()->loop();
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
        if (kv.second) hio_close(kv.second);
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

    loops_.stop(true);
}

IceSessionPtr IceAgent::createSession(IceMode mode) {
    auto session = std::make_shared<IceSession>(mode, this, loops_.loop());
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    sessions_.push_back(session);
    return session;
}

void IceAgent::destroySession(const IceSessionPtr& session) {
    session->close();
    std::lock_guard<decltype(mutex_)> lock(mutex_);
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
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    ufrag_map_[ufrag] = session;
}

void IceAgent::unregisterSession(const std::string& ufrag) {
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    ufrag_map_.erase(ufrag);
}

void IceAgent::registerPair(const sockaddr_u& addr, IceSession* session) {
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    pair_map_[addr] = session;
}

void IceAgent::unregisterPair(const sockaddr_u& addr) {
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    pair_map_.erase(addr);
}

void dispatchData(IceSession* session, const uint8_t* data, size_t len, const struct sockaddr* addr, hio_t* io) {
    if (!session) return;
    if (session->loop()->isInLoopThread()) {
        session->onRecvData(data, len, addr, io);
    } else {
        std::string temp((const char*)data, len);
        sockaddr_u peerAddr;
        memcpy(&peerAddr.sa, addr, SOCKADDR_LEN(addr));
        std::weak_ptr<IceSession> weak_self = session->shared_from_this();
        session->loop()->runInLoop([weak_self, temp, peerAddr, io]() {
            if (auto session = weak_self.lock()) {
                session->onRecvData((const uint8_t*)temp.data(), temp.size(), &peerAddr.sa, io);
            }
        });
    }
}

void IceAgent::onRecvPdu(const uint8_t* data, size_t len, const sockaddr* addr, hio_t* io) {
    PacketType ptype = classifyPacket(data, len);
    auto it = pair_map_.find(*(sockaddr_u*)addr);
    if (it != pair_map_.end()) {
        auto session = it->second;
        dispatchData(session, data, len, addr, io);
    } else {
        auto session = findSession(data, len);
        if (session) {
            dispatchData(session, data, len, addr, io);
        } else {
            char addrStr[SOCKADDR_STRLEN] = {0};
            SOCKADDR_STR(addr, addrStr);
            hlogw("IceAgent onRecvPdu: no session for data packet from %s len=%zu", addrStr, len);
        }
    }
}

std::string IceAgent::extractLocalUfrag(const uint8_t* data, size_t len) {
    if (len < 20) return "";
    if (data[0] & 0xC0) return ""; // not STUN
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
    if (!session || !addr || !session->loop()) return -1;
    auto loop = session->loop();

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
    hio_t* io = hio_create_socket(loop->loop(), host, port, HIO_TYPE_TCP, HIO_CLIENT_SIDE);
    if (!io) return -1;

    uint32_t id = hio_id(io);
    tcp_connections_[id] = io;

    hevent_set_userdata(io, this);
    hio_set_context(io, session);
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
    std::lock_guard<decltype(self->mutex_)> lock(self->mutex_);
    self->tcp_connections_[id] = io;

    hevent_set_userdata(io, self);
    hio_set_context(io, nullptr);
    hio_setcb_read(io, onTcpRecv);
    hio_setcb_close(io, onTcpClose);
    hio_set_unpack(io, &self->tcp_unpack_setting_);
    hio_read(io);
}

void IceAgent::onTcpConnect(hio_t* io) {
    IceSession* session = (IceSession*)hio_context(io);
    if (session) {
        session->onTcpConnected(io);
    }
    hio_read(io);
}

void IceAgent::onTcpClose(hio_t* io) {
    IceAgent* self = (IceAgent*)hevent_userdata(io);
    if (self) {
        std::unique_lock<decltype(self->mutex_)> lock(self->mutex_);
        self->tcp_connections_.erase(hio_id(io));
    }

    IceSession* session = (IceSession*)hio_context(io);
    if (session) {
        session->onTcpDisconnected(io);
    }
}

void IceAgent::onTcpRecv(hio_t* io, void* buf, int readbytes) {
    if (readbytes <= 2) return;
    IceSession* session = (IceSession*)hio_context(io);
    if (session) {
        session->onRecvData((const uint8_t*)buf + 2, readbytes - 2, hio_peeraddr(io), io);
        return;
    }
    IceAgent* self = (IceAgent*)hevent_userdata(io);
    if (self) {
        self->identifyTcpConnection(io, (const uint8_t*)buf + 2, readbytes - 2);
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

    auto session = findSession(data, len);
    if (!session) {
        hlogw("IceAgent identifyTcpConnection: session not found, close io");
        hio_close(io);
        return;
    }

    char peerStr[SOCKADDR_STRLEN] = {0};
    SOCKADDR_STR(hio_peeraddr(io), peerStr);
    hlogi("IceAgent identifyTcpConnection: peer=%s identified -> session %s", peerStr, session->id());
    if (session->loop()->isInLoopThread()) {
        if (!session->onTcpAccepted(io)) return;
        hio_set_context(io, session);
        session->onRecvData(data, len, hio_peeraddr(io), io);
    } else {
        std::string temp((const char*)data, len);
        sockaddr_u peerAddr;
        memcpy(&peerAddr.sa, hio_peeraddr(io), SOCKADDR_LEN(hio_peeraddr(io)));
        hio_del(io);
        hio_detach(io); // detach io from current thread, will be used in session's loop
        std::weak_ptr<IceSession> weak_self = session->shared_from_this();
        session->loop()->runInLoop([weak_self, temp, peerAddr, io]() {
            if (auto session = weak_self.lock()) {
                if (!session->onTcpAccepted(io)) return;
                hio_attach(session->loop()->loop(), io);
                hio_read(io);
                hio_set_context(io, session.get());
                session->onRecvData((const uint8_t*)temp.data(), temp.size(), &peerAddr.sa, io);
            }
        });
    }
}

IceSession* IceAgent::findSession(const uint8_t* data, size_t len){
    std::string local_ufrag = extractLocalUfrag(data, len);
    if (local_ufrag.empty()) return nullptr;
    std::unique_lock<decltype(mutex_)> lock(mutex_);
    auto it = ufrag_map_.find(local_ufrag);
    if (it != ufrag_map_.end()) {
        return it->second;
    }
    return nullptr;
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

        turn_client_ = std::make_shared<TurnClient>(loops_.loop(), server);

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
