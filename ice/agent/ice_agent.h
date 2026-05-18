#ifndef ICE_AGENT_H_
#define ICE_AGENT_H_

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <map>
#include <functional>

#include "EventLoopThread.h"
#include "ice_config.h"
#include "../stun/stun_message.h"
namespace ice {
class IceSession;
class TurnClient;
// ICE Mode
enum class IceMode {
    Full,
    Lite
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

class IDataRecv {
public:
    virtual void onRecvData(const uint8_t* data, size_t len, const sockaddr* addr) = 0;
    virtual void onStunRequest(StunMessage& req, const sockaddr* addr, hio_t* io) = 0;
    virtual void onTcpConnected(hio_t* io) {}
    virtual void onTcpDisconnected(hio_t* io) {}
};

// Compare sockaddr for use as map key
struct SockaddrCompare {
    bool operator()(const sockaddr_u& a, const sockaddr_u& b) const {
        return sockaddr_compare(&a, &b) < 0;
    }
};

// TCP connection state for ICE
struct TcpIceConnection {
    hio_t* io = nullptr;
    IDataRecv* session = nullptr;
    std::string ufrag;      // associated ufrag once identified
    bool identified = false; // true after first STUN exchange
};

// STUN Transaction for tracking requests
using StunCallback = std::function<void(StunMessage* resp, int code)>;
struct StunTransaction {
    TransactionId id;
    std::vector<uint8_t> msg;
    uint64_t sentTime = 0;               // ms
    int retransmitCount = 0;
    uint32_t rto = 500;                  // Initial RTO ms
    htimer_t* timer = nullptr;           // Retransmit timer
    StunCallback callback; // Callback on response or timeout
};

// IceAgent: Top-level API managing all ICE sessions and transport
class IceAgent {
    // Transactions
    std::map<TransactionId, StunTransaction> transactions_; // key: hex(transaction_id)
public:
    void StunRequest(const StunMessage& msg, const struct sockaddr* server, hio_t* io, StunCallback callback);
    void addTransaction(const TransactionId& id, StunCallback callback);

    // Create agent with optional external event loop
    // If loop is null, creates its own EventLoopThread
    explicit IceAgent(hv::EventLoopPtr loop = nullptr);
    ~IceAgent();

    // Configuration (must be called before start())
    void setConfig(const IceConfig& config);
    const IceConfig& config() const { return config_; }

    // Start transport (bind ports)
    // Returns 0 on success, <0 on error
    int start();

    // Stop transport and all sessions
    void stop();

    // Create a new ICE session
    std::shared_ptr<IceSession> createSession(IceMode mode = IceMode::Full);

    // Destroy a session
    void destroySession(const std::shared_ptr<IceSession>& session);

    // Get bound ports
    int udpPort() const { return udp_port_; }
    int tcpPort() const { return tcp_port_; }

    // Get local address
    struct sockaddr* udpLocalAddr() { return (struct sockaddr*)&udp_local_addr_; }

    // Get event loop
    hv::EventLoopPtr loop() const { return loop_; }

    // Check if running
    bool isRunning() const { return running_; }

    // ---- Transport APIs (used by IceSession) ----
    hio_t* udpIo() const { return udp_io_; }
    // hio send(tcp or udp or relay)
    int send(const void* data, size_t len, const struct sockaddr* addr, hio_t* io);

    // TCP connect / send / close
    int connectTcp(const struct sockaddr* addr, IDataRecv* session);
    void closeTcpConnection(hio_t* io);

    // Session registration (by ufrag)
    void registerSession(const std::string& ufrag, IDataRecv* session);
    void unregisterSession(const std::string& ufrag);

    // Register established pair mapping (by address, UDP only)
    void registerPair(const sockaddr_u& addr, IDataRecv* session);
    void unregisterPair(const sockaddr_u& addr);

    // ---- TURN relay APIs (used by IceSession) ----
    bool isTurnAllocated() const;
    bool isTurnAllocating() const;
    TurnClient* turnClient() const { return turn_client_.get(); }
    void addTurnCandidates(IceSession* session, int componentId = 1);
    void addHostCandidates(IceSession* session, int componentId = 1);

    void createTurnPermission(const struct sockaddr* peerAddr);
    // Start TURN allocation (lazy, called by IceSession during gathering)
    void allocateTurn();

private:
    // ---- TURN state ----
    std::shared_ptr<TurnClient> turn_client_;

    // UDP callbacks
    void onRecvPdu(const uint8_t* data, size_t len, const struct sockaddr* addr, hio_t* io);
    void processStunMsg(const uint8_t* data, size_t len, const struct sockaddr* addr, hio_t* io);

    // TCP callbacks (static trampolines)
    static void onTcpAccept(hio_t* io);
    static void onTcpConnect(hio_t* io);
    static void onTcpRecv(hio_t* io, void* buf, int readbytes);
    static void onTcpClose(hio_t* io);

    void handleTcpRecv(hio_t* io, const uint8_t* data, size_t len);
    void identifyTcpConnection(hio_t* io, const uint8_t* data, size_t len);

    // Extract local ufrag from STUN USERNAME attribute
    static std::string extractLocalUfrag(const uint8_t* data, size_t len);

    IceConfig config_;
    hv::EventLoopPtr loop_;
    std::unique_ptr<hv::EventLoopThread> loop_thread_; // owned if no external loop

    std::vector<std::shared_ptr<IceSession>> sessions_;
    bool running_ = false;

    std::unordered_map<std::string, IDataRecv*> ufrag_map_;
    // ---- UDP state ----
    hio_t* udp_io_ = nullptr;
    int udp_port_ = 0;
    sockaddr_u udp_local_addr_;
    std::map<sockaddr_u, IDataRecv*, SockaddrCompare> pair_map_;

    // ---- TCP state ----
    hio_t* tcp_listen_io_ = nullptr;
    int tcp_port_ = 0;
    unpack_setting_t tcp_unpack_setting_;
    std::unordered_map<uint32_t, TcpIceConnection> tcp_connections_;
};

} // namespace ice

#endif // ICE_AGENT_H_
