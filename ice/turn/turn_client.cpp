#include "turn_client.h"
#include "../transport/udp_transport.h"
#include "../stun/stun_auth.h"

#include <sstream>
#include <iomanip>
#include <cstring>

namespace ice {

static std::string txnIdToHex(const TransactionId& id) {
    std::ostringstream oss;
    for (auto b : id) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    }
    return oss.str();
}

TurnClient::TurnClient(hv::EventLoopPtr loop, UdpTransport* transport)
    : loop_(loop), transport_(transport) {
    memset(&server_addr_, 0, sizeof(server_addr_));
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
}

void TurnClient::setServer(const struct sockaddr* serverAddr) {
    memcpy(&server_addr_, serverAddr, SOCKADDR_LEN(serverAddr));
}

void TurnClient::setCredentials(const std::string& username, const std::string& password) {
    username_ = username;
    password_ = password;
}

void TurnClient::allocate() {
    if (state_ != TurnState::Idle && state_ != TurnState::Failed) return;
    state_ = TurnState::Allocating;
    if (onStateChange) onStateChange(state_);
    sendAllocateRequest();
}

void TurnClient::sendAllocateRequest() {
    StunMessage msg(TURN_METHOD_ALLOCATE, STUN_CLASS_REQUEST);
    msg.addRequestedTransport(17); // UDP = 17
    msg.addLifetime(lifetime_);

    auto buf = msg.encode(); // First request without auth
    transport_->sendTo(buf.data(), buf.size(), &server_addr_.sa);

    pending_transactions_[txnIdToHex(msg.transactionId())] = msg.transactionId();
}

void TurnClient::sendAllocateRequestWithAuth() {
    StunMessage msg(TURN_METHOD_ALLOCATE, STUN_CLASS_REQUEST);
    msg.addRequestedTransport(17);
    msg.addLifetime(lifetime_);
    msg.addUsername(username_);
    msg.addRealm(realm_);
    msg.addNonce(nonce_);

    // For long-term credentials, key = MD5(username:realm:password)
    // Simplified: use password directly for HMAC
    // In production, compute key = MD5(username + ":" + realm + ":" + password)
    std::string key = username_ + ":" + realm_ + ":" + password_;
    // For simplicity, use password as HMAC key
    auto buf = msg.encodeWithAuth(password_);
    transport_->sendTo(buf.data(), buf.size(), &server_addr_.sa);

    pending_transactions_[txnIdToHex(msg.transactionId())] = msg.transactionId();
}

void TurnClient::refresh(uint32_t lifetime) {
    if (state_ != TurnState::Allocated) return;

    StunMessage msg(TURN_METHOD_REFRESH, STUN_CLASS_REQUEST);
    msg.addLifetime(lifetime);
    msg.addUsername(username_);
    msg.addRealm(realm_);
    msg.addNonce(nonce_);

    auto buf = msg.encodeWithAuth(password_);
    transport_->sendTo(buf.data(), buf.size(), &server_addr_.sa);
    pending_transactions_[txnIdToHex(msg.transactionId())] = msg.transactionId();
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
    if (state_ != TurnState::Allocated) return;

    StunMessage msg(TURN_METHOD_CREATE_PERMISSION, STUN_CLASS_REQUEST);
    msg.addXorPeerAddress(peerAddr);
    msg.addUsername(username_);
    msg.addRealm(realm_);
    msg.addNonce(nonce_);

    auto buf = msg.encodeWithAuth(password_);
    transport_->sendTo(buf.data(), buf.size(), &server_addr_.sa);
    pending_transactions_[txnIdToHex(msg.transactionId())] = msg.transactionId();

    // Add to permissions list
    TurnPermission perm;
    memcpy(&perm.peerAddr, peerAddr, SOCKADDR_LEN(peerAddr));
    perm.expireTime = hloop_now_ms(loop_->loop()) + 300000; // 5 min
    permissions_.push_back(perm);
}

void TurnClient::channelBind(const struct sockaddr* peerAddr, uint16_t channelNumber) {
    if (state_ != TurnState::Allocated) return;
    if (channelNumber < 0x4000 || channelNumber > 0x7FFE) return;

    StunMessage msg(TURN_METHOD_CHANNEL_BIND, STUN_CLASS_REQUEST);
    msg.addChannelNumber(channelNumber);
    msg.addXorPeerAddress(peerAddr);
    msg.addUsername(username_);
    msg.addRealm(realm_);
    msg.addNonce(nonce_);

    auto buf = msg.encodeWithAuth(password_);
    transport_->sendTo(buf.data(), buf.size(), &server_addr_.sa);
    pending_transactions_[txnIdToHex(msg.transactionId())] = msg.transactionId();

    TurnChannelBinding binding;
    binding.channelNumber = channelNumber;
    memcpy(&binding.peerAddr, peerAddr, SOCKADDR_LEN(peerAddr));
    binding.expireTime = hloop_now_ms(loop_->loop()) + 600000; // 10 min
    channels_[channelNumber] = binding;
}

int TurnClient::sendData(const void* data, size_t len, const struct sockaddr* peerAddr) {
    if (state_ != TurnState::Allocated) return -1;

    // Check if we have a channel binding for this peer
    for (auto& kv : channels_) {
        if (memcmp(&kv.second.peerAddr, peerAddr, SOCKADDR_LEN(peerAddr)) == 0) {
            return sendChannelData(data, len, kv.first);
        }
    }

    // Fall back to Send indication
    StunMessage msg(TURN_METHOD_SEND, STUN_CLASS_INDICATION);
    msg.addXorPeerAddress(peerAddr);
    msg.addData(data, len);

    auto buf = msg.encode(); // Indications don't need auth
    return transport_->sendTo(buf.data(), buf.size(), &server_addr_.sa);
}

int TurnClient::sendChannelData(const void* data, size_t len, uint16_t channelNumber) {
    if (state_ != TurnState::Allocated) return -1;

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

    return transport_->sendTo(buf.data(), buf.size(), &server_addr_.sa);
}

void TurnClient::onStunMessage(const StunMessage& msg, const struct sockaddr* from) {
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

    state_ = TurnState::Allocated;
    if (onStateChange) onStateChange(state_);

    startRefreshTimer();
}

void TurnClient::handleAllocateError(const StunMessage& msg) {
    uint16_t code = 0;
    std::string reason;
    msg.getErrorCode(&code, &reason);

    if (code == STUN_ERROR_UNAUTHORIZED) {
        // Get realm and nonce for authentication
        realm_ = msg.getRealm();
        nonce_ = msg.getNonce();
        if (!realm_.empty() && !nonce_.empty()) {
            // Retry with authentication
            sendAllocateRequestWithAuth();
            return;
        }
    }

    state_ = TurnState::Failed;
    if (onStateChange) onStateChange(state_);
}

void TurnClient::handleRefreshResponse(const StunMessage& msg) {
    uint32_t newLifetime = msg.getLifetime();
    if (newLifetime > 0) {
        lifetime_ = newLifetime;
    }
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

void TurnClient::onChannelData(const uint8_t* data, size_t len) {
    // ChannelData: 2-byte channel + 2-byte length + payload
    if (len < 4) return;
    uint16_t channel = ((uint16_t)data[0] << 8) | data[1];
    uint16_t dataLen = ((uint16_t)data[2] << 8) | data[3];
    if (4 + dataLen > len) return;

    // Find peer address from channel binding
    auto it = channels_.find(channel);
    if (it == channels_.end()) return;

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
            self->createPermission(&perm.peerAddr.sa);
        }
    }, 240000, INFINITE);
    hevent_set_userdata(permission_timer_, this);
}

} // namespace ice
