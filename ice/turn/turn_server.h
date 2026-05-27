#ifndef ICE_TURN_SERVER_H_
#define ICE_TURN_SERVER_H_

#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <map>
#include <vector>
#include <cstdint>
#include <mutex>
#include <list>
#include "EventLoopThreadPool.h"
#include "hsocket.h"

#include "../stun/stun_message.h"

namespace ice {

class TurnServer;

// ────────────────────────────────────────────────────────────
// TURN server configuration
// ────────────────────────────────────────────────────────────
struct TurnServerOptions {
    std::string bindHost = "0.0.0.0";
    int  udpPort         = 3478;
    int  tcpPort         = 3478;

    // Authentication: long-term credential mechanism (RFC 5389 §10.2)
    std::string realm; // e.g. "example.com"

    // Software string for STUN SOFTWARE attribute
    std::string software = "libhv-ice/1.0";

    // User database: username -> password (plain text)
    // In production replace with a proper lookup callback.
    std::unordered_map<std::string, std::string> users;
};

// ────────────────────────────────────────────────────────────
// Per-allocation data
// ────────────────────────────────────────────────────────────
struct TurnAllocation : public std::enable_shared_from_this<TurnAllocation> {
    hv::EventLoopPtr loop;
    std::string allocKey;

    sockaddr_u  clientAddr;  // who did ALLOCATE
    hio_t*      clientIo;    // IO channel toward the client

    sockaddr_u  relayAddr;   // UDP relay endpoint bound by the server
    hio_t*      relayIo;     // hio_t for the relay UDP socket

    uint32_t    lifetime;    // seconds
    uint64_t    expireTime;  // ms (hloop_now_ms)

    std::string username;
    std::string realm;
    std::string nonce;

    // Permissions: peer IP -> expiry (ms)
    std::map<sockaddr_u, uint64_t, SockaddrCompare> permissions;

    // Channel bindings: channel number -> peer addr
    struct ChannelEntry {
        uint16_t   channelNumber;
        std::shared_ptr<sockaddr_u> peerAddr;
        uint64_t   expireTime; // ms
    };
    std::unordered_map<uint16_t, ChannelEntry> channels;
    // Reverse map: peer addr -> channel number
    std::map<sockaddr_u, uint16_t, SockaddrCompare> peerToChannel;
public:
    ~TurnAllocation() { close(); }
    void forwardData(const void* data, size_t len, std::shared_ptr<sockaddr_u> to);
    void onRelayRecv(const void* data, size_t len, const struct sockaddr* from, hio_t* relayIo);
    void bindRelaySocket(std::string host, int port);
    void close();
    void addChannelBinding(uint16_t channel, std::shared_ptr<sockaddr_u> peerAddr);
};

// ────────────────────────────────────────────────────────────
// TurnServer
//
// Minimal RFC 5766 TURN server built on libhv event loop.
// Supports UDP transport.  TCP transport can be added by setting
// enableTcp = true (skeleton provided).
// ────────────────────────────────────────────────────────────
class TurnServer {
public:
    explicit TurnServer();
    ~TurnServer();

    // Must be called before start()
    void setOptions(const TurnServerOptions& opts);
    const TurnServerOptions& options() const { return opts_; }

    // Bind ports and start serving
    int  start(int threadNum = 0);
    void stop();

    bool isRunning() const { return running_; }
    int  udpPort()   const { return udp_port_; }
    int  tcpPort()   const { return tcp_port_; }

    // Optional callback: called whenever a relay is allocated/freed
    std::function<void(const TurnAllocation&, bool /*allocated*/)> onAllocationChanged;

private:
    // ---- Incoming packet dispatch ----
    void onRecvPdu(const uint8_t* data, size_t len,
                   const struct sockaddr* from, hio_t* io);

    // ---- STUN method handlers ----
    void handleAllocate       (const StunMessage& req, const struct sockaddr* from, hio_t* io);
    void handleRefresh        (const StunMessage& req, const struct sockaddr* from, hio_t* io);
    void handleCreatePermission(const StunMessage& req, const struct sockaddr* from, hio_t* io);
    void handleChannelBind    (const StunMessage& req, const struct sockaddr* from, hio_t* io);
    void handleSendIndication (const StunMessage& req, const struct sockaddr* from, hio_t* io);
    void handleBinding        (const StunMessage& req, const struct sockaddr* from, hio_t* io);

    // ---- Authentication helpers ----
    // Returns true and fills password if credentials are valid.
    bool authenticate(const StunMessage& msg,
                      const struct sockaddr* from, hio_t* io,
                      std::string* outUsername);
    // Generate a fresh nonce
    static std::string generateNonce();
    // Verify long-term HMAC-SHA1 integrity
    bool verifyLongTermAuth(const StunMessage& msg,
                            const std::string& username,
                            const std::string& realm,
                            const std::string& password) const;

    // ---- Response helpers ----
    void sendSuccess(const StunMessage& req,
                     const struct sockaddr* to, hio_t* io);
    void sendError(const StunMessage& req, uint16_t code,
                   const std::string& reason,
                   const struct sockaddr* to, hio_t* io,
                   bool addAuth = false);

    // ---- Allocation management ----
    // Key: client address string (for UDP) or fd (for TCP)
    using AllocKey = std::string;
    AllocKey makeKey(const struct sockaddr* addr, hio_t* io) const;

    std::shared_ptr<TurnAllocation> findAllocation(const AllocKey& key);
    std::list<std::shared_ptr<TurnAllocation>> getLoopAllocations(const hv::EventLoopPtr& loop);

    void removeAllocation(const AllocKey& key);
    void removeAllocationInLoop(const std::shared_ptr<TurnAllocation>& alloc);
    void startExpirySweep(const hv::EventLoopPtr& loop);
    void sweepExpiredAllocations(const hv::EventLoopPtr& loop);
    // ---- Channel data ----
    void handleChannelData(const uint8_t* data, size_t len,
                           const struct sockaddr* from, hio_t* io);

    // ---- TCP (skeleton) ----
    static void onTcpAccept(hio_t* io);
    static void onTcpRecv (hio_t* io, void* buf, int readbytes);
    static void onTcpClose(hio_t* io);

    // ---- Packet send helper ----
    int sendTo(const void* data, size_t len,
               const struct sockaddr* to, hio_t* io);

    // ---- State ----
    TurnServerOptions opts_;
    hv::EventLoopThreadPool thread_pool_;

    bool running_   = false;
    int  udp_port_  = 0;
    int  tcp_port_  = 0;

    hio_t* udp_io_       = nullptr;
    hio_t* tcp_listen_io_= nullptr;

    // Active allocations keyed by AllocKey
    std::unordered_map<AllocKey, std::shared_ptr<TurnAllocation>> allocations_;
    std::mutex allocations_mutex_;

    // Nonce set (nonces currently issued but not yet consumed)
    // value = expiry time ms
    std::unordered_map<std::string, uint64_t> nonces_;

};

} // namespace ice

#endif // ICE_TURN_SERVER_H_
