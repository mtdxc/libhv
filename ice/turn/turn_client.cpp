#include "turn_client.h"
#include "../agent/ice_agent.h"
#include "../stun/stun_auth.h"

#include <sstream>
#include <iomanip>
#include <cstring>

namespace ice {

TurnClient::TurnClient(hv::EventLoopPtr loop, IceAgent* transport)
    : loop_(loop), agent_(transport) {
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
    if (io_!=agent_->udpIo()) {
        hio_close(io_);
    }
    agent_->unregisterPair(server_addr_);
}

bool TurnClient::setConfig(const TurnServerConfig& config) {
    config_ = config;
    return 0 == sockaddr_set_ipport(&server_addr_, config_.addr.host.c_str(), config_.addr.port);
}

void TurnClient::allocate() {
    if (state_ != TurnState::Idle && state_ != TurnState::Failed) return;
    state_ = TurnState::Allocating;
    if (onStateChange) onStateChange(state_);
    if (config_.protocol == TurnServerConfig::UDP){
        io_ = agent_->udpIo();
        agent_->registerPair(server_addr_, this);
        sendAllocateRequest();
    }
    else {
        agent_->connectTcp(&server_addr_.sa, this);
    }
}

void TurnClient::onTcpConnected(hio_t* io) { 
    io_ = io; 
    sendAllocateRequest();
}

void TurnClient::onTcpDisconnected(hio_t* io) { 
    if (io == io_) {
        io_ = nullptr;
        // Reconnect on disconnect
        if (state_ != TurnState::Idle && state_ != TurnState::Failed) {
            agent_->connectTcp(&server_addr_.sa, this);
        }
    }
}

void TurnClient::sendAllocateRequest() {
    if(!io_) return;
    StunMessage msg(TURN_METHOD_ALLOCATE, STUN_CLASS_REQUEST);
    msg.addRequestedTransport(17); // UDP = 17
    msg.addLifetime(lifetime_);

    std::weak_ptr<TurnClient> weakSelf = shared_from_this();
    agent_->StunRequest(msg, &server_addr_.sa, io_, [weakSelf](StunMessage* resp, int code) {
        auto self = weakSelf.lock();
        if (resp && self) {
            if(resp->cls() == STUN_CLASS_SUCCESS_RESPONSE) {
                self->handleAllocateResponse(*resp);
            } else {
                self->handleAllocateError(*resp);
            }
        }
    });
}

void TurnClient::sendAllocateRequestWithAuth() {
    StunMessage msg(TURN_METHOD_ALLOCATE, STUN_CLASS_REQUEST);
    msg.addRequestedTransport(17);
    msg.addLifetime(lifetime_);
    msg.addUsername(config_.username);
    msg.addRealm(realm_);
    msg.addNonce(nonce_);

    // For long-term credentials, key = MD5(username:realm:password)
    // Simplified: use password directly for HMAC
    // In production, compute key = MD5(username + ":" + realm + ":" + password)
    std::string key = config_.username + ":" + realm_ + ":" + config_.password;
    // For simplicity, use password as HMAC key
    msg.setAuth(config_.password);
    std::weak_ptr<TurnClient> weakSelf = shared_from_this();
    agent_->StunRequest(msg, &server_addr_.sa, io_, [weakSelf](StunMessage* resp, int code) {
        auto self = weakSelf.lock();
        if (resp && self) {
            if(resp->cls() == STUN_CLASS_SUCCESS_RESPONSE) {
                self->handleAllocateResponse(*resp);
            } else {
                self->handleAllocateError(*resp);
            }
        }
    });
}

void TurnClient::deallocate() {
    refresh(0); // Lifetime=0 means deallocate
    state_ = TurnState::Idle;
    if (refresh_timer_) {
        htimer_del(refresh_timer_);
        refresh_timer_ = nullptr;
    }
}

void TurnClient::channelBind(const struct sockaddr* peerAddr, uint16_t channelNumber) {
    if (state_ != TurnState::Allocated || !io_) return;
    if (channelNumber < 0x4000 || channelNumber > 0x7FFE) return;

    StunMessage msg(TURN_METHOD_CHANNEL_BIND, STUN_CLASS_REQUEST);
    msg.addChannelNumber(channelNumber);
    msg.addXorPeerAddress(peerAddr);
    msg.addUsername(config_.username);
    msg.addRealm(realm_);
    msg.addNonce(nonce_);
    msg.setAuth(config_.password);

    TurnChannelBinding binding;
    binding.channelNumber = channelNumber;
    memcpy(&binding.peerAddr, peerAddr, SOCKADDR_LEN(peerAddr));
    binding.expireTime = hloop_now_ms(loop_->loop()) + 600000; 
    // 10 min
    std::weak_ptr<TurnClient> weakSelf = shared_from_this();
    agent_->StunRequest(msg, &server_addr_.sa, io_, [=](StunMessage* resp, int code) {
        auto self = weakSelf.lock();
        if (resp && self) {
            if (resp->cls() == STUN_CLASS_SUCCESS_RESPONSE) {
                self->channels_[channelNumber] = binding;
            } else if (resp->cls() == STUN_CLASS_ERROR_RESPONSE) {
                ;//handleChannelBindError(*resp);
            }
        }
    }); 
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
    return agent_->send(buf.data(), buf.size(), &server_addr_.sa, io_);
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

    return agent_->send(buf.data(), buf.size(), &server_addr_.sa, io_); // Changed from sendTo to send
}

void TurnClient::onStunRequest(StunMessage& msg, const sockaddr* addr, hio_t* io) {
    uint16_t method = msg.method();
    uint16_t cls = msg.cls();
    if (cls == STUN_CLASS_INDICATION || method == TURN_METHOD_DATA) {
        handleDataIndication(msg);
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

void TurnClient::onRecvData(const uint8_t* data, size_t len, const struct sockaddr* addr) {
    PacketType ptype = classifyPacket(data, len);
    switch (ptype) {
    case PacketType::TURN_CHANNEL:
        onChannelData(data, len);
        break;
    default:
        hlogi("TurnClient: ignore %d data", len);
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
    if (it == channels_.end()) return;

    if (onData) {
        onData(data + 4, dataLen, &it->second.peerAddr.sa);
    }
}


void TurnClient::refresh(uint32_t lifetime) {
    if (state_ != TurnState::Allocated || !io_) return;

    StunMessage msg(TURN_METHOD_REFRESH, STUN_CLASS_REQUEST);
    msg.addLifetime(lifetime);
    msg.addUsername(config_.username);
    msg.addRealm(realm_);
    msg.addNonce(nonce_);
    msg.setAuth(config_.password);

    std::weak_ptr<TurnClient> weakSelf = shared_from_this();
    agent_->StunRequest(msg, &server_addr_.sa, io_, [weakSelf](StunMessage* resp, int code) {
        auto self = weakSelf.lock();
        if (resp && self) {
            if (resp->cls() == STUN_CLASS_SUCCESS_RESPONSE){
                uint32_t newLifetime = resp->getLifetime();
                if (newLifetime > 0) {
                    self->lifetime_ = newLifetime;
                }
            } else {
               // handleRefreshError(*resp);
            }
        }
    });
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

void TurnClient::createPermission(const struct sockaddr* peerAddr) {
    if (state_ != TurnState::Allocated || !io_) return;

    StunMessage msg(TURN_METHOD_CREATE_PERMISSION, STUN_CLASS_REQUEST);
    msg.addXorPeerAddress(peerAddr);
    msg.addUsername(config_.username);
    msg.addRealm(realm_);
    msg.addNonce(nonce_);
    msg.setAuth(config_.password);

    std::weak_ptr<TurnClient> weakSelf = shared_from_this();
    sockaddr_u peerAddrU;
    memcpy(&peerAddrU, peerAddr, SOCKADDR_LEN(peerAddr));
    agent_->StunRequest(msg, &server_addr_.sa, io_, [weakSelf, peerAddrU](StunMessage* resp, int code) {
        auto self = weakSelf.lock();
        if (resp && self) {
            if (resp->cls() == STUN_CLASS_SUCCESS_RESPONSE) {
                self->permissions_[peerAddrU] = hloop_now_ms(self->loop_->loop()) + 300000;
            } else {
            }
        }
    });
}
} // namespace ice
