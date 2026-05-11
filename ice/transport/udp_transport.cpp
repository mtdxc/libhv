#include "udp_transport.h"
#include "../stun/stun_message.h"
#include "../session/ice_session.h"

namespace ice {

UdpTransport::UdpTransport(hv::EventLoopPtr loop)
    : loop_(loop) {
    memset(&local_addr_, 0, sizeof(local_addr_));
}

UdpTransport::~UdpTransport() {
    close();
}

int UdpTransport::bind(const std::string& host, int port) {
    hloop_t* loop = loop_->loop();
    if (!loop) return -1;

    io_ = hloop_create_udp_server(loop, host.c_str(), port);
    if (!io_) return -1;

    port_ = hio_localaddr(io_)->sa_family == AF_INET
        ? ntohs(((struct sockaddr_in*)hio_localaddr(io_))->sin_port)
        : ntohs(((struct sockaddr_in6*)hio_localaddr(io_))->sin6_port);

    memcpy(&local_addr_, hio_localaddr(io_), SOCKADDR_LEN(hio_localaddr(io_)));

    // Set read callback
    hio_setcb_read(io_, [](hio_t* io, void* buf, int readbytes) {
        UdpTransport* self = (UdpTransport*)hio_context(io);
        if (self) {
            self->onRecv((const uint8_t*)buf, readbytes, hio_peeraddr(io));
        }
    });
    hio_set_context(io_, this);
    hio_read(io_);

    return port_;
}

int UdpTransport::sendTo(const void* data, size_t len, const struct sockaddr* addr) {
    if (!io_) return -1;
    return hio_sendto(io_, data, len, (struct sockaddr*)addr);
}

void UdpTransport::registerSession(const std::string& ufrag, IceSession* session) {
    loop_->runInLoop([this, ufrag, session]() {
        ufrag_map_[ufrag] = session;
    });
}

void UdpTransport::unregisterSession(const std::string& ufrag) {
    loop_->runInLoop([this, ufrag]() {
        ufrag_map_.erase(ufrag);
    });
}

void UdpTransport::registerPair(const sockaddr_u& addr, IceSession* session) {
    loop_->runInLoop([this, addr, session]() {
        pair_map_[addr] = session;
    });
}

void UdpTransport::unregisterPair(const sockaddr_u& addr) {
    loop_->runInLoop([this, addr]() {
        pair_map_.erase(addr);
    });
}

void UdpTransport::close() {
    if (io_) {
        // Only call hio_close if loop is still valid (not yet freed)
        if (loop_ && loop_->loop()) {
            hio_close(io_);
        }
        io_ = nullptr;
    }
    ufrag_map_.clear();
    pair_map_.clear();
}

void UdpTransport::onRecv(const uint8_t* data, size_t len, const struct sockaddr* addr) {
    PacketType ptype = classifyPacket(data, len);
    switch (ptype) {
    case PacketType::STUN:
        dispatchStun(data, len, addr);
        break;
    case PacketType::TURN_CHANNEL:
        dispatchChannelData(data, len, addr);
        break;
    case PacketType::DATA:
        dispatchData(data, len, addr);
        break;
    }
}

std::string UdpTransport::extractLocalUfrag(const uint8_t* data, size_t len) {
    // Quick parse: find USERNAME attribute without full decode
    // STUN header: 20 bytes, then TLV attributes
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
            // Format: "local_ufrag:remote_ufrag"
            size_t colon = username.find(':');
            if (colon != std::string::npos) {
                return username.substr(0, colon);
            }
            return username;
        }

        // Next attribute (padded to 4 bytes)
        offset += 4 + ((attr_len + 3) & ~3);
    }
    return "";
}

void UdpTransport::dispatchStun(const uint8_t* data, size_t len, const struct sockaddr* addr) {
    // For responses, we need to match by transaction ID
    // For requests/indications, we match by USERNAME attribute's local ufrag
    uint16_t msg_type = ((uint16_t)data[0] << 8) | data[1];
    uint16_t cls = stun_get_class(msg_type);

    if (cls == STUN_CLASS_REQUEST || cls == STUN_CLASS_INDICATION) {
        // Extract local ufrag from USERNAME
        std::string ufrag = extractLocalUfrag(data, len);
        if (!ufrag.empty()) {
            auto it = ufrag_map_.find(ufrag);
            if (it != ufrag_map_.end() && it->second) {
                // Dispatch to session - session will handle full decode
                it->second->onStunPacket(data, len, addr);
                return;
            }
        }
    } else {
        // Response: try all sessions (they match by transaction ID)
        // First try pair_map by source address
        sockaddr_u su;
        memcpy(&su, addr, SOCKADDR_LEN(addr));
        auto pit = pair_map_.find(su);
        if (pit != pair_map_.end() && pit->second) {
            pit->second->onStunPacket(data, len, addr);
            return;
        }
        // Broadcast to all sessions for transaction ID matching
        for (auto& kv : ufrag_map_) {
            if (kv.second) {
                kv.second->onStunPacket(data, len, addr);
                return; // First match wins (session checks txn ID internally)
            }
        }
    }
}

void UdpTransport::dispatchChannelData(const uint8_t* data, size_t len, const struct sockaddr* addr) {
    // TURN ChannelData: 4-byte header (channel number + length)
    if (len < 4) return;

    // Route by source address (TURN server)
    sockaddr_u su;
    memcpy(&su, addr, SOCKADDR_LEN(addr));
    auto it = pair_map_.find(su);
    if (it != pair_map_.end() && it->second) {
        it->second->onChannelData(data, len, addr);
        return;
    }

    // Fallback: try all sessions
    for (auto& kv : ufrag_map_) {
        if (kv.second) {
            kv.second->onChannelData(data, len, addr);
            break;
        }
    }
}

void UdpTransport::dispatchData(const uint8_t* data, size_t len, const struct sockaddr* addr) {
    // Route application data by 5-tuple (peer address)
    sockaddr_u su;
    memcpy(&su, addr, SOCKADDR_LEN(addr));
    auto it = pair_map_.find(su);
    if (it != pair_map_.end() && it->second) {
        it->second->onDataPacket(data, len, addr);
    }
}

} // namespace ice
