#ifndef ICE_TURN_CLIENT_H_
#define ICE_TURN_CLIENT_H_

#include <string>
#include <memory>
#include <functional>
#include <vector>
#include <unordered_map>

#include "EventLoop.h"
#include "hsocket.h"

#include "../stun/stun_message.h"
#include "../agent/ice_agent.h"

namespace ice {

class IceAgent;

// TURN allocation state
enum class TurnState {
    Idle,
    Allocating,
    Allocated,
    Refreshing,
    Failed
};

// TURN permission entry
struct TurnPermission {
    sockaddr_u peerAddr;
    uint64_t expireTime; // ms
};

// TURN channel binding
struct TurnChannelBinding {
    uint16_t channelNumber;
    sockaddr_u peerAddr;
    uint64_t expireTime; // ms
};

// TURN Client (RFC 5766)
class TurnClient : public IDataRecv, public std::enable_shared_from_this<TurnClient> {
public:
    TurnClient(hv::EventLoopPtr loop, IceAgent* transport);
    ~TurnClient();

    // Configuration
    void setServer(const struct sockaddr* serverAddr);
    void setCredentials(const std::string& username, const std::string& password);

    // Allocation
    void allocate();
    void refresh(uint32_t lifetime = 600);
    void deallocate();

    // Permissions
    void createPermission(const struct sockaddr* peerAddr);

    // Channel binding
    void channelBind(const struct sockaddr* peerAddr, uint16_t channelNumber);

    // Send data via TURN
    int sendData(const void* data, size_t len, const struct sockaddr* peerAddr);
    int sendChannelData(const void* data, size_t len, uint16_t channelNumber);

    // State
    TurnState state() const { return state_; }
    bool isAllocated() const { return state_ == TurnState::Allocated; }

    // Get relay address (allocated by TURN server)
    const sockaddr_u& relayAddr() const { return relay_addr_; }
    const sockaddr_u& serverReflexiveAddr() const { return srflx_addr_; }
    const sockaddr_u& serverAddr() const { return server_addr_; }

    // Handle incoming STUN messages from TURN server
    void onStunMessage(const StunMessage& msg, const struct sockaddr* from);
    void onChannelData(const uint8_t* data, size_t len);
    void onRecvData(const uint8_t* data, size_t len, const struct sockaddr* addr) override;

    // Callbacks
    std::function<void(TurnState)> onStateChange;
    std::function<void(const void*, size_t, const struct sockaddr*)> onData; // Data from peer via TURN

private:
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
    IceAgent* transport_;
    TurnState state_ = TurnState::Idle;

    // Server info
    sockaddr_u server_addr_;
    std::string username_;
    std::string password_;

    // Authentication (long-term credentials)
    std::string realm_;
    std::string nonce_;

    // Allocated addresses
    sockaddr_u relay_addr_;
    sockaddr_u srflx_addr_;
    uint32_t lifetime_ = 600; // seconds

    // Permissions
    std::vector<TurnPermission> permissions_;

    // Channel bindings
    std::unordered_map<uint16_t, TurnChannelBinding> channels_;
    uint16_t next_channel_ = 0x4000; // Channel numbers range: 0x4000-0x7FFE

    // Timers
    htimer_t* refresh_timer_ = nullptr;
    htimer_t* permission_timer_ = nullptr;

    // Pending transactions
    std::unordered_map<std::string, TransactionId> pending_transactions_;
};

using TurnClientPtr = std::shared_ptr<TurnClient>;

} // namespace ice

#endif // ICE_TURN_CLIENT_H_
