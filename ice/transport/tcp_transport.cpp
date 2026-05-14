#include "tcp_transport.h"
#include "udp_transport.h" // for classifyPacket, extractLocalUfrag pattern
#include "../stun/stun_message.h"
#include "../session/ice_session.h"

namespace ice {

TcpTransport::TcpTransport(hv::EventLoopPtr loop)
    : loop_(loop) {
    memset(&unpack_setting_, 0, sizeof(unpack_setting_t));
    unpack_setting_.mode = UNPACK_BY_LENGTH_FIELD;
    unpack_setting_.package_max_length = DEFAULT_PACKAGE_MAX_LENGTH;
    unpack_setting_.body_offset = 2;
    unpack_setting_.length_field_offset = 0;
    unpack_setting_.length_field_bytes = 2;
    unpack_setting_.length_field_coding = ENCODE_BY_BIG_ENDIAN;
    unpack_setting_.length_adjustment = 0;
}

TcpTransport::~TcpTransport() {
    close();
}

int TcpTransport::listen(const std::string& host, int port) {
    hloop_t* loop = loop_->loop();
    if (!loop) return -1;

    listen_io_ = hloop_create_tcp_server(loop, host.c_str(), port, onAccept);
    if (!listen_io_) return -1;

    hevent_set_userdata(listen_io_, this);

    port_ = sockaddr_port((sockaddr_u*)hio_localaddr(listen_io_)); 
    return port_;
}

int TcpTransport::connect(const struct sockaddr* addr, IceSession* session) {
    hloop_t* loop = loop_->loop();
    if (!loop) return -1;

    // Determine port and host from sockaddr
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
    conn.identified = true; // We know the session for outgoing connections
    uint32_t id = hio_id(io);
    connections_[id] = conn;

    hevent_set_userdata(io, this);
    hio_setcb_connect(io, onConnect);
    hio_setcb_read(io, onRecv);
    hio_setcb_close(io, onClose);
    hio_set_unpack(io, &unpack_setting_);
    hio_connect(io);

    return (int)id;
}

int TcpTransport::send(hio_t* io, const void* data, size_t len) {
    if (!io) return -1;
    // TCP needs framing: prepend 2-byte length (RFC 4571 style)
    uint8_t header[2];
    header[0] = (uint8_t)((len >> 8) & 0xFF);
    header[1] = (uint8_t)(len & 0xFF);
    hio_write(io, header, 2);
    return hio_write(io, data, len);
}

void TcpTransport::registerSession(const std::string& ufrag, IceSession* session) {
    loop_->runInLoop([this, ufrag, session]() {
        ufrag_map_[ufrag] = session;
    });
}

void TcpTransport::unregisterSession(const std::string& ufrag) {
    loop_->runInLoop([this, ufrag]() {
        ufrag_map_.erase(ufrag);
    });
}

void TcpTransport::closeConnection(hio_t* io) {
    if (io) {
        hio_close(io);
    }
}

void TcpTransport::close() {
    // Only call hio_close if loop is still valid (not yet freed)
    bool loopValid = loop_ && loop_->loop();

    // Close all connections
    if (loopValid) {
        for (auto& kv : connections_) {
            if (kv.second.io) {
                hio_close(kv.second.io);
            }
        }
    }
    connections_.clear();

    if (listen_io_) {
        if (loopValid) {
            hio_close(listen_io_);
        }
        listen_io_ = nullptr;
    }
    ufrag_map_.clear();
}

void TcpTransport::onAccept(hio_t* io) {
    TcpTransport* self = (TcpTransport*)hevent_userdata(io);
    if (!self) {
        hio_close(io);
        return;
    }

    // New incoming connection
    TcpIceConnection conn;
    conn.io = io;
    conn.session = nullptr;
    conn.identified = false;
    uint32_t id = hio_id(io);
    self->connections_[id] = conn;

    hevent_set_userdata(io, self);
    hio_setcb_read(io, onRecv);
    hio_setcb_close(io, onClose);
    hio_set_unpack(io, &self->unpack_setting_);
    hio_read(io);
}

void TcpTransport::onConnect(hio_t* io) {
    TcpTransport* self = (TcpTransport*)hevent_userdata(io);
    if (!self) return;

    // Connection established, start reading
    hio_read(io);

    // Notify session
    uint32_t id = hio_id(io);
    auto it = self->connections_.find(id);
    if (it != self->connections_.end() && it->second.session) {
        it->second.session->onTcpConnected(io);
    }
}

void TcpTransport::onRecv(hio_t* io, void* buf, int readbytes) {
    TcpTransport* self = (TcpTransport*)hevent_userdata(io);
    if (!self) return;
    // Skip 2-byte RFC 4571 length header (unpack callback includes full frame)
    if (readbytes <= 2) return;
    self->handleRecv(io, (const uint8_t*)buf + 2, readbytes - 2);
}

void TcpTransport::onClose(hio_t* io) {
    TcpTransport* self = (TcpTransport*)hevent_userdata(io);
    if (!self) return;

    uint32_t id = hio_id(io);
    auto it = self->connections_.find(id);
    if (it != self->connections_.end()) {
        if (it->second.session) {
            it->second.session->onTcpDisconnected(io);
        }
        self->connections_.erase(it);
    }
}

void TcpTransport::handleRecv(hio_t* io, const uint8_t* data, size_t len) {
    uint32_t id = hio_id(io);
    auto it = connections_.find(id);
    if (it == connections_.end()) return;

    if (!it->second.identified) {
        // Try to identify which session this connection belongs to
        identifyConnection(io, data, len);
        return;
    }

    // Dispatch to associated session
    if (it->second.session) {
        PacketType ptype = classifyPacket(data, len);
        if (ptype == PacketType::STUN) {
            it->second.session->onStunPacket(data, len, hio_peeraddr(io));
        } else {
            it->second.session->onDataPacket(data, len, hio_peeraddr(io));
        }
    }
}

void TcpTransport::identifyConnection(hio_t* io, const uint8_t* data, size_t len) {
    // First packet on unidentified connection should be STUN
    PacketType ptype = classifyPacket(data, len);
    if (ptype != PacketType::STUN) {
        // Cannot identify, close connection
        hio_close(io);
        return;
    }

    // Parse USERNAME to get local ufrag
    // Quick parse without full decode
    if (len < 20) return;
    size_t offset = 20;
    uint16_t msg_len = ((uint16_t)data[2] << 8) | data[3];
    size_t end = 20 + msg_len;
    if (end > len) end = len;

    std::string local_ufrag;
    while (offset + 4 <= end) {
        uint16_t attr_type = ((uint16_t)data[offset] << 8) | data[offset + 1];
        uint16_t attr_len = ((uint16_t)data[offset + 2] << 8) | data[offset + 3];
        if (attr_type == 0x0006) { // USERNAME
            if (offset + 4 + attr_len <= end) {
                std::string username((const char*)data + offset + 4, attr_len);
                size_t colon = username.find(':');
                if (colon != std::string::npos) {
                    local_ufrag = username.substr(0, colon);
                }
            }
            break;
        }
        offset += 4 + ((attr_len + 3) & ~3);
    }

    if (local_ufrag.empty()) return;

    // Find session by ufrag
    auto sit = ufrag_map_.find(local_ufrag);
    if (sit == ufrag_map_.end() || !sit->second) {
        hio_close(io);
        return;
    }

    // Associate connection with session
    uint32_t id = hio_id(io);
    auto it = connections_.find(id);
    if (it != connections_.end()) {
        it->second.session = sit->second;
        it->second.ufrag = local_ufrag;
        it->second.identified = true;
    }

    // Dispatch the first STUN packet
    sit->second->onStunPacket(data, len, hio_peeraddr(io));
}

} // namespace ice
