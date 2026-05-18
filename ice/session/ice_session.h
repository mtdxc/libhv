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
#include "../agent/ice_agent.h"

namespace ice {

// TURN allocation state (forward declaration from turn_client.h)
enum class TurnState;

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


// ICE Session - one per peer connection component
class IceSession : public IDataRecv, public std::enable_shared_from_this<IceSession> {
public:
    IceSession(IceMode mode, IceAgent* agent, hv::EventLoopPtr loop);
    ~IceSession();

    // Configuration
    void setRole(IceRole role) { role_ = role; }
    IceRole role() const { return role_; }
    void setNomination(NominationMode mode) { nomination_ = mode; }
    void setTiebreaker(uint64_t tb) { tiebreaker_ = tb; }

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

    // TURN state notification (called by IceAgent)
    void onTurnStateChanged(TurnState state);

    // Packet handlers (called by transport layer)
    void onRecvData(const uint8_t* data, size_t len, const struct sockaddr* from) override;
    // STUN request handling
    void onStunRequest(StunMessage& req, const sockaddr* addr, hio_t* io) override;
    void onTcpConnected(hio_t* io) override;
    void onTcpDisconnected(hio_t* io) override;

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
    void sendStunResponse(const StunMessage& request, const struct sockaddr* to, hio_t* io);
    void sendStunErrorResponse(const StunMessage& request, uint16_t code, const std::string& reason, 
                               const struct sockaddr* to, hio_t* io);

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
    IceAgent* agent_ = nullptr;

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


    // Gathering state
    int pending_gathering_requests_ = 0;
};

using IceSessionPtr = std::shared_ptr<IceSession>;

} // namespace ice

#endif // ICE_SESSION_H_
