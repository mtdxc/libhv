#include "turn_client.h"
#include "../stun/stun_auth.h"
#include "hlog.h"
#include "md5.h"

#include <sstream>
#include <iomanip>
#include <cstring>

namespace ice {

const char* turnStateToString(TurnState state) {
    switch (state) {
        case TurnState::Idle: return "Idle";
        case TurnState::Allocating: return "Allocating";
        case TurnState::Allocated: return "Allocated";
        case TurnState::Refreshing: return "Refreshing";
        case TurnState::Failed: return "Failed";
        default: return "Unknown";
    }
}

TurnClient::TurnClient(hv::EventLoopPtr loop, const TurnServerConfig& config)
    : loop_(loop), config_(config) {
    memset(&relay_addr_, 0, sizeof(relay_addr_));
    memset(&srflx_addr_, 0, sizeof(srflx_addr_));
}

TurnClient::~TurnClient() {
    if (refresh_timer_) {
        htimer_del(refresh_timer_);
        refresh_timer_ = nullptr;
    }
    if (permission_timer_) {
        htimer_del(permission_timer_);
        permission_timer_ = nullptr;
    }
    if (io_) {
        hio_close(io_);
        io_ = nullptr;
    }
}

const char* TurnClient::id() const {
    return config_.addr.host.c_str();
}

// RFC 5766 Section 10.2: long-term credential HMAC key
// key = MD5(username ":" realm ":" password) as 16-byte binary digest
std::string TurnClient::longTermKey() const {
    std::string raw = config_.username + ":" + realm_ + ":" + config_.password;
    uint8_t hex[16] = {0};
    hv_md5((unsigned char*)raw.data(), (unsigned int)raw.size(), hex);
    return std::string((char*)hex, sizeof(hex));
}

void TurnClient::setState(TurnState s, const char* reason) {
    if (state_ == s) return;
    hlogi("TurnClient %s setState %s reason %s", id(), turnStateToString(s), reason);
    state_ = s;
    if (onStateChange) onStateChange(state_);
}

void TurnClient::onTcpRecv(const void* data, int len) {
    tcp_buff_.append((const char*)data, len);
    len = tcp_buff_.length();
    auto p = (const uint8_t*)tcp_buff_.data();
    while (len >= 4) {
        int padding = 0;
        int plen = TurnTcpLength(p, len, &padding);
        if (plen + padding > len) {
            break;
        }
        onRecvPdu(p, plen);
        p = p + plen + padding;
        len = len - plen - padding;
    }
    if (len != tcp_buff_.length()) {
        if (len) {
            memcpy(&tcp_buff_[0], p, len);
        }
        tcp_buff_.resize(len);
    }
}

void TurnClient::onTcpConnected(hio_t* io) {
    io_ = io;
    hlogi("TurnClient %s TCP connected, sending AllocateRequest", id());
    sendAllocateRequest();
    hio_read(io);
}

void TurnClient::onTcpDisconnected(hio_t* io) {
    if (io == io_) {
        hlogi("TurnClient %s TCP disconnected", id());
        io_ = nullptr;
        tcp_buff_.clear();
        // Reconnect on disconnect
        if (state_ != TurnState::Idle && state_ != TurnState::Failed) {
            hlogi("TurnClient %s reconnecting to TURN server", id());
            connectTcp();
        }
    }
}

int TurnClient::connectTcp() {
    hloop_t* loop = loop_->loop();
    if (!loop) return -1;

    hio_t* io = hio_create_socket(loop, config_.addr.host.c_str(), config_.addr.port, HIO_TYPE_TCP, HIO_CLIENT_SIDE);
    if (!io) return -1;

    hevent_set_userdata(io, this);
    hio_setcb_connect(io, [](hio_t* io) { 
      auto cli = (TurnClient*)hevent_userdata(io); 
      if (cli) cli->onTcpConnected(io);
    });
    hio_setcb_read(io, [](hio_t* io, void* buf, int readbytes) {
        auto cli = (TurnClient*)hevent_userdata(io);
        if (cli) cli->onTcpRecv(buf, readbytes);
    });
    hio_setcb_close(io, [](hio_t* io) {
        auto cli = (TurnClient*)hevent_userdata(io);
        if (cli) cli->onTcpDisconnected(io);
    });
    hio_connect(io);

    return hio_id(io);
}

void TurnClient::allocate() {
    if (state_ != TurnState::Idle && state_ != TurnState::Failed) {
        hlogw("TurnClient %s allocate called in invalid state %s", id(), turnStateToString(state_));
        return;
    }

    setState(TurnState::Allocating, "allocate");

    if (config_.protocol == TurnServerConfig::UDP){
        io_ = hio_create_socket(loop_->loop(), config_.addr.host.c_str(), config_.addr.port, HIO_TYPE_UDP, HIO_CLIENT_SIDE);
        if (io_) {
            hevent_set_userdata(io_, this);
            hio_setcb_read(io_, [](hio_t* io, void* buf, int readbytes) {
                auto cli = (TurnClient*)hevent_userdata(io);
                if (cli) cli->onRecvPdu((const uint8_t*)buf, readbytes);
            });
            hio_read(io_);
        }
        sendAllocateRequest();
    }
    else {
        connectTcp();
    }
}

void TurnClient::sendAllocateRequest() {
    if (!io_) {
        hlogw("TurnClient %s sendAllocateRequest without io_", id());
        return;
    }
    hlogi("TurnClient %s sending AllocateRequest (without auth)", id());
    StunMessage msg(TURN_METHOD_ALLOCATE, STUN_CLASS_REQUEST);
    msg.addRequestedTransport(17); // UDP = 17
    msg.addLifetime(lifetime_);

    auto buf = msg.encode(); // First request without auth
    hio_write(io_, buf.data(), buf.size());
}

void TurnClient::sendAllocateRequestWithAuth() {
    hlogi("TurnClient %s sending AllocateRequest with auth user=%s realm=%s",
          id(), config_.username.c_str(), realm_.c_str());
    StunMessage msg(TURN_METHOD_ALLOCATE, STUN_CLASS_REQUEST);
    msg.addRequestedTransport(17);
    msg.addLifetime(lifetime_);
    msg.addUsername(config_.username);
    msg.addRealm(realm_);
    msg.addNonce(nonce_);

    auto buf = msg.encodeWithAuth(longTermKey());
    hio_write(io_, buf.data(), buf.size());
}

void TurnClient::refresh(uint32_t lifetime) {
    if (state_ != TurnState::Allocated || !io_) return;

    hlogi("TurnClient %s refreshing allocation lifetime=%u", id(), lifetime);
    StunMessage msg(TURN_METHOD_REFRESH, STUN_CLASS_REQUEST);
    msg.addLifetime(lifetime);
    msg.addUsername(config_.username);
    msg.addRealm(realm_);
    msg.addNonce(nonce_);

    auto buf = msg.encodeWithAuth(longTermKey());
    hio_write(io_, buf.data(), buf.size());
}

void TurnClient::handleRefreshResponse(const StunMessage& msg){
    if (msg.cls() == STUN_CLASS_SUCCESS_RESPONSE) {
        uint32_t newLifetime = msg.getLifetime();
        if (newLifetime > 0) {
            lifetime_ = newLifetime;
        }
        hlogi("TurnClient %s refresh success new lifetime=%u", id(), lifetime_);
    } else {
        uint16_t errCode = 0;
        std::string reason;
        msg.getErrorCode(&errCode, &reason);
        hlogw("TurnClient %s refresh error code=%d reason=%s", id(), errCode, reason.c_str());
    }
}

void TurnClient::deallocate() {
    refresh(0); // Lifetime=0 means deallocate
    state_ = TurnState::Idle;
    if (refresh_timer_) {
        htimer_del(refresh_timer_);
        refresh_timer_ = nullptr;
    }
}

void TurnClient::createPermission(const struct sockaddr* peerAddr) {
    if (state_ != TurnState::Allocated || !io_) return;

    char peerStr[SOCKADDR_STRLEN] = {0};
    SOCKADDR_STR(peerAddr, peerStr);
    hlogi("TurnClient %s createPermission peer=%s", id(), peerStr);

    StunMessage msg(TURN_METHOD_CREATE_PERMISSION, STUN_CLASS_REQUEST);
    msg.addXorPeerAddress(peerAddr);
    msg.addUsername(config_.username);
    msg.addRealm(realm_);
    msg.addNonce(nonce_);

    auto buf = msg.encodeWithAuth(longTermKey());
    hio_write(io_, buf.data(), buf.size());

    sockaddr_u peerAddrU;
    memcpy(&peerAddrU, peerAddr, SOCKADDR_LEN(peerAddr));
    // Add to permissions list
    permissions_[peerAddrU] = hloop_now_ms(loop_->loop()) + 300000;
}

void TurnClient::channelBind(const struct sockaddr* peerAddr, uint16_t channelNumber) {
    if (state_ != TurnState::Allocated || !io_) return;
    if (channelNumber < 0x4000 || channelNumber > 0x7FFE) return;

    char peerStr[SOCKADDR_STRLEN] = {0};
    SOCKADDR_STR(peerAddr, peerStr);
    hlogi("TurnClient %s channelBind channel=0x%04x peer=%s", id(), channelNumber, peerStr);

    StunMessage msg(TURN_METHOD_CHANNEL_BIND, STUN_CLASS_REQUEST);
    msg.addChannelNumber(channelNumber);
    msg.addXorPeerAddress(peerAddr);
    msg.addUsername(config_.username);
    msg.addRealm(realm_);
    msg.addNonce(nonce_);

    auto buf = msg.encodeWithAuth(longTermKey());
    hio_write(io_, buf.data(), buf.size());

    TurnChannelBinding binding;
    binding.channelNumber = channelNumber;
    memcpy(&binding.peerAddr, peerAddr, SOCKADDR_LEN(peerAddr));
    binding.expireTime = hloop_now_ms(loop_->loop()) + 600000; // 10 min
    channels_[channelNumber] = binding;
}

int TurnClient::sendData(const void* data, size_t len, const struct sockaddr* peerAddr) {
    if (state_ != TurnState::Allocated || !io_) return -1;

    // Check if we have a channel binding for this peer
    for (auto& kv : channels_) {
        if (sockaddr_compare(&kv.second.peerAddr, (const sockaddr_u*)peerAddr) == 0) {
            return sendChannelData(data, len, kv.first);
        }
    }

    // Fall back to Send indication
    StunMessage msg(TURN_METHOD_SEND, STUN_CLASS_INDICATION);
    msg.addXorPeerAddress(peerAddr);
    msg.addData(data, len);

    auto buf = msg.encode(); // Indications don't need auth
    return hio_write(io_, buf.data(), buf.size());
}

int TurnClient::sendChannelData(const void* data, size_t len, uint16_t channelNumber) {
    if (state_ != TurnState::Allocated || !io_) return -1;

    // ChannelData format: 2-byte channel number + 2-byte length + data
    std::vector<uint8_t> buf(4 + len);
    buf[0] = (uint8_t)(channelNumber >> 8);
    buf[1] = (uint8_t)(channelNumber & 0xFF);
    buf[2] = (uint8_t)(len >> 8);
    buf[3] = (uint8_t)(len & 0xFF);
    memcpy(buf.data() + 4, data, len);

    // Pad to 4 bytes
    size_t padded = (buf.size() + 3) & ~3;
    buf.resize(padded, 0);

    return hio_write(io_, buf.data(), buf.size());
}

void TurnClient::onStunMessage(StunMessage& msg) {
    uint16_t method = msg.method();
    uint16_t cls = msg.cls();

    if (cls == STUN_CLASS_SUCCESS_RESPONSE) {
        switch (method) {
        case TURN_METHOD_ALLOCATE:
            handleAllocateResponse(msg);
            break;
        case TURN_METHOD_REFRESH:
            handleRefreshResponse(msg);
            break;
        case TURN_METHOD_CREATE_PERMISSION:
            handleCreatePermissionResponse(msg);
            break;
        case TURN_METHOD_CHANNEL_BIND:
            handleChannelBindResponse(msg);
            break;
        }
    } else if (cls == STUN_CLASS_ERROR_RESPONSE) {
        if (method == TURN_METHOD_ALLOCATE) {
            handleAllocateError(msg);
        }
    } else if (cls == STUN_CLASS_INDICATION) {
        if (method == TURN_METHOD_DATA) {
            handleDataIndication(msg);
        }
    }
}

void TurnClient::handleAllocateResponse(const StunMessage& msg) {
    // Get XOR-RELAYED-ADDRESS
    struct sockaddr_storage relayed;
    if (msg.getXorRelayedAddress(&relayed)) {
        memcpy(&relay_addr_, &relayed, SOCKADDR_LEN((struct sockaddr*)&relayed));
    }

    // Get XOR-MAPPED-ADDRESS (server-reflexive)
    struct sockaddr_storage mapped;
    if (msg.getXorMappedAddress(&mapped)) {
        memcpy(&srflx_addr_, &mapped, SOCKADDR_LEN((struct sockaddr*)&mapped));
    }

    // Get LIFETIME
    lifetime_ = msg.getLifetime();
    if (lifetime_ == 0) lifetime_ = 600;

    setState(TurnState::Allocated, "alloc ok");

    char relayStr[SOCKADDR_STRLEN] = {0};
    SOCKADDR_STR((struct sockaddr*)&relay_addr_, relayStr);
    char srflxStr[SOCKADDR_STRLEN] = {0};
    SOCKADDR_STR((struct sockaddr*)&srflx_addr_, srflxStr);

    hlogi("TurnClient %s allocation succeeded relay=%s srflx=%s lifetime=%us", id(),
          relayStr, srflxStr, lifetime_);

    startRefreshTimer();
}

void TurnClient::handleAllocateError(const StunMessage& msg) {
    uint16_t code = 0;
    std::string reason;
    msg.getErrorCode(&code, &reason);

    hlogw("TurnClient %s allocate error code=%d reason=%s", id(), code, reason.c_str());

    if (code == STUN_ERROR_UNAUTHORIZED) {
        // Get realm and nonce for authentication
        realm_ = msg.getRealm();
        nonce_ = msg.getNonce();
        if (!realm_.empty() && !nonce_.empty()) {
            hlogi("TurnClient %s retrying AllocateRequest with auth realm=%s", id(), realm_.c_str());
            // Retry with authentication
            sendAllocateRequestWithAuth();
            return;
        } else {
            hlogw("TurnClient %s 401 received but realm/nonce missing, cannot auth", id());
        }
    }

    setState(TurnState::Failed, reason.c_str());
}


void TurnClient::handleCreatePermissionResponse(const StunMessage& msg) {
    // Permission created successfully - no additional action needed
}

void TurnClient::handleChannelBindResponse(const StunMessage& msg) {
    // Channel bound successfully - no additional action needed
}

void TurnClient::handleDataIndication(const StunMessage& msg) {
    // Data from peer via TURN server
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    if (!msg.getData(&data, &dataLen)) return;

    struct sockaddr_storage peerAddr;
    if (!msg.getXorPeerAddress(&peerAddr)) return;

    if (onData) {
        onData(data, dataLen, (struct sockaddr*)&peerAddr);
    }
}

void TurnClient::onRecvPdu(const uint8_t* data, size_t len) {
    PacketType ptype = classifyPacket(data, len);
    switch (ptype) {
    case PacketType::TURN_CHANNEL:
        onChannelData(data, len);
        break;
    case PacketType::STUN:
    {
        StunMessage msg;
        if (StunMessage::decode(data, len, &msg)) {
            onStunMessage(msg);
        }
        break;
    }
    default:
        hlogd("TurnClient %s ignoring %zu bytes of non-channel data", id(), len);
        break;
    }
}

void TurnClient::onChannelData(const uint8_t* data, size_t len) {
    // ChannelData: 2-byte channel + 2-byte length + payload
    if (len < 4) return;
    uint16_t channel = ((uint16_t)data[0] << 8) | data[1];
    uint16_t dataLen = ((uint16_t)data[2] << 8) | data[3];
    if (4 + dataLen > len) return;

    // Find peer address from channel binding
    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        hlogw("TurnClient %s onChannelData unknown channel=0x%04x", id(), channel);
        return;
    }

    hlogd("TurnClient %s onChannelData channel=0x%04x len=%u", id(), channel, dataLen);
    if (onData) {
        onData(data + 4, dataLen, &it->second.peerAddr.sa);
    }
}

void TurnClient::startRefreshTimer() {
    if (refresh_timer_) {
        htimer_del(refresh_timer_);
    }
    // Refresh at 80% of lifetime
    uint32_t interval = (lifetime_ * 800); // 80% in ms
    refresh_timer_ = htimer_add(loop_->loop(), [](htimer_t* timer) {
        TurnClient* self = (TurnClient*)hevent_userdata(timer);
        if (self && self->state_ == TurnState::Allocated) {
            self->refresh();
        }
    }, interval, INFINITE);
    hevent_set_userdata(refresh_timer_, this);
}

void TurnClient::startPermissionRefreshTimer() {
    if (permission_timer_) return;
    // Refresh permissions every 4 minutes (permissions expire at 5)
    permission_timer_ = htimer_add(loop_->loop(), [](htimer_t* timer) {
        TurnClient* self = (TurnClient*)hevent_userdata(timer);
        if (!self) return;
        for (auto& perm : self->permissions_) {
            self->createPermission(&perm.first.sa);
        }
    }, 240000, INFINITE);
    hevent_set_userdata(permission_timer_, this);
}

} // namespace ice
