#ifndef ICE_AGENT_H_
#define ICE_AGENT_H_

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <map>
#include <functional>

#include "ice_config.h"
#include "EventLoopThreadPool.h"
#include "../stun/stun_message.h"
namespace ice {
class IceSession;
class TurnClient;
struct TcpIceConnection;

// IceAgent: Top-level API managing all ICE sessions and transport
class IceAgent {
public:

    // Create agent with optional external event loop
    // If loop is null, creates its own EventLoopThread
    IceAgent();
    ~IceAgent();

    // Configuration (must be called before start())
    void setConfig(const IceConfig& config);
    const IceConfig& config() const { return config_; }
    void setThreadNum(int num) { loops_.setThreadNum(num); }
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

    // Check if running
    bool isRunning() const { return running_; }

    // ---- Transport APIs (used by IceSession) ----
    hio_t* udpIo() const { return udp_io_; }
    // 当hio为nullptr，数据通过relay方式转发，否则通过hio指定的tcp或udp方式转发
    int send(const void* data, size_t len, const struct sockaddr* addr, hio_t* io);

    // TCP connect / send / close
    int connectTcp(const struct sockaddr* addr, IceSession* session);
    void closeTcpConnection(hio_t* io);

    // Session registration (by ufrag)
    void registerSession(const std::string& ufrag, IceSession* session);
    void unregisterSession(const std::string& ufrag);
    IceSession* findSession(const uint8_t* data, size_t len);

    // Register established pair mapping (by address, UDP only)
    void registerPair(const sockaddr_u& addr, IceSession* session);
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

    // TCP callbacks (static trampolines)
    static void onTcpAccept(hio_t* io);
    static void onTcpConnect(hio_t* io);
    static void onTcpRecv(hio_t* io, void* buf, int readbytes);
    static void onTcpClose(hio_t* io);

    void identifyTcpConnection(hio_t* io, const uint8_t* data, size_t len);

    // Extract local ufrag from STUN USERNAME attribute
    static std::string extractLocalUfrag(const uint8_t* data, size_t len);

    IceConfig config_;
    hv::EventLoopThreadPool loops_;

    std::vector<std::shared_ptr<IceSession>> sessions_;
    bool running_ = false;

    std::unordered_map<std::string, IceSession*> ufrag_map_;
    // ---- UDP state ----
    hio_t* udp_io_ = nullptr;
    int udp_port_ = 0;
    sockaddr_u udp_local_addr_;
    std::map<sockaddr_u, IceSession*, SockaddrCompare> pair_map_;
    std::recursive_mutex mutex_;

    // ---- TCP state ----
    hio_t* tcp_listen_io_ = nullptr;
    int tcp_port_ = 0;
    unpack_setting_t tcp_unpack_setting_;
    std::unordered_map<uint32_t, hio_t*> tcp_connections_;
};

} // namespace ice

#endif // ICE_AGENT_H_
