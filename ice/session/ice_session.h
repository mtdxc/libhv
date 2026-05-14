#ifndef ICE_SESSION_H_
#define ICE_SESSION_H_

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

#include "EventLoop.h"

#include "../candidate/ice_candidate.h"
#include "../candidate/candidate_pair.h"
#include "ice_checklist.h"
#include "../stun/stun_message.h"

namespace ice {

class UdpTransport;
class TcpTransport;
class TurnClient;

// ICE Mode
enum class IceMode {
    Full,
    Lite
};

// ICE State
enum class IceState {
    New,
    Gathering,
    Checking,
    Connected,
    Completed,
    Failed,
    Closed
};

// Nomination mode
enum class NominationMode {
    Regular,
    Aggressive
};

const char* iceStateString(IceState state);

// STUN Transaction for tracking requests
struct StunTransaction {
    TransactionId id;
    CandidatePair* pair = nullptr;       // Associated candidate pair (for connectivity checks)
    struct sockaddr_storage destAddr;     // Where the request was sent
    uint64_t sentTime = 0;               // ms
    int retransmitCount = 0;
    uint32_t rto = 500;                  // Initial RTO ms
    htimer_t* timer = nullptr;           // Retransmit timer
    bool isGathering = false;            // STUN binding for gathering (not connectivity check)
    std::string serverAddr;              // STUN/TURN server address (for gathering)
};

// ICE Session - one per peer connection component
class IceSession : public std::enable_shared_from_this<IceSession> {
public:
    IceSession(IceMode mode, hv::EventLoopPtr loop);
    ~IceSession();

    // Configuration
    void setRole(IceRole role) { role_ = role; }
    IceRole role() const { return role_; }
    void setNomination(NominationMode mode) { nomination_ = mode; }
    void setTiebreaker(uint64_t tb) { tiebreaker_ = tb; }

    // Transport association
    void setUdpTransport(UdpTransport* transport) { udp_transport_ = transport; }
    void setTcpTransport(TcpTransport* transport) { tcp_transport_ = transport; }

    // Credentials
    std::string localUfrag() const { return local_ufrag_; }
    std::string localPwd() const { return local_pwd_; }
    void setRemoteCredentials(const std::string& ufrag, const std::string& pwd);
    std::string remoteUfrag() const { return remote_ufrag_; }
    std::string remotePwd() const { return remote_pwd_; }

    // State
    IceState state() const { return state_; }
    IceMode mode() const { return mode_; }

    // Candidate management
    void addLocalCandidate(const IceCandidate& candidate);
    void addRemoteCandidate(const IceCandidate& candidate);
    void setRemoteCandidatesDone();
    const std::vector<IceCandidate>& localCandidates() const { return local_candidates_; }
    const std::vector<IceCandidate>& remoteCandidates() const { return remote_candidates_; }

    // Gathering
    void gatherCandidates();

    // Start connectivity checks
    void startChecks();

    // Send data (post-ICE-completion)
    int send(const void* data, size_t len);

    // Get selected pair
    const CandidatePair* selectedPair() const { return selected_pair_; }

    // Packet handlers (called by transport layer)
    void onStunPacket(const uint8_t* data, size_t len, const struct sockaddr* from);
    void onDataPacket(const uint8_t* data, size_t len, const struct sockaddr* from);
    void onChannelData(const uint8_t* data, size_t len, const struct sockaddr* from);
    void onTcpConnected(hio_t* io);
    void onTcpDisconnected(hio_t* io);

    // Callbacks
    std::function<void(IceState)> onStateChange;
    std::function<void(const IceCandidate&)> onLocalCandidate;
    std::function<void(const CandidatePair&)> onSelectedPair;
    std::function<void(const void*, size_t)> onData;

    // Close session
    void close();

private:
    // State management
    void setState(IceState state);

    // STUN request handling
    void handleStunRequest(const StunMessage& msg, const struct sockaddr* from);
    void handleStunResponse(const StunMessage& msg, const struct sockaddr* from);
    void handleStunErrorResponse(const StunMessage& msg, const struct sockaddr* from);

    // Connectivity checks
    void sendConnectivityCheck(CandidatePair* pair);
    void onCheckSuccess(CandidatePair* pair, const StunMessage& response);
    void setSelectPair(ice::CandidatePair* pair);
    void onCheckFailure(CandidatePair* pair, uint16_t errorCode);
    void scheduleNextCheck();
    void onCheckTimer();

    // Nomination
    void nominate(CandidatePair* pair);
    void checkNominationComplete();

    // Gathering helpers
    void gatherHostCandidates();
    void sendStunBindingRequest(const struct sockaddr* server, const std::string& serverStr);
    void onGatheringResponse(const StunMessage& msg, const std::string& serverAddr);
    void onGatheringComplete();

    // Role conflict handling
    void handleRoleConflict(bool isControlling);

    // Send STUN response
    void sendStunResponse(const StunMessage& request, const struct sockaddr* to);
    void sendStunErrorResponse(const StunMessage& request, uint16_t code,
                               const std::string& reason, const struct sockaddr* to);

    // Transaction management
    void addTransaction(const StunTransaction& txn);
    void removeTransaction(const TransactionId& id);
    StunTransaction* findTransaction(const TransactionId& id);

    // Form candidate pairs
    void formPairs();

    // Keepalive
    void startKeepalive();
    void sendKeepalive();

    // Members
    IceMode mode_;
    IceState state_ = IceState::New;
    IceRole role_ = IceRole::Controlling;
    NominationMode nomination_ = NominationMode::Regular;
    uint64_t tiebreaker_ = 0;

    hv::EventLoopPtr loop_;
    UdpTransport* udp_transport_ = nullptr;
    TcpTransport* tcp_transport_ = nullptr;

    // Credentials
    std::string local_ufrag_;
    std::string local_pwd_;
    std::string remote_ufrag_;
    std::string remote_pwd_;

    // Candidates
    std::vector<IceCandidate> local_candidates_;
    std::vector<IceCandidate> remote_candidates_;
    bool remote_candidates_done_ = false;

    // Check list
    IceCheckList checklist_;
    CandidatePair* selected_pair_ = nullptr;

    // Timers
    htimer_t* check_timer_ = nullptr;
    htimer_t* keepalive_timer_ = nullptr;
    int check_interval_ms_ = 50; // Ta

    // Transactions
    std::unordered_map<std::string, StunTransaction> transactions_; // key: hex(transaction_id)

    // Gathering state
    int pending_gathering_requests_ = 0;

    // TURN client (optional)
    std::shared_ptr<TurnClient> turn_client_;
};

using IceSessionPtr = std::shared_ptr<IceSession>;

} // namespace ice

#endif // ICE_SESSION_H_
