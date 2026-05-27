#include "turn_server.h"

#include <cstring>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <algorithm>

#include "hloop.h"
#include "hbase.h"
#include "htime.h"
#include "hlog.h"
#include "../stun/stun_auth.h"

namespace ice {

// ────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────

// Format sockaddr as "ip:port" string used as allocation key
static std::string sockaddrToKey(const struct sockaddr* addr) {
    char buf[64] = {};
    if (addr->sa_family == AF_INET) {
        auto* a4 = reinterpret_cast<const struct sockaddr_in*>(addr);
        inet_ntop(AF_INET, &a4->sin_addr, buf, sizeof(buf));
        return std::string(buf) + ":" + std::to_string(ntohs(a4->sin_port));
    } else if (addr->sa_family == AF_INET6) {
        auto* a6 = reinterpret_cast<const struct sockaddr_in6*>(addr);
        inet_ntop(AF_INET6, &a6->sin6_addr, buf, sizeof(buf));
        return std::string(buf) + ":" + std::to_string(ntohs(a6->sin6_port));
    }
    return "unknown";
}

// ────────────────────────────────────────────────────────────
// TurnServer construction / destruction
// ────────────────────────────────────────────────────────────

TurnServer::TurnServer() {
}

TurnServer::~TurnServer() {
    stop();
}

void TurnServer::setOptions(const TurnServerOptions& opts) {
    opts_ = opts;
}

// ────────────────────────────────────────────────────────────
// start / stop
// ────────────────────────────────────────────────────────────

int TurnServer::start(int threadNum) {
    if (running_) return 0;

    if (threadNum > 0) {
        thread_pool_.setThreadNum(threadNum);
    }

    thread_pool_.start();
    hlogi("TurnServer: allocation worker pool started threads=%d", thread_pool_.threadNum());

    hloop_t* loop = thread_pool_.loop(0)->loop();
    if (!loop) return -1;

    if (opts_.udpPort > 0) {
        udp_io_ = hloop_create_udp_server(loop, opts_.bindHost.c_str(), opts_.udpPort);
        if (!udp_io_) {
            hloge("TurnServer: failed to bind UDP %s:%d", opts_.bindHost.c_str(), opts_.udpPort);
            return -1;
        }
        udp_port_ = sockaddr_port((sockaddr_u*)hio_localaddr(udp_io_));

        hio_setcb_read(udp_io_, [](hio_t* io, void* buf, int readbytes) {
            auto* self = static_cast<TurnServer*>(hio_context(io));
            if (self) {
                self->onRecvPdu(static_cast<const uint8_t*>(buf),
                                static_cast<size_t>(readbytes),
                                hio_peeraddr(io), io);
            }
        });
        hio_set_context(udp_io_, this);
        hio_read(udp_io_);
        hlogi("TurnServer: listening UDP %s:%d", opts_.bindHost.c_str(), udp_port_);
    }

    if (opts_.tcpPort > 0) {
        tcp_listen_io_ = hloop_create_tcp_server(loop, opts_.bindHost.c_str(), opts_.tcpPort, onTcpAccept);
        if (!tcp_listen_io_) {
            hloge("TurnServer: failed to bind TCP %s:%d", opts_.bindHost.c_str(), opts_.tcpPort);
            if (udp_io_) { hio_close(udp_io_); udp_io_ = nullptr; }
            return -2;
        }
        hevent_set_userdata(tcp_listen_io_, this);
        tcp_port_ = sockaddr_port((sockaddr_u*)hio_localaddr(tcp_listen_io_));
        hlogi("TurnServer: listening TCP %s:%d", opts_.bindHost.c_str(), tcp_port_);
    }

    running_ = true;
    for (int i = 0; i < thread_pool_.threadNum(); ++i) {
        startExpirySweep(thread_pool_.loop(i));
    }
    return 0;
}

void TurnServer::stop() {
    if (!running_) return;
    running_ = false;

    std::vector<std::shared_ptr<TurnAllocation>> toRemove;
    {
        std::lock_guard<std::mutex> lk(allocations_mutex_);
        for (auto& kv : allocations_) {
            toRemove.push_back(kv.second);
        }
        allocations_.clear();
    }
    for (auto& alloc : toRemove) {
        if (!alloc || !alloc->loop) continue;
        alloc->loop->runInLoop([this, alloc]() {
            removeAllocationInLoop(alloc);
        });
    }

    if (tcp_listen_io_) { 
        hio_close(tcp_listen_io_); 
        tcp_listen_io_ = nullptr; 
    }
    if (udp_io_) { 
        hio_close(udp_io_);
        udp_io_ = nullptr; 
    }

    thread_pool_.stop(true);
}

// ────────────────────────────────────────────────────────────
// Incoming UDP dispatch
// ────────────────────────────────────────────────────────────

void TurnServer::onRecvPdu(const uint8_t* data, size_t len,
                           const struct sockaddr* from, hio_t* io) {
    if (len < 4) return;

    PacketType ptype = classifyPacket(data, len);
    if (ptype == PacketType::STUN) {
        StunMessage msg;
        if (!StunMessage::decode(data, len, &msg)) {
            hlogw("TurnServer: failed to decode STUN from %s", sockaddrToKey(from).c_str());
            return;
        }
        uint16_t method = msg.method();
        uint16_t cls    = msg.cls();

        if (method == STUN_METHOD_BINDING && cls == STUN_CLASS_REQUEST) {
            handleBinding(msg, from, io);
        } else if (method == TURN_METHOD_ALLOCATE && cls == STUN_CLASS_REQUEST) {
            handleAllocate(msg, from, io);
        } else if (method == TURN_METHOD_REFRESH && cls == STUN_CLASS_REQUEST) {
            handleRefresh(msg, from, io);
        } else if (method == TURN_METHOD_CREATE_PERMISSION && cls == STUN_CLASS_REQUEST) {
            handleCreatePermission(msg, from, io);
        } else if (method == TURN_METHOD_CHANNEL_BIND && cls == STUN_CLASS_REQUEST) {
            handleChannelBind(msg, from, io);
        } else if (method == TURN_METHOD_SEND && cls == STUN_CLASS_INDICATION) {
            handleSendIndication(msg, from, io);
        } else {
            hlogw("TurnServer: unknown STUN method=0x%04x cls=0x%04x", method, cls);
        }
    } else if (ptype == PacketType::TURN_CHANNEL) {
        handleChannelData(data, len, from, io);
    } else {
        // Raw data arriving on main socket from a peer – unlikely, ignore
        hlogd("TurnServer: ignoring non-STUN/channel packet len=%zu", len);
    }
}

// ────────────────────────────────────────────────────────────
// STUN Binding
// ────────────────────────────────────────────────────────────

void TurnServer::handleBinding(const StunMessage& req,
                               const struct sockaddr* from, hio_t* io) {
    hlogd("TurnServer: handleBinding from %s", sockaddrToKey(from).c_str());
    StunMessage resp(STUN_METHOD_BINDING, STUN_CLASS_SUCCESS_RESPONSE);
    resp.setTransactionId(req.transactionId());
    resp.addXorMappedAddress(from);
    if (!opts_.software.empty()) resp.addSoftware(opts_.software);
    auto buf = resp.encode();
    sendTo(buf.data(), buf.size(), from, io);
}

// ────────────────────────────────────────────────────────────
// Authentication
// ────────────────────────────────────────────────────────────

/*static*/ std::string TurnServer::generateNonce() {
    // Simple random hex nonce
    std::string ret;
    char rand_bytes[3];
    for (int i =0 ; i<16; i++) {
        snprintf(rand_bytes, 3, "%02X", (uint8_t)(rand() & 0xFF));
        ret += rand_bytes;
    }
    return ret;
}

bool TurnServer::authenticate(const StunMessage& msg,
                              const struct sockaddr* from, hio_t* io,
                              std::string* outUsername) {
    // If no realm configured, skip auth
    if (opts_.realm.empty()) {
        if (outUsername) *outUsername = msg.getUsername();
        return true;
    }

    std::string username = msg.getUsername();
    std::string realm    = msg.getRealm();
    std::string nonce    = msg.getNonce();

    // Missing credentials -> send 401 with realm + fresh nonce
    if (username.empty() || realm.empty() || nonce.empty()) {
        std::string freshNonce = generateNonce();
        uint64_t expiry = gettimeofday_ms() + 600000; // 10 min
        nonces_[freshNonce] = expiry;

        hlogd("TurnServer: auth challenge 401 for client %s (no credentials)",
              sockaddrToKey(from).c_str());

        StunMessage err(msg.method(), STUN_CLASS_ERROR_RESPONSE);
        err.setTransactionId(msg.transactionId());
        err.addErrorCode(STUN_ERROR_UNAUTHORIZED, "Unauthorized");
        err.addRealm(opts_.realm);
        err.addNonce(freshNonce);
        if (!opts_.software.empty()) err.addSoftware(opts_.software);
        auto buf = err.encode();
        sendTo(buf.data(), buf.size(), from, io);
        return false;
    }

    // Realm mismatch
    if (realm != opts_.realm) {
        hlogw("TurnServer: auth failed realm mismatch from %s got=%s expected=%s",
              sockaddrToKey(from).c_str(), realm.c_str(), opts_.realm.c_str());
        sendError(msg, STUN_ERROR_UNAUTHORIZED, "Wrong realm", from, io);
        return false;
    }

    // Validate nonce (stale check)
    uint64_t now = gettimeofday_ms();
    auto nit = nonces_.find(nonce);
    if (nit == nonces_.end() || nit->second < now) {
        // Stale nonce – issue a new one
        std::string freshNonce = generateNonce();
        nonces_[freshNonce] = now + 600000;
        // Remove stale
        if (nit != nonces_.end()) nonces_.erase(nit);

        hlogw("TurnServer: auth stale nonce from %s", sockaddrToKey(from).c_str());

        StunMessage err(msg.method(), STUN_CLASS_ERROR_RESPONSE);
        err.setTransactionId(msg.transactionId());
        err.addErrorCode(STUN_ERROR_STALE_NONCE, "Stale Nonce");
        err.addRealm(opts_.realm);
        err.addNonce(freshNonce);
        auto buf = err.encode();
        sendTo(buf.data(), buf.size(), from, io);
        return false;
    }

    // Lookup password
    auto uit = opts_.users.find(username);
    if (uit == opts_.users.end()) {
        hlogw("TurnServer: auth failed unknown user=%s from %s",
              username.c_str(), sockaddrToKey(from).c_str());
        sendError(msg, STUN_ERROR_UNAUTHORIZED, "Unknown user", from, io);
        return false;
    }

    // Verify MESSAGE-INTEGRITY using long-term key: MD5(username:realm:password) as 16-byte binary
    if (!verifyLongTermAuth(msg, username, realm, uit->second)) {
        hlogw("TurnServer: auth failed bad credentials user=%s from %s",
              username.c_str(), sockaddrToKey(from).c_str());
        sendError(msg, STUN_ERROR_UNAUTHORIZED, "Bad credentials", from, io);
        return false;
    }

    // Do NOT consume nonce here – nonce is valid until it expires (time-based).
    // Consuming it one-time would break Refresh/CreatePermission/ChannelBind
    // which reuse the same nonce obtained from the initial 401 challenge.

    hlogd("TurnServer: auth success user=%s client=%s", username.c_str(), sockaddrToKey(from).c_str());

    if (outUsername) *outUsername = username;
    return true;
}

bool TurnServer::verifyLongTermAuth(const StunMessage& msg,
                                    const std::string& username,
                                    const std::string& realm,
                                    const std::string& password) const {
    return msg.verifyIntegrity(long_turn_auth_key(username, realm, password));
}

// ────────────────────────────────────────────────────────────
// ALLOCATE (RFC 5766 §6)
// ────────────────────────────────────────────────────────────

void TurnServer::handleAllocate(const StunMessage& req,
                                const struct sockaddr* from, hio_t* io) {
    std::string username;
    if (!authenticate(req, from, io, &username)) return;

    AllocKey key = makeKey(from, io);
    auto existing = findAllocation(key);
    if (existing) {
        hlogi("TurnServer: allocate refresh for existing client %s", key.c_str());
        StunMessage resp(TURN_METHOD_ALLOCATE, STUN_CLASS_SUCCESS_RESPONSE);
        resp.setTransactionId(req.transactionId());
        resp.addXorRelayedAddress(&existing->relayAddr.sa);
        resp.addXorMappedAddress(from);
        resp.addLifetime(existing->lifetime);
        if (!opts_.software.empty()) resp.addSoftware(opts_.software);
        auto buf = resp.encode();
        sendTo(buf.data(), buf.size(), from, io);
        return;
    }

    uint32_t lifetime = req.getLifetime();
    if (lifetime == 0 || lifetime > 3600) lifetime = 600;

    auto alloc = std::make_shared<TurnAllocation>();
    alloc->loop = thread_pool_.nextLoop();
    if (!alloc->loop) {
        sendError(req, STUN_ERROR_SERVER_ERROR, "No worker loop", from, io);
        return;
    }
    alloc->allocKey = key;
    memset(&alloc->clientAddr, 0, sizeof(alloc->clientAddr));
    memcpy(&alloc->clientAddr, from, SOCKADDR_LEN(from));
    alloc->clientIo = io;
    alloc->lifetime = lifetime;
    alloc->username = username;
    alloc->realm = opts_.realm;
    bool attach = false;
    if (hio_type(io) == HIO_TYPE_TCP && !alloc->loop->isInLoopThread()) {
        hio_detach(io);
        attach = true;
    }
    alloc->loop->runInLoop([this, alloc, req, lifetime, attach]() {
        if (attach) {
            hio_attach(alloc->loop->loop(), alloc->clientIo);
        }
        alloc->bindRelaySocket(opts_.bindHost, 0);
        if (!alloc->relayIo) {
            hloge("TurnServer: bindRelaySocket failed for client %s", alloc->allocKey.c_str());
            sendError(req, STUN_ERROR_INSUFFICIENT_CAPACITY, "No relay port available",
                      &alloc->clientAddr.sa, alloc->clientIo);
            return;
        }

        alloc->expireTime = hloop_now_ms(alloc->loop->loop()) + (uint64_t)lifetime * 1000;

        std::lock_guard<std::mutex> lk(allocations_mutex_);
        allocations_[alloc->allocKey] = alloc;

        if (onAllocationChanged) onAllocationChanged(*alloc, true);

        hlogd("TurnServer: allocated relay %s for client %s (lifetime=%us)",
              sockaddrToKey(&alloc->relayAddr.sa).c_str(), alloc->allocKey.c_str(), lifetime);

        StunMessage resp(TURN_METHOD_ALLOCATE, STUN_CLASS_SUCCESS_RESPONSE);
        resp.setTransactionId(req.transactionId());
        resp.addXorRelayedAddress(&alloc->relayAddr.sa);
        resp.addXorMappedAddress(&alloc->clientAddr.sa);
        resp.addLifetime(lifetime);
        if (!opts_.software.empty()) resp.addSoftware(opts_.software);
        auto buf = resp.encode();
        sendTo(buf.data(), buf.size(), &alloc->clientAddr.sa, alloc->clientIo);
    });

    return;
}

// ────────────────────────────────────────────────────────────
// REFRESH (RFC 5766 §7)
// ────────────────────────────────────────────────────────────

void TurnServer::handleRefresh(const StunMessage& req,
                               const struct sockaddr* from, hio_t* io) {
    std::string username;
    if (!authenticate(req, from, io, &username)) return;

    AllocKey key = makeKey(from, io);
    auto alloc = findAllocation(key);
    if (!alloc) {
        sendError(req, STUN_ERROR_BAD_REQUEST, "No allocation", from, io);
        return;
    }

    alloc->loop->runInLoop([this, alloc, req]() {
        uint32_t lifetime = req.getLifetime();
        if (lifetime == 0) {
            removeAllocationInLoop(alloc);
        } else {
            if (lifetime > 3600) lifetime = 600;
            alloc->lifetime = lifetime;
            alloc->expireTime = hloop_now_ms(alloc->loop->loop()) + (uint64_t)lifetime * 1000;
        }


        StunMessage resp(TURN_METHOD_REFRESH, STUN_CLASS_SUCCESS_RESPONSE);
        resp.setTransactionId(req.transactionId());
        resp.addLifetime(lifetime);
        auto buf = resp.encode();
        sendTo(buf.data(), buf.size(), &alloc->clientAddr.sa, alloc->clientIo);
    });
}

// ────────────────────────────────────────────────────────────
// CREATE PERMISSION (RFC 5766 §9)
// ────────────────────────────────────────────────────────────

void TurnServer::handleCreatePermission(const StunMessage& req,
                                        const struct sockaddr* from, hio_t* io) {
    std::string username;
    if (!authenticate(req, from, io, &username)) return;

    AllocKey key = makeKey(from, io);
    auto alloc = findAllocation(key);
    if (!alloc) {
        sendError(req, STUN_ERROR_BAD_REQUEST, "No allocation", from, io);
        return;
    }

    alloc->loop->runInLoop([this, alloc, req]() {
        uint64_t expiry = hloop_now_ms(alloc->loop->loop()) + 300000;
        for (const auto& attr : req.attributes()) {
            if (attr.type != STUN_ATTR_XOR_PEER_ADDRESS) continue;
            struct sockaddr_storage peer;
            if (!req.getXorPeerAddress(&peer)) continue;

            sockaddr_u permKey;
            memcpy(&permKey, &peer, SOCKADDR_LEN((struct sockaddr*)&peer));
            sockaddr_set_port(&permKey, 0); // permissions are port-independent

            alloc->permissions[permKey] = expiry;
            hlogd("TurnAllocation %s addPermission for peer %s", alloc->allocKey.c_str(), sockaddrToKey((struct sockaddr*)&peer).c_str());
        }
    });

    StunMessage resp(TURN_METHOD_CREATE_PERMISSION, STUN_CLASS_SUCCESS_RESPONSE);
    resp.setTransactionId(req.transactionId());
    auto buf = resp.encode();
    sendTo(buf.data(), buf.size(), from, io);
}

// ────────────────────────────────────────────────────────────
// CHANNEL-BIND (RFC 5766 §11)
// ────────────────────────────────────────────────────────────

void TurnServer::handleChannelBind(const StunMessage& req,
                                   const struct sockaddr* from, hio_t* io) {
    std::string username;
    if (!authenticate(req, from, io, &username)) return;

    AllocKey key = makeKey(from, io);
    auto alloc = findAllocation(key);
    if (!alloc) {
        sendError(req, STUN_ERROR_BAD_REQUEST, "No allocation", from, io);
        return;
    }

    uint16_t channel = req.getChannelNumber();
    if (channel < 0x4000 || channel > 0x7FFE) {
        sendError(req, STUN_ERROR_BAD_REQUEST, "Invalid channel number", from, io);
        return;
    }

    struct sockaddr_storage peer;
    if (!req.getXorPeerAddress(&peer)) {
        sendError(req, STUN_ERROR_BAD_REQUEST, "Missing XOR-PEER-ADDRESS", from, io);
        return;
    }

    auto peerAddr = std::make_shared<sockaddr_u>();
    memcpy(&peerAddr->sa, &peer, SOCKADDR_LEN((struct sockaddr*)&peer));

    alloc->addChannelBinding(channel, peerAddr);

    StunMessage resp(TURN_METHOD_CHANNEL_BIND, STUN_CLASS_SUCCESS_RESPONSE);
    resp.setTransactionId(req.transactionId());
    auto buf = resp.encode();
    sendTo(buf.data(), buf.size(), from, io);
}

// ────────────────────────────────────────────────────────────
// SEND indication (RFC 5766 §10)
// ────────────────────────────────────────────────────────────

void TurnServer::handleSendIndication(const StunMessage& req,
                                      const struct sockaddr* from, hio_t* io) {
    AllocKey key = makeKey(from, io);
    auto alloc = findAllocation(key);
    if (!alloc) return; // silently drop

    struct sockaddr_storage peer;
    if (!req.getXorPeerAddress(&peer)) return;

    const uint8_t* payload = nullptr;
    size_t         payloadLen = 0;
    if (!req.getData(&payload, &payloadLen) || !payload || payloadLen == 0) return;

    auto peerAddr = std::make_shared<sockaddr_u>();
    memcpy(&peerAddr->sa, &peer, SOCKADDR_LEN((struct sockaddr*)&peer));

    alloc->forwardData(payload, payloadLen, peerAddr);
}

// ────────────────────────────────────────────────────────────
// ChannelData (RFC 5766 §11.5)
// ────────────────────────────────────────────────────────────

void TurnServer::handleChannelData(const uint8_t* data, size_t len,
                                   const struct sockaddr* from, hio_t* io) {
    if (len < TURN_CHANNLE_HEAD_LEN) return;
    uint16_t channel  = read_be16(data);
    uint16_t dataLen  = read_be16(data + 2);
    if (TURN_CHANNLE_HEAD_LEN + dataLen > len) return;

    AllocKey key = makeKey(from, io);
    auto alloc = findAllocation(key);
    if (!alloc) return;

    auto cit = alloc->channels.find(channel);
    if (cit == alloc->channels.end()) return;

    alloc->forwardData(data + TURN_CHANNLE_HEAD_LEN, dataLen, cit->second.peerAddr);
}

// ────────────────────────────────────────────────────────────
// Response helpers
// ────────────────────────────────────────────────────────────

void TurnServer::sendSuccess(const StunMessage& req,
                             const struct sockaddr* to, hio_t* io) {
    StunMessage resp(req.method(), STUN_CLASS_SUCCESS_RESPONSE);
    resp.setTransactionId(req.transactionId());
    auto buf = resp.encode();
    sendTo(buf.data(), buf.size(), to, io);
}

void TurnServer::sendError(const StunMessage& req, uint16_t code,
                           const std::string& reason,
                           const struct sockaddr* to, hio_t* io,
                           bool addAuth) {
    StunMessage err(req.method(), STUN_CLASS_ERROR_RESPONSE);
    err.setTransactionId(req.transactionId());
    err.addErrorCode(code, reason);
    if (addAuth && !opts_.realm.empty()) {
        err.addRealm(opts_.realm);
        std::string nonce = generateNonce();
        nonces_[nonce] = gettimeofday_ms() + 600000;
        err.addNonce(nonce);
    }
    if (!opts_.software.empty()) err.addSoftware(opts_.software);
    auto buf = err.encode();
    sendTo(buf.data(), buf.size(), to, io);
}

int TurnServer::sendTo(const void* data, size_t len,
                       const struct sockaddr* to, hio_t* io) {
    if (!io) return -1;
    return hio_sendto(io, data, (int)len, (struct sockaddr*)to);
}

// ────────────────────────────────────────────────────────────
// Allocation management
// ────────────────────────────────────────────────────────────

TurnServer::AllocKey TurnServer::makeKey(const struct sockaddr* addr, hio_t* io) const {
    if (io && hio_type(io) == HIO_TYPE_TCP) {
        return "tcp:" + std::to_string(hio_fd(io));
    }
    return sockaddrToKey(addr);
}

std::shared_ptr<TurnAllocation> TurnServer::findAllocation(const AllocKey& key) {
    std::lock_guard<std::mutex> lk(allocations_mutex_);
    auto it = allocations_.find(key);
    return (it != allocations_.end()) ? it->second : nullptr;
}

std::list<std::shared_ptr<TurnAllocation>> TurnServer::getLoopAllocations(const hv::EventLoopPtr& loop) {
    std::list<std::shared_ptr<TurnAllocation>> result;
    if (!loop) return result;

    std::lock_guard<std::mutex> lk(allocations_mutex_);
    for (const auto& kv : allocations_) {
        const auto& alloc = kv.second;
        if (alloc && alloc->loop == loop) {
            result.push_back(alloc);
        }
    }
    return result;
}

void TurnServer::removeAllocation(const AllocKey& key) {
    auto alloc = findAllocation(key);
    if (!alloc) return;

    if (alloc->loop) {
        alloc->loop->runInLoop([this, alloc]() {
            removeAllocationInLoop(alloc);
        });
    } else {
        removeAllocationInLoop(alloc);
    }
}

void TurnServer::removeAllocationInLoop(const std::shared_ptr<TurnAllocation>& alloc) {
    if (!alloc) return;

    alloc->close();

    if (onAllocationChanged) onAllocationChanged(*alloc, false);

    hlogd("TurnServer: allocation removed for client=%s", alloc->allocKey.c_str());
    std::lock_guard<std::mutex> lk(allocations_mutex_);
    auto it = allocations_.find(alloc->allocKey);
    if (it != allocations_.end() && it->second.get() == alloc.get()) {
        allocations_.erase(it);
    }
}

void TurnServer::startExpirySweep(const hv::EventLoopPtr& loop) {
    if (!loop) return;

    loop->setTimeout(1000, [this, loop](hv::TimerID) {
        if (!running_) return;
        sweepExpiredAllocations(loop);
        startExpirySweep(loop);
    });
}


void TurnServer::sweepExpiredAllocations(const hv::EventLoopPtr& loop) {
    if (!running_ || !loop) return;

    auto allocs = getLoopAllocations(loop);
    const uint64_t now = hloop_now_ms(loop->loop());
    for (const auto& alloc : allocs) {
        if (!alloc) {
            continue;
        }

        if (alloc->expireTime != 0 && alloc->expireTime <= now) {
            hlogd("TurnServer: allocation expired for client=%s", alloc->allocKey.c_str());
            removeAllocationInLoop(alloc);
            continue;
        }

        for (auto it = alloc->permissions.begin(); it != alloc->permissions.end();) {
            if (it->second <= now) {
                it = alloc->permissions.erase(it);
            } else {
                ++it;
            }
        }

        for (auto it = alloc->channels.begin(); it != alloc->channels.end();) {
            if (it->second.expireTime <= now) {
                alloc->peerToChannel.erase(*it->second.peerAddr);
                it = alloc->channels.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ────────────────────────────────────────────────────────────
// Relay socket management
// ────────────────────────────────────────────────────────────
void TurnAllocation::forwardData(const void* payload, size_t payloadLen, std::shared_ptr<sockaddr_u> peer) {
    if (!loop->isInLoopThread()) {
        std::vector<uint8_t> payloadCopy((uint8_t*)payload, (uint8_t*)payload + payloadLen);
        std::weak_ptr<TurnAllocation> weakSelf = shared_from_this();
        loop->runInLoop([weakSelf, payloadCopy, peer]() {
            if (auto self = weakSelf.lock()) {
                self->forwardData(payloadCopy.data(), payloadCopy.size(), peer);
            }
        });
        return;
    }

    uint64_t now = hloop_now_ms(loop->loop());
    sockaddr_u permKey = *peer;
    sockaddr_set_port(&permKey, 0);
    auto pit = permissions.find(permKey);
    if (pit == permissions.end() || pit->second < now) {
        hlogd("TurnAllocation %s no permission for peer %s", allocKey.c_str(), sockaddrToKey(&peer->sa).c_str());
        return;
    }

    hlogd("TurnAllocation %s forwarding %zu bytes to peer %s", allocKey.c_str(), payloadLen, sockaddrToKey(&peer->sa).c_str());

    if (relayIo) {
        hio_sendto(relayIo, payload, (int)payloadLen, (struct sockaddr*)&peer->sa);
    }
}

// Packets arriving on a relay UDP socket (from remote peers)
void TurnAllocation::onRelayRecv(const void* data, size_t len, const struct sockaddr* from, hio_t* io) {

    // Check permission
    sockaddr_u peerAddr;
    memcpy(&peerAddr, from, SOCKADDR_LEN(from));

    // Permission is keyed by IP only (port-independent) per RFC 5766 §8
    // We store full address, so strip port for lookup
    sockaddr_u permKey;
    memcpy(&permKey, from, SOCKADDR_LEN(from));
    sockaddr_set_port(&permKey, 0);

    uint64_t now = hloop_now_ms(loop->loop());
    auto pit = permissions.find(permKey);
    if (pit == permissions.end() || pit->second < now) {
        hlogd("TurnAllocation %s without permission, dropping %zu bytes from peer=%s", allocKey.c_str(), len, sockaddrToKey(&peerAddr.sa).c_str());
        return;
    }

    // Check if there is a channel binding for this peer
    auto cit = peerToChannel.find(peerAddr);
    if (cit != peerToChannel.end()) {
        // Send ChannelData to client
        uint16_t ch = cit->second;
        size_t frameLen = TURN_CHANNLE_HEAD_LEN + len;
        std::vector<uint8_t> buf(frameLen);
        // channel
        buf[0] = (uint8_t)(ch >> 8);
        buf[1] = (uint8_t)(ch & 0xFF);
        // len
        buf[2] = (uint8_t)(len >> 8);
        buf[3] = (uint8_t)(len & 0xFF);
        memcpy(buf.data() + 4, data, len);

        // Pad to 4-byte boundary
        size_t padded = (buf.size() + 3) & ~3u;
        buf.resize(padded, 0);
        hio_sendto(clientIo, buf.data(), buf.size(), &clientAddr.sa);
    } else {
        // Send Data indication to client
        StunMessage ind(TURN_METHOD_DATA, STUN_CLASS_INDICATION);
        ind.addXorPeerAddress(from);
        ind.addData(data, len);
        auto encoded = ind.encode();
        hio_sendto(clientIo, encoded.data(), encoded.size(), &clientAddr.sa);
    }
}

void TurnAllocation::close() {
    if (relayIo) {
        hio_del(relayIo, HV_READ);
        hio_close(relayIo);
        relayIo = nullptr;
    }
}

void TurnAllocation::bindRelaySocket(std::string host, int port) {
    // Bind ephemeral UDP on all interfaces, port 0
    relayIo = hloop_create_udp_server(loop->loop(), host.c_str(), port);
    if (!relayIo) return;
    memcpy(&relayAddr, hio_localaddr(relayIo), SOCKADDR_LEN(hio_localaddr(relayIo)));
    hio_setcb_read(relayIo, [](hio_t* io, void* buf, int readbytes) {
        auto* alloc = static_cast<TurnAllocation*>(hio_context(io));
        if (alloc) {
            alloc->onRelayRecv(buf, readbytes, hio_peeraddr(io), io);
        }
    });
    hio_set_context(relayIo, this);
    hio_read(relayIo);
}

void TurnAllocation::addChannelBinding(uint16_t channel, std::shared_ptr<sockaddr_u> peerAddr) {
    if (!loop->isInLoopThread()) {
        std::weak_ptr<TurnAllocation> weakSelf = shared_from_this();
        loop->runInLoop([weakSelf, channel, peerAddr]() {
            if (auto self = weakSelf.lock()) {
                self->addChannelBinding(channel, peerAddr);
            }
        });
        return;
    }

    uint64_t expiry = hloop_now_ms(loop->loop()) + 600000;

    ChannelEntry entry;
    entry.channelNumber = channel;
    entry.peerAddr = peerAddr;
    entry.expireTime = expiry;

    channels[channel] = entry;
    peerToChannel[*peerAddr] = channel;

    sockaddr_u permKey = *peerAddr;
    sockaddr_set_port(&permKey, 0);
    permissions[permKey] = expiry;

    hlogd("TurnAllocation %s addChannel 0x%04x binding %s", allocKey.c_str(), channel, sockaddrToKey(&peerAddr->sa).c_str());
}

// ────────────────────────────────────────────────────────────
// TCP (skeleton)
// ────────────────────────────────────────────────────────────

/*static*/ void TurnServer::onTcpAccept(hio_t* io) {
    auto* self = static_cast<TurnServer*>(hevent_userdata(io));
    if (!self) return;

    char peerStr[SOCKADDR_STRLEN] = {0};
    SOCKADDR_STR(hio_peeraddr(io), peerStr);
    hlogi("TurnServer: TCP connection accepted from %s", peerStr);

    hio_setcb_read(io, onTcpRecv);
    hio_setcb_close(io, onTcpClose);
    hio_set_context(io, self);
    // for segment tcp message
    hevent_set_userdata(io, new std::string());
    hio_read(io);
}

/*static*/ void TurnServer::onTcpRecv(hio_t* io, void* buf, int readbytes) {
    auto* self = static_cast<TurnServer*>(hio_context(io));
    if (!self) return;
    auto str = (std::string*)hevent_userdata(io);
    if (!str) {
        return;
    }
    auto* peerAddr = hio_peeraddr(io);
    str->append((const char*)buf, readbytes);
    int len = str->length();
    auto p = (const uint8_t*)str->data();
    while (len >= 4) {
        int padding = 0;
        int plen = TurnTcpLength(p, len, &padding);
        if (plen + padding > len) {
            break;
        }
        self->onRecvPdu(p, plen, peerAddr, io);
        p = p + plen + padding;
        len = len - plen - padding;
    }
    if (len != str->length()) {
        if (len) {
            memcpy((void*)str->data(), p, len);
        }
        str->resize(len);
    }
}

/*static*/ void TurnServer::onTcpClose(hio_t* io) {
    auto str = (std::string*)hevent_userdata(io);
    if (str) {
        hevent_set_userdata(io, nullptr);
        delete str;
    }
    auto* self = static_cast<TurnServer*>(hio_context(io));
    if (!self) return;
    AllocKey key = self->makeKey(hio_peeraddr(io), io);
    hlogi("TurnServer: TCP connection closed, removing allocation client=%s", key.c_str());
    self->removeAllocation(key);
}

} // namespace ice
