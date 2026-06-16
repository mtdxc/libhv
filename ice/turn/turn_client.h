#ifndef ICE_TURN_CLIENT_H_
#define ICE_TURN_CLIENT_H_

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "EventLoop.h"
#include "hsocket.h"

#include "../stun/stun_message.h"
#include "../agent/ice_config.h"

namespace ice {

// TURN allocation state
enum class TurnState {
    Idle,
    Allocating,
    Allocated,
    Refreshing,
    Failed
};

const char* turnStateString(TurnState state);

// TURN channel binding
struct TurnChannelBinding {
    uint16_t channelNumber;
    sockaddr_u peerAddr;
    uint64_t expireTime; // ms
};

// TURN Client (RFC 5766)
class TurnClient : public std::enable_shared_from_this<TurnClient> {
public:
    TurnClient(hv::EventLoopPtr loop, const TurnServerConfig& config);
    ~TurnClient();

    // Configuration
    const TurnServerConfig& config() const { return config_; }
    const char* id() const;

    // Allocation
    void allocate();
    void refresh(uint32_t lifetime = 600);
    void deallocate();

    // Permissions
    void createPermission(const struct sockaddr* peerAddr);

    // Channel binding
    void channelBind(const struct sockaddr* peerAddr, uint16_t channelNumber = 0 /* auto generatte with next_channel_ */);
    uint16_t nextChannelNumber();
    // Send data via TURN
    int sendData(const void* data, size_t len, const struct sockaddr* peerAddr);
    int sendChannelData(const void* data, size_t len, uint16_t channelNumber);

    // State
    TurnState state() const { return state_; }
    bool isAllocated() const { return state_ == TurnState::Allocated; }

    // Get relay address (allocated by TURN server)
    const sockaddr_u& relayAddr() const { return relay_addr_; }
    const sockaddr_u& serverReflexiveAddr() const { return srflx_addr_; }
    std::string serverAddr() const { return config_.addr.toString(); }

    // Handle incoming STUN messages from TURN server
    void onStunMessage(StunMessage& msg);
    void onRecvPdu(const uint8_t* data, size_t len);

    void onChannelData(const uint8_t* data, size_t len);

    // Callbacks
    std::function<void(TurnState)> onStateChange;
    std::function<void(const void*, size_t, const struct sockaddr*)> onData; // Data from peer via TURN

private:
    std::string tcp_buff_;
    int connectTcp();
    // 由于turn的特殊性：只有stun消息和channel data
    // 完全可以从两种包头部的长度字段来判断长度，@see TurnTcpLength 实现
    // 因此Turn tcp的分包和常规stun tcp的分包不一样，Tcp头部没两字节的长度字段!
    void onTcpRecv(const void* data, int len);
    void onTcpConnected(hio_t* io);
    void onTcpDisconnected(hio_t* io);

    void setState(TurnState s, const char* reason);
    void sendAllocateRequest();
    void sendAllocateRequestWithAuth();
    void handleAllocateResponse(const StunMessage& msg);
    void handleAllocateError(const StunMessage& msg);
    void handleRefreshResponse(const StunMessage& msg);
    void handleCreatePermissionResponse(const StunMessage& msg);
    void handleChannelBindResponse(const StunMessage& msg);
    void handleDataIndication(const StunMessage& msg);

    void startRefreshTimer();
    void startPermissionRefreshTimer();

    hv::EventLoopPtr loop_;
    hio_t* io_ = nullptr;
    TurnState state_ = TurnState::Idle;

    // Server info
    TurnServerConfig config_;

    // Authentication (long-term credentials)
    std::string realm_;
    std::string nonce_;

    // Allocated addresses
    sockaddr_u relay_addr_;
    sockaddr_u srflx_addr_;
    uint32_t lifetime_ = 600; // seconds

    // Permissions addr-> expireTime
    std::map<sockaddr_u, uint64_t, SockaddrCompare> permissions_;

    // Channel bindings
    std::unordered_map<uint16_t, TurnChannelBinding> channels_;
    uint16_t next_channel_ = 0x4000; // Channel numbers range: 0x4000-0x7FFE

    // Timers
    htimer_t* refresh_timer_ = nullptr;
    htimer_t* permission_timer_ = nullptr;

};

using TurnClientPtr = std::shared_ptr<TurnClient>;

} // namespace ice

#endif // ICE_TURN_CLIENT_H_
