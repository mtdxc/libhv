#ifndef ICE_UDP_TRANSPORT_H_
#define ICE_UDP_TRANSPORT_H_

#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <map>
#include <mutex>

#include "hloop.h"
#include "hsocket.h"
#include "EventLoopThread.h"
#include "Channel.h"

namespace ice {

class IceSession;

// Compare sockaddr for use as map key
struct SockaddrCompare {
    bool operator()(const sockaddr_u& a, const sockaddr_u& b) const {
        if (a.sa.sa_family != b.sa.sa_family) {
            return a.sa.sa_family < b.sa.sa_family;
        }
        if (a.sa.sa_family == AF_INET) {
            if (a.sin.sin_addr.s_addr != b.sin.sin_addr.s_addr)
                return a.sin.sin_addr.s_addr < b.sin.sin_addr.s_addr;
            return a.sin.sin_port < b.sin.sin_port;
        } else if (a.sa.sa_family == AF_INET6) {
            int cmp = memcmp(&a.sin6.sin6_addr, &b.sin6.sin6_addr, 16);
            if (cmp != 0) return cmp < 0;
            return a.sin6.sin6_port < b.sin6.sin6_port;
        }
        return false;
    }
};

// Packet type classification
enum class PacketType {
    STUN,           // STUN message (first byte 0x00 or 0x01)
    TURN_CHANNEL,   // TURN ChannelData (first byte 0x40-0x7F)
    DATA            // Application data (other)
};

// Classify incoming packet
inline PacketType classifyPacket(const uint8_t* data, size_t len) {
    if (len < 1) return PacketType::DATA;
    uint8_t first = data[0];
    if ((first & 0xC0) == 0x00) return PacketType::STUN;      // 0x00-0x3F
    if (first >= 0x40 && first <= 0x7F) return PacketType::TURN_CHANNEL;
    return PacketType::DATA;
}

// Callback for receiving data
using UdpRecvCallback = std::function<void(const uint8_t* data, size_t len, const struct sockaddr* addr)>;

// UdpTransport: single shared UDP socket for all ICE sessions
class UdpTransport {
public:
    UdpTransport(hv::EventLoopPtr loop);
    ~UdpTransport();

    // Bind to port (0 for ephemeral)
    int bind(const std::string& host, int port);

    // Get bound port
    int port() const { return port_; }

    // Get local address
    struct sockaddr* localAddr() { return (struct sockaddr*)&local_addr_; }

    // Send data to peer
    int sendTo(const void* data, size_t len, const struct sockaddr* addr);

    // Session registration (by ufrag)
    void registerSession(const std::string& ufrag, IceSession* session);
    void unregisterSession(const std::string& ufrag);

    // Register established pair mapping (by address)
    void registerPair(const sockaddr_u& addr, IceSession* session);
    void unregisterPair(const sockaddr_u& addr);

    // Close transport
    void close();

    // Get event loop
    hv::EventLoopPtr loop() const { return loop_; }

private:
    void onRecv(const uint8_t* data, size_t len, const struct sockaddr* addr);
    void dispatchStun(const uint8_t* data, size_t len, const struct sockaddr* addr);
    void dispatchChannelData(const uint8_t* data, size_t len, const struct sockaddr* addr);
    void dispatchData(const uint8_t* data, size_t len, const struct sockaddr* addr);

    // Extract local ufrag from STUN USERNAME attribute
    // USERNAME format: "local_ufrag:remote_ufrag"
    static std::string extractLocalUfrag(const uint8_t* data, size_t len);

    hv::EventLoopPtr loop_;
    hio_t* io_ = nullptr;
    int port_ = 0;
    sockaddr_u local_addr_;

    // Demux tables (accessed only from event loop thread)
    std::unordered_map<std::string, IceSession*> ufrag_map_;
    std::map<sockaddr_u, IceSession*, SockaddrCompare> pair_map_;
};

} // namespace ice

#endif // ICE_UDP_TRANSPORT_H_
