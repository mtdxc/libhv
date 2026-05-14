#ifndef ICE_TCP_TRANSPORT_H_
#define ICE_TCP_TRANSPORT_H_

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

// TCP connection state for ICE
struct TcpIceConnection {
    hio_t* io = nullptr;
    IceSession* session = nullptr;
    std::string ufrag;      // associated ufrag once identified
    bool identified = false; // true after first STUN exchange
};

// TcpTransport: shared TCP listener for all ICE sessions (passive candidates)
class TcpTransport {
public:
    TcpTransport(hv::EventLoopPtr loop);
    ~TcpTransport();

    // Listen on port (0 for ephemeral)
    int listen(const std::string& host, int port);

    // Get listen port
    int port() const { return port_; }

    // Create outgoing TCP connection (for active candidates)
    // Returns connection id or -1 on error
    int connect(const struct sockaddr* addr, IceSession* session);

    // Send data on a specific TCP connection
    int send(hio_t* io, const void* data, size_t len);

    // Session registration
    void registerSession(const std::string& ufrag, IceSession* session);
    void unregisterSession(const std::string& ufrag);

    // Close a specific connection
    void closeConnection(hio_t* io);

    // Close transport
    void close();

    hv::EventLoopPtr loop() const { return loop_; }

private:
    static void onAccept(hio_t* io);
    static void onConnect(hio_t* io);
    static void onRecv(hio_t* io, void* buf, int readbytes);
    static void onClose(hio_t* io);

    void handleRecv(hio_t* io, const uint8_t* data, size_t len);
    void identifyConnection(hio_t* io, const uint8_t* data, size_t len);

    hv::EventLoopPtr loop_;
    hio_t* listen_io_ = nullptr;
    int port_ = 0;

    // Connection tracking (by hio id)
    std::unordered_map<uint32_t, TcpIceConnection> connections_;

    // Session lookup by ufrag
    std::unordered_map<std::string, IceSession*> ufrag_map_;

    // RFC 4571 unpack setting (2-byte big-endian length field)
    unpack_setting_t unpack_setting_;
};

} // namespace ice

#endif // ICE_TCP_TRANSPORT_H_
