#include "ice_agent.h"
#include "../stun/stun_message.h"
#include "../stun/stun_auth.h"
#include "../session/ice_session.h"
#include "../turn/turn_client.h"
#include "hloop.h"
#include "hlog.h"
#include <algorithm>
#include "EventLoopThreadPool.h"

namespace ice {

static constexpr int MAX_RETRANSMIT = 4;  // ICE check: 4 retransmits (~1.5s max)
static constexpr uint32_t MAX_RTO = 800; // Cap RTO at 800ms
class StunTransaction : public std::enable_shared_from_this<StunTransaction> {
public:
    TransactionId id;
    StunCallback callback;    // Callback on response or timeout
    // retransmission
    std::vector<uint8_t> msg;  // encoded STUN message for retransmission
    sockaddr_u destAddr;      // destination address for retransmission
    hio_t* io = nullptr;      // IO handle for retransmission
    IceAgent* agent = nullptr;
    hv::EventLoop* loop = nullptr;

    uint64_t sentTime = 0;    // ms
    int retransmitCount = 0;
    uint32_t rto = 50;                        // Initial RTO ms (50ms for ICE checks)
    hv::TimerID timer = INVALID_TIMER_ID;     // Retransmit timer

    std::string IdStr() const { return TransactionIdStr(id); }
    void doCb(StunMessage* resp, int code);
    void setRto(uint32_t newRto);
    ~StunTransaction();
};

void StunTransaction::doCb(StunMessage* resp, int code) {
    if (!callback) return;
    auto cb = std::move(callback);
    if (!loop || loop->isInLoopThread()) {
        cb(resp, code);
        return;
    }
    else {
        StunMessage* respCopy = nullptr;
        if (resp) {
            respCopy = new StunMessage();
            *respCopy = *resp; // Copy the response message for use in the callback
        }
        // Post callback to EventLoop to ensure thread safety
        loop->runInLoop([respCopy, code, cb]() {
            cb(respCopy, code);
            delete respCopy;
        });
    }
}

void StunTransaction::setRto(uint32_t newRto) {
    rto = newRto;
    if (timer != INVALID_TIMER_ID) {
        loop->killTimer(timer);
        timer = INVALID_TIMER_ID;
    }
    std::weak_ptr<StunTransaction> weak_self = shared_from_this();
    timer = loop->setTimeout(rto, [weak_self](hv::TimerID tid) {
        if (auto self = weak_self.lock()) {
            if (self->agent) {
                self->timer = INVALID_TIMER_ID; // Clear timer ID to indicate no active timer
                self->agent->onStunRetransmit(self->id);
            }
        }
    });
}

StunTransaction::~StunTransaction() {
    if (timer!= INVALID_TIMER_ID && loop) {
        loop->killTimer(timer);
        timer = INVALID_TIMER_ID;
    }
}

/// IceAgent implementation
IceAgent::IceAgent(hv::ThreadPool* pool) {
    if (pool) {
        pools_ = pool;
        owns_pools_ = false;
    } else {
        auto pool = new hv::EventLoopThreadPool();
        pool->start();
        pools_ = pool;
        owns_pools_ = true;
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
    if (owns_pools_) {
        delete pools_;
        pools_ = nullptr;
    }
}

void IceAgent::setConfig(const IceConfig& config) {
    config_ = config;
}

int IceAgent::start() {
    std::unique_lock<decltype(mutex_)> lock(mutex_);
    if (running_) return 0;
    hloop_t* loop = pools_->loop()->loop();
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
    decltype(sessions_) sessions;
    std::vector<hio_t*> tcp_ios;
    std::shared_ptr<TurnClient> turn_client;
    hio_t* tcp_listen_io = nullptr;
    hio_t* udp_io = nullptr;

    {
        std::unique_lock<decltype(mutex_)> lock(mutex_);
        if (!running_) return;
        running_ = false;

        sessions = sessions_;
        sessions_.clear();

        turn_client = turn_client_;
        turn_client_.reset();

        for (auto& kv : tcp_connections_) {
            tcp_ios.push_back(kv.second);
        }
        tcp_connections_.clear();

        tcp_listen_io = tcp_listen_io_;
        tcp_listen_io_ = nullptr;
        udp_io = udp_io_;
        udp_io_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(route_mutex_);
        ufrag_map_.clear();
        pair_map_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(txn_mutex_);
        transactions_.clear();
    }

    hlogi("IceAgent stop with %zu sessions", sessions.size());
    for (auto& session : sessions) {
        session->close();
    }

    turn_client.reset();

    // Close TCP connections
    for (auto* io : tcp_ios) {
        if (io && nullptr == hio_context(io)) { // Prevent callbacks after close
            hio_close(io);
        }
    }

    if (tcp_listen_io) {
        hio_close(tcp_listen_io);
    }

    if (udp_io) {
        hio_close(udp_io);
    }

}

IceSessionPtr IceAgent::createSession() {
    auto session = std::make_shared<IceSession>(this, pools_->loop());
    std::unique_lock<decltype(mutex_)> lock(mutex_);
    sessions_.insert(session);
    return session;
}

void IceAgent::destroySession(const IceSessionPtr& session) {
    session->close();
    std::unique_lock<decltype(mutex_)> lock(mutex_);
    sessions_.erase(session);
}

std::shared_ptr<TurnClient> IceAgent::getTurnClient() const {
    std::unique_lock<decltype(mutex_)> lock(mutex_);
    return turn_client_;
}

// ---- UDP APIs ----
int IceAgent::send(const void* data, size_t len, const struct sockaddr* addr, hio_t* io) {
    int ret = -1;
    if (!io) {
        // sendViaRelay
        auto turn_client = getTurnClient();
        if (turn_client && turn_client->isAllocated())
            ret = turn_client->sendData(data, len, addr);
    } else if (hio_type(io) == HIO_TYPE_TCP) {
        uint8_t header[2];
        header[0] = (uint8_t)((len >> 8) & 0xFF);
        header[1] = (uint8_t)(len & 0xFF);
        hio_write(io, header, 2);
        ret = hio_write(io, data, len);
    } else {
        ret = hio_sendto(io, data, len, (struct sockaddr*)addr);
    }
    if (ret < 0) {
        char addrStr[SOCKADDR_STRLEN] = {0};
        if (addr) {
            SOCKADDR_STR(addr, addrStr);
        }
        hlogw("IceAgent send io=%p to %s return %d", io, addrStr, ret);
    }
    return ret;
}

std::shared_ptr<IceSession> IceAgent::findSessionByAddr(const struct sockaddr* addr) {
    std::lock_guard<std::mutex> lock(route_mutex_);
    auto it = pair_map_.find(*((sockaddr_u*)addr));
    if (it!= pair_map_.end()) {
        if (auto session = it->second.lock()) {
            return session;
        } else {
            // Session expired, remove from map
            pair_map_.erase(it);
        }
    }
    return nullptr;
}

std::shared_ptr<IceSession> IceAgent::findSessionByUfrag(const std::string& ufrag) {
    std::lock_guard<std::mutex> lock(route_mutex_);
    auto it = ufrag_map_.find(ufrag);
    if (it != ufrag_map_.end()) {
        if (auto session = it->second.lock()) {
            return session;
        } else {
            // Session expired, remove from map
            ufrag_map_.erase(it);
        }
    }
    return nullptr;
}

void IceAgent::registerSession(const std::string& ufrag, IceSession* session) {
    std::lock_guard<std::mutex> lock(route_mutex_);
    ufrag_map_[ufrag] = session->shared_from_this();
}

void IceAgent::unregisterSession(const std::string& ufrag) {
    std::lock_guard<std::mutex> lock(route_mutex_);
    ufrag_map_.erase(ufrag);
}

void IceAgent::registerPair(const sockaddr_u& addr, IceSession* session) {
    std::lock_guard<std::mutex> lock(route_mutex_);
    pair_map_[addr] = session->shared_from_this();
}

void IceAgent::unregisterPair(const sockaddr_u& addr) {
    std::lock_guard<std::mutex> lock(route_mutex_);
    pair_map_.erase(addr);
}

std::shared_ptr<StunTransaction> IceAgent::getTransaction(TransactionId id, bool pop) {
    std::shared_ptr<StunTransaction> ret;
    std::lock_guard<std::mutex> lock(txn_mutex_);
    auto it = transactions_.find(id);
    if (it != transactions_.end()) {
        ret = it->second;
        if (pop) transactions_.erase(it);
    }
    return ret;
}

void IceAgent::StunRequest(const StunMessage& req, const struct sockaddr* server, hio_t* io,
    std::function<void(StunMessage* resp, int code)> callback, hv::EventLoop* loop) {
    if (!loop) {
        loop = hv::tlsEventLoop();
        if (!loop) {
            hlogw("IceAgent skip StunRequest %s without loop", req.IdStr().c_str());
            return;
        }
    }
    auto encoded = req.encode();
    if (callback) {
        auto txn = std::make_shared<StunTransaction>();
        txn->id = req.transactionId();
        txn->msg = encoded;
        txn->callback = callback;
        txn->loop = loop;
        txn->agent = this;
        txn->io = io;
        memcpy(&txn->destAddr, server, SOCKADDR_LEN(server));
        txn->sentTime = hloop_now_ms(loop->loop());
        txn->setRto(50); // RFC 5389 initial RTO (50ms for ICE checks)
        std::lock_guard<std::mutex> lock(txn_mutex_);
        transactions_[txn->id] = txn;
    }
    send(encoded.data(), encoded.size(), server, io);
}

void IceAgent::onStunRetransmit(TransactionId tid) {
    std::shared_ptr<StunTransaction> txn;
    bool timeout = false;

    {
        // Serialize decision against response path.
        std::lock_guard<std::mutex> lock(txn_mutex_);
        auto it = transactions_.find(tid);
        if (it == transactions_.end()) return;
        txn = it->second;
        if (!txn) {
            transactions_.erase(it);
            return;
        }
        if (txn->retransmitCount >= MAX_RETRANSMIT) {
            transactions_.erase(it);
            timeout = true;
        }
    }

    auto id = txn->IdStr();
    if (timeout) {
        char destStr[SOCKADDR_STRLEN] = {0};
        SOCKADDR_STR((struct sockaddr*)&txn->destAddr, destStr);
        hlogi("IceAgent onStunRetransmit %s: to %s timed out after %d retries", 
          id.c_str(), destStr, txn->retransmitCount);
        txn->doCb(nullptr, -1);// code=-1 indicates timeout
    }
    else {
        // Retransmit the STUN message
        send(txn->msg.data(), txn->msg.size(), &txn->destAddr.sa, txn->io);
        txn->retransmitCount++;
        hlogd("IceAgent onStunRetransmit %s: retransmit #%d rto=%u", id.c_str(), txn->retransmitCount, txn->rto);

        // Exponential backoff: RTO = min(RTO * 2, MAX_RTO)
        txn->setRto((std::min)(txn->rto * 2, MAX_RTO));
    }
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
            auto session = findSessionByUfrag(ufrag);
            if (session) {
                hlogd("IceAgent processStunMsg: request from %s -> session ufrag=%s", addrStr, ufrag.c_str());
                session->onStunRequest(msg, addr, io);
            } else {
                hlogw("IceAgent processStunMsg: request from %s ufrag=%s not found", addrStr, ufrag.c_str());
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
        auto session = findSessionByAddr(addr);
        if (session) {
            session->onStunRequest(msg, addr, io);
        } else {
            hlogd("IceAgent processStunMsg: indication from %s no handler", addrStr);
        }
    } else { // stun响应
        auto txn = getTransaction(msg.transactionId(), true);
        if (txn) {
            txn->doCb(&msg, 0);
        } else {
            hlogw("IceAgent processStunMsg: response from %s no matching transaction %s", 
              addrStr, msg.IdStr().c_str());
        }
    }
}

void IceAgent::onRecvPdu(const uint8_t* data, size_t len, const sockaddr* addr, hio_t* io) {
    PacketType ptype = classifyPacket(data, len);
    if (ptype == PacketType::STUN) {
        processStunMsg(data, len, addr, io);
    }
    else{
        auto session = findSessionByAddr(addr);
        if (session) {
            session->onRecvData(data, len, addr);
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

hio_t* IceAgent::bindUdp(IceSession* session, uint16_t port) {
    auto loop = session->loop();
    if (!loop) return nullptr;

    hio_t* io = hio_create_socket(loop->loop(), nullptr, port, HIO_TYPE_UDP, HIO_SERVER_SIDE);
    if (!io) return nullptr;
    auto local_adr = (sockaddr_u*)hio_localaddr(io);
    hlogi("IceAgent %s bindUdp %d", session->id(), port);
    hio_set_context(io, session);
    hevent_set_userdata(io, this);
    hio_setcb_read(io, [](hio_t* io, void* buf, int len) {
        IceAgent* self = (IceAgent*)hevent_userdata(io);
        IceSession* session = (IceSession*)hio_context(io);
        if (!self || !session) return;
        const struct sockaddr* addr = hio_peeraddr(io);
        uint8_t* data = (uint8_t*)buf;
        PacketType ptype = classifyPacket(data, len);
        if (ptype == PacketType::STUN) {
            self->processStunMsg(data, len, addr, io);
        }
        else{
            session->onRecvData(data, len, addr);
        }
    });
    hio_read(io);
    return io;
}

// ---- TCP APIs ----

hio_t* IceAgent::connectTcp(const struct sockaddr* addr, IceSession* session) {
    auto loop = session->loop();
    if (!loop) return nullptr;

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
    if (!io) return nullptr;

    uint32_t id = hio_id(io);
    hio_set_context(io, session);
    hevent_set_userdata(io, this);
    hio_setcb_connect(io, onTcpConnect);
    hio_setcb_read(io, onTcpRecv);
    hio_setcb_close(io, onTcpClose);
    hio_set_unpack(io, &tcp_unpack_setting_);
    hio_connect(io);

    return io;
}

void IceAgent::closeIo(hio_t* io) {
    if (io) {
        hio_set_context(io, nullptr); // Prevent callbacks after close
        hevent_set_userdata(io, nullptr);
        hio_close(io);
    }
}

void IceAgent::onTcpAccept(hio_t* io) {
    IceAgent* self = (IceAgent*)hevent_userdata(io);
    if (!self) {
        hio_close(io);
        return;
    }

    uint32_t id = hio_id(io);
    {
        std::unique_lock<decltype(self->mutex_)> lock(self->mutex_);
        self->tcp_connections_[id] = io;
    }
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
        std::unique_lock<decltype(self->mutex_)> lock(self->mutex_);
        self->tcp_connections_.erase(id);
    }
}

void IceAgent::onTcpRecv(hio_t* io, void* buf, int readbytes) {
    IceAgent* self = (IceAgent*)hevent_userdata(io);
    if (!self) return;
    if (readbytes < 2) return;
    self->handleTcpRecv(io, (const uint8_t*)buf + 2, readbytes - 2);
}

void IceAgent::handleTcpRecv(hio_t* io, const uint8_t* data, size_t len) {
    IceSession* session = (IceSession*)hio_context(io);    
    if (session) {
        PacketType ptype = classifyPacket(data, len);
        if (ptype == PacketType::STUN) {
            processStunMsg(data, len, hio_peeraddr(io), io);
        } else {
            session->onRecvData(data, len, hio_peeraddr(io));
        }
    }
    else{
        identifyTcpConnection(io, data, len);
    }
}

void IceAgent::identifyTcpConnection(hio_t* io, const uint8_t* data, size_t len) {
    PacketType ptype = classifyPacket(data, len);
    if (ptype != PacketType::STUN) {
        hlogw("IceAgent identifyTcpConnection: first packet is not STUN, close io");
        hio_close(io);
        return;
    }

    StunMessage msg;
    if (!StunMessage::decode(data, len, &msg)) {
        return;
    }

    if (msg.cls() != STUN_CLASS_REQUEST || msg.getUsername().empty()) { 
        hlogw("IceAgent identifyTcpConnection: cannot extract ufrag, close io");
        return ;
    }

    std::string local_ufrag, username = msg.getUsername();
    size_t colon = username.find(':');
    if (colon != std::string::npos) {
        local_ufrag = username.substr(0, colon);
    }
    if (local_ufrag.empty()) {
        hlogw("IceAgent identifyTcpConnection: cannot extract ufrag, close io");
        return;
    }
    
    auto session = findSessionByUfrag(local_ufrag);
    if (!session) {
        hlogw("IceAgent identifyTcpConnection: session for ufrag=%s expired, close io", local_ufrag.c_str());
        hio_close(io);
        return;
    }
    if (session->onTcpAccepted(io)) {
        char peerStr[SOCKADDR_STRLEN] = {0};
        SOCKADDR_STR(hio_peeraddr(io), peerStr);
        hlogi("IceAgent identifyTcpConnection: peer=%s accepted -> session ufrag=%s",
                peerStr, session->localUfrag().c_str());
        // tcp连接迁移到session的EventLoop，确保后续STUN请求都在同一线程处理
        if (session->loop()->isInLoopThread()) {
            hio_set_context(io, session.get());
            session->onStunRequest(msg, hio_peeraddr(io), io);
        }
        else {
            hio_del(io); // Delay processing until we can safely attach to session's loop
            hio_detach(io); // Detach from current loop, will be attached to session's loop
            std::weak_ptr<IceSession> weak_self = session->shared_from_this();
            session->loop()->runInLoop([this, weak_self, io, msg]() {
                if (auto session = weak_self.lock()) {
                    hio_set_context(io, session.get());
                    hio_attach(session->loop()->loop(), io);
                    hio_read(io);
                    session->onStunRequest(msg, hio_peeraddr(io), io);
                }
                else{
                    hlogw("IceAgent identifyTcpConnection: session freed close io: %p", io);
                    hio_close(io);
                }
            });
        }
    } else {
        hlogw("IceAgent identifyTcpConnection: session rejected TCP connection for ufrag=%s", local_ufrag.c_str());
        hio_close(io);
    }
}

void IceAgent::addTurnCandidates(IceSession* session, int componentId) {
    auto turn_client = getTurnClient();
    if (!turn_client) return;

    auto serverAddr = turn_client->serverAddr();

    auto addr = turn_client->serverReflexiveAddr();
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

    addr = turn_client->relayAddr();
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
    auto turn_client = getTurnClient();
    return turn_client && turn_client->isAllocated();
}

bool IceAgent::isTurnAllocating() const {
    auto turn_client = getTurnClient();
    return turn_client && turn_client->state() == TurnState::Allocating;
}

void IceAgent::createTurnPermission(const struct sockaddr* peerAddr) {
    auto turn_client = getTurnClient();
    if (turn_client && turn_client->isAllocated()) {
        turn_client->createPermission(peerAddr);
    }
}

void IceAgent::allocateTurn() {
    std::unique_lock<decltype(mutex_)> lock(mutex_);
    if (turn_client_) return; // Already created

    if (config_.turnServers.empty()) return;

    for (const auto& server : config_.turnServers) {

        turn_client_ = std::make_shared<TurnClient>(pools_->loop(), server);

        // Route peer data received via TURN to the appropriate session
        turn_client_->onData = [this](const void* data, size_t len, const struct sockaddr* peerAddr) {
            onRecvPdu((const uint8_t*)data, len, (const sockaddr*)peerAddr, nullptr);
        };

        // Notify sessions when TURN state changes
        turn_client_->onStateChange = [this](TurnState state) {
            decltype(sessions_) sessions;
            {
                std::unique_lock<decltype(mutex_)> lock(mutex_);
                sessions = sessions_;
            }
            for (auto& session : sessions) {
                session->onTurnStateChanged(state);
            }
        };

        turn_client_->allocate();
        break; // Only support one TURN server for now
    }
}

} // namespace ice
