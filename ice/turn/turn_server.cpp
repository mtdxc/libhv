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

TurnServer::TurnServer(hv::EventLoopPtr loop) {
    if (loop) {
        loop_ = loop;
    } else {
        loop_thread_.reset(new hv::EventLoopThread());
        loop_ = loop_thread_->loop();
    }

    // RFC 4571 framing for TCP (same as ICE agent)
    memset(&tcp_unpack_setting_, 0, sizeof(unpack_setting_t));
    tcp_unpack_setting_.mode               = UNPACK_BY_LENGTH_FIELD;
    tcp_unpack_setting_.package_max_length = DEFAULT_PACKAGE_MAX_LENGTH;
    tcp_unpack_setting_.body_offset        = 2;
    tcp_unpack_setting_.length_field_offset = 0;
    tcp_unpack_setting_.length_field_bytes  = 2;
    tcp_unpack_setting_.length_field_coding = ENCODE_BY_BIG_ENDIAN;
    tcp_unpack_setting_.length_adjustment   = 0;
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

int TurnServer::start() {
    if (running_) return 0;

    hloop_t* loop = loop_->loop();
    if (!loop) return -1;

    if (opts_.enableUdp) {
        udp_io_ = hloop_create_udp_server(loop, opts_.bindHost.c_str(), opts_.udpPort);
        if (!udp_io_) {
            hloge("TurnServer: failed to bind UDP %s:%d", opts_.bindHost.c_str(), opts_.udpPort);
            return -1;
        }
        udp_port_ = sockaddr_port((sockaddr_u*)hio_localaddr(udp_io_));

        hio_setcb_read(udp_io_, [](hio_t* io, void* buf, int readbytes) {
            auto* self = static_cast<TurnServer*>(hio_context(io));
            if (self) {
                self->onUdpRecv(static_cast<const uint8_t*>(buf),
                                static_cast<size_t>(readbytes),
                                hio_peeraddr(io), io);
            }
        });
        hio_set_context(udp_io_, this);
        hio_read(udp_io_);
        hlogi("TurnServer: listening UDP %s:%d", opts_.bindHost.c_str(), udp_port_);
    }

    if (opts_.enableTcp) {
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

    if (loop_thread_ && !loop_thread_->isRunning()) {
        loop_thread_->start();
    }

    running_ = true;
    return 0;
}

void TurnServer::stop() {
    if (!running_) return;
    running_ = false;

    // Cancel all expiry timers
    for (auto& kv : expiry_timers_) {
        if (kv.second) htimer_del(kv.second);
    }
    expiry_timers_.clear();

    // Close all relay sockets
    for (auto& kv : allocations_) {
        if (kv.second.relayIo) {
            hio_del(kv.second.relayIo, HV_READ);
            hio_close(kv.second.relayIo);
        }
    }
    allocations_.clear();
    relay_io_map_.clear();

    if (tcp_listen_io_) { hio_close(tcp_listen_io_); tcp_listen_io_ = nullptr; }
    if (udp_io_)        { hio_close(udp_io_);         udp_io_        = nullptr; }

    if (loop_thread_) {
        loop_thread_->stop(true);
    }
}

// ────────────────────────────────────────────────────────────
// Incoming UDP dispatch
// ────────────────────────────────────────────────────────────

void TurnServer::onUdpRecv(const uint8_t* data, size_t len,
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

// Packets arriving on a relay UDP socket (from remote peers)
void TurnServer::onRelayRecv(const uint8_t* data, size_t len,
                             const struct sockaddr* from, hio_t* relay_io) {
    TurnAllocation* alloc = findAllocationByRelay(relay_io);
    if (!alloc) return;

    // Check permission
    sockaddr_u peerAddr;
    memcpy(&peerAddr, from, SOCKADDR_LEN(from));
    // Permission is keyed by IP only (port-independent) per RFC 5766 §8
    // We store full address, so strip port for lookup
    sockaddr_u permKey;
    memcpy(&permKey, from, SOCKADDR_LEN(from));
    if (permKey.sa.sa_family == AF_INET)        permKey.sin.sin_port = 0;
    else if (permKey.sa.sa_family == AF_INET6)  permKey.sin6.sin6_port = 0;

    uint64_t now = hloop_now_ms(loop_->loop());
    auto pit = alloc->permissions.find(permKey);
    if (pit == alloc->permissions.end() || pit->second < now) {
        hlogd("TurnServer: relay recv from peer with no permission, dropping");
        return;
    }

    // Check if there is a channel binding for this peer
    auto cit = alloc->peerToChannel.find(peerAddr);
    if (cit != alloc->peerToChannel.end()) {
        // Send ChannelData to client
        uint16_t ch = cit->second;
        size_t   frameLen = 4 + len;
        std::vector<uint8_t> buf(frameLen);
        buf[0] = (uint8_t)(ch >> 8);
        buf[1] = (uint8_t)(ch & 0xFF);
        buf[2] = (uint8_t)(len >> 8);
        buf[3] = (uint8_t)(len & 0xFF);
        memcpy(buf.data() + 4, data, len);
        // Pad to 4-byte boundary
        size_t padded = (buf.size() + 3) & ~3u;
        buf.resize(padded, 0);
        sendTo(buf.data(), buf.size(), &alloc->clientAddr.sa, alloc->clientIo);
    } else {
        // Send Data indication to client
        StunMessage ind(TURN_METHOD_DATA, STUN_CLASS_INDICATION);
        ind.addXorPeerAddress(from);
        ind.addData(data, len);
        auto encoded = ind.encode();
        sendTo(encoded.data(), encoded.size(), &alloc->clientAddr.sa, alloc->clientIo);
    }
}

// ────────────────────────────────────────────────────────────
// STUN Binding
// ────────────────────────────────────────────────────────────

void TurnServer::handleBinding(const StunMessage& req,
                               const struct sockaddr* from, hio_t* io) {
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
    uint8_t rand_bytes[16];
    for (auto& b : rand_bytes) b = (uint8_t)(rand() & 0xFF);
    std::ostringstream oss;
    for (auto b : rand_bytes) oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    return oss.str();
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
        uint64_t expiry = hloop_now_ms(loop_->loop()) + 600000; // 10 min
        nonces_[freshNonce] = expiry;

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
        sendError(msg, STUN_ERROR_UNAUTHORIZED, "Wrong realm", from, io);
        return false;
    }

    // Validate nonce (stale check)
    uint64_t now = hloop_now_ms(loop_->loop());
    auto nit = nonces_.find(nonce);
    if (nit == nonces_.end() || nit->second < now) {
        // Stale nonce – issue a new one
        std::string freshNonce = generateNonce();
        nonces_[freshNonce] = now + 600000;
        // Remove stale
        if (nit != nonces_.end()) nonces_.erase(nit);

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
        sendError(msg, STUN_ERROR_UNAUTHORIZED, "Unknown user", from, io);
        return false;
    }

    // Verify MESSAGE-INTEGRITY using long-term key
    if (!msg.verifyIntegrity(uit->second)) {
        sendError(msg, STUN_ERROR_UNAUTHORIZED, "Bad credentials", from, io);
        return false;
    }

    // Consume nonce (one-time use to prevent replay)
    nonces_.erase(nit);

    if (outUsername) *outUsername = username;
    return true;
}

bool TurnServer::verifyLongTermAuth(const StunMessage& msg,
                                    const std::string& password) const {
    return msg.verifyIntegrity(password);
}

// ────────────────────────────────────────────────────────────
// ALLOCATE (RFC 5766 §6)
// ────────────────────────────────────────────────────────────

void TurnServer::handleAllocate(const StunMessage& req,
                                const struct sockaddr* from, hio_t* io) {
    std::string username;
    if (!authenticate(req, from, io, &username)) return;

    AllocKey key = makeKey(from, io);

    // Check for existing allocation (refresh-like behaviour)
    if (allocations_.count(key)) {
        TurnAllocation& existing = allocations_[key];
        // Return success with existing relay address
        StunMessage resp(TURN_METHOD_ALLOCATE, STUN_CLASS_SUCCESS_RESPONSE);
        resp.setTransactionId(req.transactionId());
        resp.addXorRelayedAddress(&existing.relayAddr.sa);
        resp.addXorMappedAddress(from);
        resp.addLifetime(existing.lifetime);
        if (!opts_.software.empty()) resp.addSoftware(opts_.software);
        auto buf = resp.encode();
        sendTo(buf.data(), buf.size(), from, io);
        return;
    }

    // Bind a new relay UDP socket
    hio_t* relayIo = bindRelaySocket();
    if (!relayIo) {
        sendError(req, STUN_ERROR_INSUFFICIENT_CAPACITY, "No relay port available", from, io);
        return;
    }

    // Parse requested lifetime
    uint32_t lifetime = req.getLifetime();
    if (lifetime == 0 || lifetime > 3600) lifetime = 600;

    TurnAllocation alloc;
    memset(&alloc.clientAddr, 0, sizeof(alloc.clientAddr));
    memcpy(&alloc.clientAddr, from, SOCKADDR_LEN(from));
    alloc.clientIo  = io;
    alloc.lifetime  = lifetime;
    alloc.expireTime = hloop_now_ms(loop_->loop()) + (uint64_t)lifetime * 1000;
    alloc.username  = username;
    alloc.realm     = opts_.realm;
    alloc.relayIo   = relayIo;
    memcpy(&alloc.relayAddr, hio_localaddr(relayIo), SOCKADDR_LEN(hio_localaddr(relayIo)));

    // Insert before scheduling expiry (scheduleExpiry accesses allocations_)
    allocations_[key] = std::move(alloc);
    relay_io_map_[hio_fd(relayIo)] = key;
    scheduleExpiry(key, lifetime);

    TurnAllocation& stored = allocations_[key];

    // Notify
    if (onAllocationChanged) onAllocationChanged(stored, true);

    hlogd("TurnServer: allocated relay %s for client %s (lifetime=%us)",
          sockaddrToKey(&stored.relayAddr.sa).c_str(), key.c_str(), lifetime);

    // Build success response
    StunMessage resp(TURN_METHOD_ALLOCATE, STUN_CLASS_SUCCESS_RESPONSE);
    resp.setTransactionId(req.transactionId());
    resp.addXorRelayedAddress(&stored.relayAddr.sa);
    resp.addXorMappedAddress(from);
    resp.addLifetime(lifetime);
    if (!opts_.software.empty()) resp.addSoftware(opts_.software);
    auto buf = resp.encode();
    sendTo(buf.data(), buf.size(), from, io);
}

// ────────────────────────────────────────────────────────────
// REFRESH (RFC 5766 §7)
// ────────────────────────────────────────────────────────────

void TurnServer::handleRefresh(const StunMessage& req,
                               const struct sockaddr* from, hio_t* io) {
    std::string username;
    if (!authenticate(req, from, io, &username)) return;

    AllocKey key = makeKey(from, io);
    TurnAllocation* alloc = findAllocation(key);
    if (!alloc) {
        sendError(req, STUN_ERROR_BAD_REQUEST, "No allocation", from, io);
        return;
    }

    uint32_t lifetime = req.getLifetime();
    if (lifetime == 0) {
        // Deallocation
        removeAllocation(key);
        StunMessage resp(TURN_METHOD_REFRESH, STUN_CLASS_SUCCESS_RESPONSE);
        resp.setTransactionId(req.transactionId());
        resp.addLifetime(0);
        auto buf = resp.encode();
        sendTo(buf.data(), buf.size(), from, io);
        return;
    }

    if (lifetime > 3600) lifetime = 600;

    alloc->lifetime   = lifetime;
    alloc->expireTime = hloop_now_ms(loop_->loop()) + (uint64_t)lifetime * 1000;
    scheduleExpiry(key, lifetime);

    StunMessage resp(TURN_METHOD_REFRESH, STUN_CLASS_SUCCESS_RESPONSE);
    resp.setTransactionId(req.transactionId());
    resp.addLifetime(lifetime);
    auto buf = resp.encode();
    sendTo(buf.data(), buf.size(), from, io);
}

// ────────────────────────────────────────────────────────────
// CREATE PERMISSION (RFC 5766 §9)
// ────────────────────────────────────────────────────────────

void TurnServer::handleCreatePermission(const StunMessage& req,
                                        const struct sockaddr* from, hio_t* io) {
    std::string username;
    if (!authenticate(req, from, io, &username)) return;

    AllocKey key = makeKey(from, io);
    TurnAllocation* alloc = findAllocation(key);
    if (!alloc) {
        sendError(req, STUN_ERROR_BAD_REQUEST, "No allocation", from, io);
        return;
    }

    // May contain multiple XOR-PEER-ADDRESS attributes
    for (const auto& attr : req.attributes()) {
        if (attr.type != STUN_ATTR_XOR_PEER_ADDRESS) continue;
        struct sockaddr_storage peer;
        if (!req.getXorPeerAddress(&peer)) continue;

        sockaddr_u permKey;
        memcpy(&permKey, &peer, SOCKADDR_LEN((struct sockaddr*)&peer));
        // Store permission keyed by IP only (zero the port)
        if (permKey.sa.sa_family == AF_INET)       permKey.sin.sin_port = 0;
        else if (permKey.sa.sa_family == AF_INET6)  permKey.sin6.sin6_port = 0;

        uint64_t expiry = hloop_now_ms(loop_->loop()) + 300000; // 5 min
        alloc->permissions[permKey] = expiry;
        hlogd("TurnServer: permission added for peer IP, client=%s", key.c_str());
    }

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
    TurnAllocation* alloc = findAllocation(key);
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

    sockaddr_u peerAddr;
    memcpy(&peerAddr, &peer, SOCKADDR_LEN((struct sockaddr*)&peer));

    uint64_t expiry = hloop_now_ms(loop_->loop()) + 600000; // 10 min

    TurnAllocation::ChannelEntry entry;
    entry.channelNumber = channel;
    entry.peerAddr      = peerAddr;
    entry.expireTime    = expiry;

    alloc->channels[channel]         = entry;
    alloc->peerToChannel[peerAddr]   = channel;

    // Also ensure permission exists for this peer IP
    sockaddr_u permKey = peerAddr;
    if (permKey.sa.sa_family == AF_INET)       permKey.sin.sin_port = 0;
    else if (permKey.sa.sa_family == AF_INET6)  permKey.sin6.sin6_port = 0;
    alloc->permissions[permKey] = expiry;

    hlogd("TurnServer: channel 0x%04x bound to peer %s, client=%s",
          channel, sockaddrToKey((const struct sockaddr*)&peer).c_str(), key.c_str());

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
    TurnAllocation* alloc = findAllocation(key);
    if (!alloc) return; // silently drop

    struct sockaddr_storage peer;
    if (!req.getXorPeerAddress(&peer)) return;

    const uint8_t* payload = nullptr;
    size_t         payloadLen = 0;
    if (!req.getData(&payload, &payloadLen) || !payload || payloadLen == 0) return;

    // Check permission
    sockaddr_u permKey;
    memcpy(&permKey, &peer, SOCKADDR_LEN((struct sockaddr*)&peer));
    if (permKey.sa.sa_family == AF_INET)       permKey.sin.sin_port = 0;
    else if (permKey.sa.sa_family == AF_INET6)  permKey.sin6.sin6_port = 0;
    uint64_t now = hloop_now_ms(loop_->loop());
    auto pit = alloc->permissions.find(permKey);
    if (pit == alloc->permissions.end() || pit->second < now) {
        hlogd("TurnServer: Send indication dropped – no permission for peer");
        return;
    }

    // Forward to peer through relay socket
    if (alloc->relayIo) {
        hio_sendto(alloc->relayIo, payload, (int)payloadLen, (struct sockaddr*)&peer);
    }
}

// ────────────────────────────────────────────────────────────
// ChannelData (RFC 5766 §11.5)
// ────────────────────────────────────────────────────────────

void TurnServer::handleChannelData(const uint8_t* data, size_t len,
                                   const struct sockaddr* from, hio_t* io) {
    if (len < 4) return;
    uint16_t channel  = ((uint16_t)data[0] << 8) | data[1];
    uint16_t dataLen  = ((uint16_t)data[2] << 8) | data[3];
    if (4 + dataLen > len) return;

    AllocKey key = makeKey(from, io);
    TurnAllocation* alloc = findAllocation(key);
    if (!alloc) return;

    auto cit = alloc->channels.find(channel);
    if (cit == alloc->channels.end()) return;

    const struct sockaddr* peerAddr = &cit->second.peerAddr.sa;

    // Check permission
    sockaddr_u permKey = cit->second.peerAddr;
    if (permKey.sa.sa_family == AF_INET)       permKey.sin.sin_port = 0;
    else if (permKey.sa.sa_family == AF_INET6)  permKey.sin6.sin6_port = 0;
    uint64_t now = hloop_now_ms(loop_->loop());
    auto pit = alloc->permissions.find(permKey);
    if (pit == alloc->permissions.end() || pit->second < now) return;

    // Forward to peer through relay socket
    if (alloc->relayIo) {
        hio_sendto(alloc->relayIo, data + 4, dataLen, (struct sockaddr*)peerAddr);
    }
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
        nonces_[nonce] = hloop_now_ms(loop_->loop()) + 600000;
        err.addNonce(nonce);
    }
    if (!opts_.software.empty()) err.addSoftware(opts_.software);
    auto buf = err.encode();
    sendTo(buf.data(), buf.size(), to, io);
}

int TurnServer::sendTo(const void* data, size_t len,
                       const struct sockaddr* to, hio_t* io) {
    if (!io) return -1;
    if (hio_type(io) == HIO_TYPE_TCP) {
        // RFC 4571: 2-byte length prefix
        uint8_t header[2];
        header[0] = (uint8_t)((len >> 8) & 0xFF);
        header[1] = (uint8_t)(len & 0xFF);
        hio_write(io, header, 2);
        return hio_write(io, data, (int)len);
    }
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

TurnAllocation* TurnServer::findAllocation(const AllocKey& key) {
    auto it = allocations_.find(key);
    return (it != allocations_.end()) ? &it->second : nullptr;
}

TurnAllocation* TurnServer::findAllocationByRelay(hio_t* relay_io) {
    auto it = relay_io_map_.find(hio_fd(relay_io));
    if (it == relay_io_map_.end()) return nullptr;
    return findAllocation(it->second);
}

void TurnServer::removeAllocation(const AllocKey& key) {
    auto it = allocations_.find(key);
    if (it == allocations_.end()) return;

    TurnAllocation& alloc = it->second;

    if (onAllocationChanged) onAllocationChanged(alloc, false);

    // Remove relay socket
    if (alloc.relayIo) {
        relay_io_map_.erase(hio_fd(alloc.relayIo));
        closeRelaySocket(alloc.relayIo);
    }

    // Cancel expiry timer
    auto tit = expiry_timers_.find(key);
    if (tit != expiry_timers_.end()) {
        if (tit->second) htimer_del(tit->second);
        expiry_timers_.erase(tit);
    }

    hlogd("TurnServer: allocation removed for client=%s", key.c_str());
    allocations_.erase(it);
}

void TurnServer::scheduleExpiry(const AllocKey& key, uint32_t lifetimeSec) {
    // Cancel old timer
    auto tit = expiry_timers_.find(key);
    if (tit != expiry_timers_.end() && tit->second) {
        htimer_del(tit->second);
        tit->second = nullptr;
    }

    uint32_t ms = lifetimeSec * 1000;
    struct ExpiryCtx {
        TurnServer* server;
        AllocKey    key;
    };
    auto* ctx = new ExpiryCtx{this, key};
    htimer_t* t = htimer_add(loop_->loop(), [](htimer_t* timer) {
        auto* ctx = static_cast<ExpiryCtx*>(hevent_userdata(timer));
        if (ctx) {
            hlogd("TurnServer: allocation expired for client=%s", ctx->key.c_str());
            ctx->server->removeAllocation(ctx->key);
            delete ctx;
            hevent_set_userdata(timer, nullptr);
        }
    }, ms, 1 /*once*/);
    hevent_set_userdata(t, ctx);
    expiry_timers_[key] = t;
}

// ────────────────────────────────────────────────────────────
// Relay socket management
// ────────────────────────────────────────────────────────────

hio_t* TurnServer::bindRelaySocket() {
    hloop_t* loop = loop_->loop();
    // Bind ephemeral UDP on all interfaces, port 0
    hio_t* relay_io = hloop_create_udp_server(loop, opts_.bindHost.c_str(), 0);
    if (!relay_io) return nullptr;

    hio_setcb_read(relay_io, [](hio_t* io, void* buf, int readbytes) {
        auto* self = static_cast<TurnServer*>(hio_context(io));
        if (self) {
            self->onRelayRecv(static_cast<const uint8_t*>(buf),
                              static_cast<size_t>(readbytes),
                              hio_peeraddr(io), io);
        }
    });
    hio_set_context(relay_io, this);
    hio_read(relay_io);
    return relay_io;
}

void TurnServer::closeRelaySocket(hio_t* io) {
    if (!io) return;
    hio_del(io, HV_READ);
    hio_close(io);
}

// ────────────────────────────────────────────────────────────
// TCP (skeleton)
// ────────────────────────────────────────────────────────────

/*static*/ void TurnServer::onTcpAccept(hio_t* io) {
    auto* self = static_cast<TurnServer*>(hevent_userdata(hio_get_upstream(io)));
    if (!self) return;

    hio_setcb_read(io, onTcpRecv);
    hio_setcb_close(io, onTcpClose);
    hio_set_context(io, self);
    hio_set_unpack(io, &self->tcp_unpack_setting_);
    hio_read(io);
}

/*static*/ void TurnServer::onTcpRecv(hio_t* io, void* buf, int readbytes) {
    auto* self = static_cast<TurnServer*>(hio_context(io));
    if (!self) return;

    const uint8_t* data = static_cast<const uint8_t*>(buf);
    size_t         len  = static_cast<size_t>(readbytes);
    auto* peerAddr = hio_peeraddr(io);

    PacketType ptype = classifyPacket(data, len);
    if (ptype == PacketType::STUN) {
        StunMessage msg;
        if (!StunMessage::decode(data, len, &msg)) return;
        uint16_t method = msg.method();
        uint16_t cls    = msg.cls();
        if      (method == STUN_METHOD_BINDING          && cls == STUN_CLASS_REQUEST)    self->handleBinding(msg, peerAddr, io);
        else if (method == TURN_METHOD_ALLOCATE          && cls == STUN_CLASS_REQUEST)    self->handleAllocate(msg, peerAddr, io);
        else if (method == TURN_METHOD_REFRESH           && cls == STUN_CLASS_REQUEST)    self->handleRefresh(msg, peerAddr, io);
        else if (method == TURN_METHOD_CREATE_PERMISSION && cls == STUN_CLASS_REQUEST)    self->handleCreatePermission(msg, peerAddr, io);
        else if (method == TURN_METHOD_CHANNEL_BIND      && cls == STUN_CLASS_REQUEST)    self->handleChannelBind(msg, peerAddr, io);
        else if (method == TURN_METHOD_SEND              && cls == STUN_CLASS_INDICATION) self->handleSendIndication(msg, peerAddr, io);
    } else if (ptype == PacketType::TURN_CHANNEL) {
        self->handleChannelData(data, len, peerAddr, io);
    }
}

/*static*/ void TurnServer::onTcpClose(hio_t* io) {
    auto* self = static_cast<TurnServer*>(hio_context(io));
    if (!self) return;
    AllocKey key = self->makeKey(hio_peeraddr(io), io);
    self->removeAllocation(key);
}

} // namespace ice
