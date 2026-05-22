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
// TCP connection state for ICE
struct TcpIceConnection {
    hio_t* io = nullptr;
    IceSession* session = nullptr;
    bool identified = false; // true after first STUN exchange
};

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
        acceptor_loop_ = loop;
    } else {
        acceptor_loop_thread_.reset(new hv::EventLoopThread());
        acceptor_loop_ = acceptor_loop_thread_->loop();
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

    hloop_t* loop = acceptor_loop_->loop();
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

    // Start acceptor loop thread if we own it
    if (acceptor_loop_thread_ && !acceptor_loop_thread_->isRunning()) {
        acceptor_loop_thread_->start();
    }

    // Start worker thread pool
    if (config_.workerThreadNum > 0) {
        worker_pool_.reset(new hv::EventLoopThreadPool(config_.workerThreadNum));
        worker_pool_->start(true); // wait until all threads are running
    }

    hlogi("IceAgent start udpPort=%d tcpPort=%d workers=%d",
          udp_port_, tcp_port_, config_.workerThreadNum);
    running_ = true;
    return 0;
}

void IceAgent::stop() {
    if (!running_) return;
    running_ = false;
    hlogi("IceAgent stop with %zu sessions", sessions_.size());

    // Post close to each session's own worker loop
    for (auto& session : sessions_) {
        auto sloop = session->loop();
        sloop->runInLoop([session]() { session->close(); });
    }

    // Stop worker pool (waits for all worker loops to finish)
    if (worker_pool_) {
        worker_pool_->stop(true);
        worker_pool_.reset();
    }
    sessions_.clear();

    // Cleanup TURN client
    if (turn_client_) {
        turn_client_.reset();
    }

    // Close TCP connections that remain (unidentified, still in acceptor loop)
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
        delete kv.second; // ~StunTransaction() handles htimer_del
    }
    transactions_.clear();

    if (acceptor_loop_thread_) {
        acceptor_loop_thread_->stop(true);
    }
}

IceSessionPtr IceAgent::createSession(IceMode mode) {
    auto worker_loop = getWorkerLoop();
    auto session = std::make_shared<IceSession>(mode, this, worker_loop);
    sessions_.push_back(session);
    return session;
}

hv::EventLoopPtr IceAgent::getWorkerLoop() {
    if (worker_pool_ && worker_pool_->status() == hv::Status::kRunning) {
        return worker_pool_->nextLoop();
    }
    return acceptor_loop_;
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
    acceptor_loop_->runInLoop([this, ufrag, session]() {
        ufrag_map_[ufrag] = session;
    });
}

void IceAgent::unregisterSession(const std::string& ufrag) {
    acceptor_loop_->runInLoop([this, ufrag]() {
        ufrag_map_.erase(ufrag);
    });
}

void IceAgent::registerPair(const sockaddr_u& addr, IceSession* session) {
    acceptor_loop_->runInLoop([this, addr, session]() {
        pair_map_[addr] = session;
    });
}

void IceAgent::unregisterPair(const sockaddr_u& addr) {
    acceptor_loop_->runInLoop([this, addr]() {
        pair_map_.erase(addr);
    });
}

void IceAgent::StunRequest(const StunMessage& req, const struct sockaddr* server, hio_t* io,
    std::function<void(StunMessage* resp, int code)> callback,
    hv::EventLoopPtr callback_loop) {
    auto encoded = req.encode();
    send(encoded.data(), encoded.size(), server, io);
    if (callback) {
        // Wrap callback so it is dispatched to callback_loop if provided
        StunCallback wrapped_cb;
        if (callback_loop) {
            wrapped_cb = [callback, callback_loop](StunMessage* resp, int code) {
                if (callback_loop->isInLoopThread()) {
                    callback(resp, code);
                } else {
                    // resp points into a local StunTransaction that will be deleted after this call,
                    // so we must copy the message if non-null
                    std::shared_ptr<StunMessage> resp_copy;
                    if (resp) resp_copy = std::make_shared<StunMessage>(*resp);
                    callback_loop->runInLoop([callback, resp_copy, code]() mutable {
                        callback(resp_copy.get(), code);
                    });
                }
            };
        } else {
            wrapped_cb = callback;
        }

        auto* txn = new StunTransaction();
        txn->id = req.transactionId();
        txn->msg = std::move(encoded);
        txn->callback = std::move(wrapped_cb);
        txn->agent = this;
        memcpy(&txn->destAddr, server, SOCKADDR_LEN(server));
        txn->io = io;
        txn->sentTime = hloop_now_ms(acceptor_loop_->loop());
        txn->rto = 50; // RFC 5389 initial RTO (50ms for ICE checks)

        // Start retransmission timer (one-shot, rescheduled on each retransmit)
        // StunTransaction* itself is the htimer userdata
        htimer_t* timer = htimer_add(acceptor_loop_->loop(), [](htimer_t* t) {
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
    txn->timer = htimer_add(acceptor_loop_->loop(), [](htimer_t* t) {
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
                IceSession* sess = it->second;
                hlogd("IceAgent processStunMsg: request from %s -> session ufrag=%s",
                      addrStr, ufrag.c_str());
                // Dispatch to session's worker loop
                StunMessage msg_copy = msg;
                sockaddr_u from_copy;
                memcpy(&from_copy, addr, SOCKADDR_LEN(addr));
                sess->loop()->runInLoop([sess, msg_copy, from_copy, io]() mutable {
                    sess->onStunRequest(msg_copy, &from_copy.sa, io);
                });
            } else {
                hlogw("IceAgent processStunMsg: request from %s ufrag=%s not found",
                      addrStr, ufrag.c_str());
            }
        }
    } else if (msg.cls() == STUN_CLASS_INDICATION) {
        // INDICATION (e.g. TURN DATA indication) —— 路由到 TCP session 或 pair_map_
        if (io && hio_type(io) == HIO_TYPE_TCP) {
            uint32_t id = hio_id(io);
            auto cit = tcp_connections_.find(id);
            if (cit != tcp_connections_.end() && cit->second.session) {
                IceSession* sess = cit->second.session;
                StunMessage msg_copy = msg;
                sockaddr_u from_copy;
                memcpy(&from_copy, addr, SOCKADDR_LEN(addr));
                sess->loop()->runInLoop([sess, msg_copy, from_copy, io]() mutable {
                    sess->onStunRequest(msg_copy, &from_copy.sa, io);
                });
                return;
            }
        }
        // UDP: route via pair_map_
        auto pit = pair_map_.find(*(sockaddr_u*)addr);
        if (pit != pair_map_.end() && pit->second) {
            IceSession* sess = pit->second;
            StunMessage msg_copy = msg;
            sockaddr_u from_copy;
            memcpy(&from_copy, addr, SOCKADDR_LEN(addr));
            sess->loop()->runInLoop([sess, msg_copy, from_copy, io]() mutable {
                sess->onStunRequest(msg_copy, &from_copy.sa, io);
            });
        } else {
            hlogd("IceAgent processStunMsg: indication from %s no handler", addrStr);
        }
    } else { // stun响应 —— transactions_ 在 acceptor loop，直接处理
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
              addrStr, TransactionIdStr(msg.transactionId()));
        }
    }
}

void IceAgent::onRecvPdu(const uint8_t* data, size_t len, const sockaddr* addr, hio_t* io) {
    PacketType ptype = classifyPacket(data, len);
    if (ptype == PacketType::STUN) {
        processStunMsg(data, len, addr, io);
    } else {
        auto it = pair_map_.find(*(sockaddr_u*)addr);
        if (it != pair_map_.end() && it->second) {
            IceSession* sess = it->second;
            // Dispatch data to session's worker loop (copy required)
            std::vector<uint8_t> data_copy(data, data + len);
            sockaddr_u from_copy;
            memcpy(&from_copy, addr, SOCKADDR_LEN(addr));
            sess->loop()->runInLoop([sess, data_copy, from_copy]() {
                sess->onRecvData(data_copy.data(), data_copy.size(), &from_copy.sa);
            });
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

    // Create the outbound TCP socket directly on the session's worker loop —
    // userdata points to session, no migration needed.
    std::string host_str(host);
    auto worker_loop = session->loop();
    auto* unpack = &tcp_unpack_setting_;
    worker_loop->runInLoop([this, host_str, port, session, unpack]() {
        hloop_t* wloop = hv::tlsEventLoop() ? hv::tlsEventLoop()->loop() : acceptor_loop_->loop();
        hio_t* io = hio_create_socket(wloop, host_str.c_str(), port, HIO_TYPE_TCP, HIO_CLIENT_SIDE);
        if (!io) {
            hloge("IceAgent connectTcp: hio_create_socket failed for %s:%d", host_str.c_str(), port);
            return;
        }
        hevent_set_userdata(io, session);
        hio_setcb_connect(io, onSessionTcpConnect);
        hio_setcb_read(io, onSessionTcpRecv);
        hio_setcb_close(io, onSessionTcpClose);
        hio_set_unpack(io, unpack);
        hio_connect(io);
    });
    return 0;
}

void IceAgent::closeTcpConnection(hio_t* io) {
    if (io) hio_close(io);
}

// ---- Acceptor-loop TCP callbacks (unidentified phase) ----

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
    // Legacy path (acceptor-loop managed outbound): not used after refactoring,
    // kept for safety.
    IceAgent* self = (IceAgent*)hevent_userdata(io);
    if (!self) return;
    hio_read(io);
    uint32_t id = hio_id(io);
    auto it = self->tcp_connections_.find(id);
    if (it != self->tcp_connections_.end() && it->second.session) {
        it->second.session->onTcpConnected(io);
    }
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

void IceAgent::onTcpRecv(hio_t* io, void* buf, int readbytes) {
    IceAgent* self = (IceAgent*)hevent_userdata(io);
    if (!self) return;
    if (readbytes <= 2) return;
    self->handleTcpRecv(io, (const uint8_t*)buf + 2, readbytes - 2);
}

void IceAgent::handleTcpRecv(hio_t* io, const uint8_t* data, size_t len) {
    uint32_t id = hio_id(io);
    auto it = tcp_connections_.find(id);
    if (it == tcp_connections_.end()) return;

    if (!it->second.identified) {
        identifyTcpConnection(io, data, len);
        // After identification the io is removed from tcp_connections_ (migrated),
        // so do NOT continue using 'it'.
        return;
    }

    // Shouldn't normally reach here after migration; guard for robustness.
    if (it->second.session) {
        PacketType ptype = classifyPacket(data, len);
        if (ptype == PacketType::STUN) {
            processStunMsg(data, len, hio_peeraddr(io), io);
        } else {
            it->second.session->onRecvData(data, len, hio_peeraddr(io));
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

    IceSession* session = sit->second;
    char peerStr[SOCKADDR_STRLEN] = {0};
    SOCKADDR_STR(hio_peeraddr(io), peerStr);
    hlogi("IceAgent identifyTcpConnection: io=%u peer=%s -> session ufrag=%s",
          hio_id(io), peerStr, local_ufrag.c_str());

    // Migrate the io to the session's worker loop (removes from tcp_connections_)
    // Capture first-packet data before detach
    sockaddr_u from_copy;
    memcpy(&from_copy, hio_peeraddr(io), SOCKADDR_LEN(hio_peeraddr(io)));

    StunMessage msg;
    bool decoded = StunMessage::decode(data, len, &msg);

    migrateTcpToSession(io, session, [session, msg, from_copy, decoded](hio_t* io) mutable {
        // Called inside worker loop AFTER hio_attach — io is fully owned by worker loop now
        if (decoded) {
            session->onStunRequest(msg, &from_copy.sa, io);
        }
    });
}

void IceAgent::migrateTcpToSession(hio_t* io, IceSession* session,
                                   std::function<void(hio_t*)> on_attached) {
    // Must be called from acceptor loop thread.
    uint32_t id = hio_id(io);

    // 1. Remove from agent's tcp_connections_ (before detach)
    tcp_connections_.erase(id);

    // 2. Switch io callbacks: userdata -> session, use worker-loop trampolines
    hevent_set_userdata(io, session);
    hio_setcb_read(io, onSessionTcpRecv);
    hio_setcb_close(io, onSessionTcpClose);

    // 3. Detach from acceptor loop
    hio_detach(io);

    // 4. Attach to session's worker loop, then invoke on_attached before hio_read
    auto worker_loop = session->loop();
    worker_loop->runInLoop([io, worker_loop, on_attached]() {
        hio_attach(worker_loop->loop(), io);
        if (on_attached) on_attached(io); // io is fully owned by worker loop here
        hio_read(io);
    });
}

// ---- Worker-loop TCP trampolines (session-side) ----

void IceAgent::onSessionTcpConnect(hio_t* io) {
    IceSession* session = (IceSession*)hevent_userdata(io);
    if (!session) return;
    hio_read(io);
    session->onTcpConnected(io);
}

void IceAgent::onSessionTcpRecv(hio_t* io, void* buf, int readbytes) {
    IceSession* session = (IceSession*)hevent_userdata(io);
    if (!session || readbytes <= 2) return;

    const uint8_t* data = (const uint8_t*)buf + 2;
    size_t len = readbytes - 2;

    PacketType ptype = classifyPacket(data, len);
    if (ptype == PacketType::STUN) {
        StunMessage msg;
        if (!StunMessage::decode(data, len, &msg)) return;

        if (msg.cls() == STUN_CLASS_SUCCESS_RESPONSE || msg.cls() == STUN_CLASS_ERROR_RESPONSE) {
            // STUN responses: route to acceptor loop for transaction matching
            IceAgent* agent = session->agent();
            std::vector<uint8_t> data_copy(data, data + len);
            sockaddr_u peer_copy;
            memcpy(&peer_copy, hio_peeraddr(io), SOCKADDR_LEN(hio_peeraddr(io)));
            agent->acceptor_loop_->runInLoop([agent, data_copy, peer_copy, io]() mutable {
                agent->processStunMsg(data_copy.data(), data_copy.size(), &peer_copy.sa, io);
            });
        } else {
            // STUN REQUEST or INDICATION: handle directly in worker loop
            sockaddr_u peer_copy;
            memcpy(&peer_copy, hio_peeraddr(io), SOCKADDR_LEN(hio_peeraddr(io)));
            session->onStunRequest(msg, &peer_copy.sa, io);
        }
    } else {
        session->onRecvData(data, len, hio_peeraddr(io));
    }
}

void IceAgent::onSessionTcpClose(hio_t* io) {
    IceSession* session = (IceSession*)hevent_userdata(io);
    if (!session) return;
    session->onTcpDisconnected(io);
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

        turn_client_ = std::make_shared<TurnClient>(acceptor_loop_, server);

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
