#include "ice_session.h"
#include "../agent/ice_agent.h"
#include "../stun/stun_auth.h"
#include "../turn/turn_client.h"
#include "hloop.h"

#include <cstdlib>
#include <ctime>
#include <memory>
#include <sstream>
#include <iomanip>

namespace ice {

const char* iceStateString(IceState state) {
    switch (state) {
    case IceState::New: return "new";
    case IceState::Gathering: return "gathering";
    case IceState::Checking: return "checking";
    case IceState::Connected: return "connected";
    case IceState::Completed: return "completed";
    case IceState::Failed: return "failed";
    case IceState::Closed: return "closed";
    }
    return "unknown";
}

static std::string randomString(int len) {
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string result;
    result.reserve(len);
    for (int i = 0; i < len; ++i) {
        result += chars[rand() % (sizeof(chars) - 1)];
    }
    return result;
}

IceSession::IceSession(IceMode mode, IceAgent* agent, hv::EventLoopPtr loop)
    : mode_(mode), agent_(agent), loop_(loop) {
    local_ufrag_ = randomString(8);
    local_pwd_ = randomString(24);
    tiebreaker_ = ((uint64_t)rand() << 32) | rand();
}

IceSession::~IceSession() {
    close();
}

void IceSession::close() {
    if (state_ == IceState::Closed) return;
    setState(IceState::Closed);

    // Kill timers (only if the loop is still valid)
    hloop_t* loop = loop_ ? loop_->loop() : nullptr;
    if (loop) {
        if (check_timer_) {
            htimer_del(check_timer_);
        }
        if (keepalive_timer_) {
            htimer_del(keepalive_timer_);
        }
    }
    check_timer_ = nullptr;
    keepalive_timer_ = nullptr;

    // Unregister from agent
    if (agent_) {
        agent_->unregisterSession(local_ufrag_);
    }
}

void IceSession::setState(IceState state) {
    if (state_ == state) return;
    state_ = state;
    if (onStateChange) {
        onStateChange(state);
    }
}

void IceSession::setRemoteCredentials(const std::string& ufrag, const std::string& pwd) {
    remote_ufrag_ = ufrag;
    remote_pwd_ = pwd;
}

void IceSession::addLocalCandidate(const IceCandidate& candidate) {
    local_candidates_.push_back(candidate);
    if (onLocalCandidate) {
        onLocalCandidate(candidate);
    }
}

void IceSession::addRemoteCandidate(const IceCandidate& candidate) {
    remote_candidates_.push_back(candidate);

    // If already checking, form new pairs with this candidate
    if (state_ == IceState::Checking || state_ == IceState::Connected) {
        for (const auto& local : local_candidates_) {
            // Only pair same protocol and same component
            if (local.protocol != candidate.protocol) continue;
            if (local.componentId != candidate.componentId) continue;
            if (local.addr.sa.sa_family != candidate.addr.sa.sa_family) continue;

            CandidatePairPtr pair = std::make_shared<CandidatePair>();
            pair->local = local;
            pair->remote = candidate;
            pair->computePriority(role_);
            pair->state = PairState::Waiting; // New pairs start as Waiting (trickle)
            checklist_.addPair(pair);
        }
        checklist_.sort();
    }
}

void IceSession::setRemoteCandidatesDone() {
    remote_candidates_done_ = true;
}

void IceSession::gatherCandidates() {
    if (state_ != IceState::New) return;
    if (!agent_) return;
    setState(IceState::Gathering);

    // Register with agent
    agent_->registerSession(local_ufrag_, this);

    // gather host candidates
    agent_->addHostCandidates(this);

    auto config = agent_->config();
    sockaddr_u addr;
    // Send STUN binding requests to configured servers
    for (const auto& server : config.stunServers) {
        if(server.toSockaddr(&addr)) {
            sendStunBindingRequest(&addr.sa, server.toString());
        }
    }

    if (config.turnServers.size() > 0) {
        // Delegate TURN allocation to IceAgent
        if (!agent_->isTurnAllocated() && !agent_->isTurnAllocating()) {
            agent_->allocateTurn();
        }
        if (agent_->isTurnAllocated()) {
            // Already allocated, add candidates immediately
            agent_->addTurnCandidates(this);
        } else if (agent_->isTurnAllocating()) {
            // Allocation in progress, wait for onTurnStateChanged notification
            pending_gathering_requests_++;
        }
    }
    // If no STUN servers configured, gathering is complete
    if (pending_gathering_requests_ == 0) {
        onGatheringComplete();
    }
}

void IceSession::onTurnStateChanged(TurnState state) {
    if (state_ != IceState::Gathering) return;

    if (state == TurnState::Allocated) {
        if (agent_) {
            agent_->addTurnCandidates(this);
        }
        pending_gathering_requests_--;
        if (pending_gathering_requests_ == 0) {
            onGatheringComplete();
        }
    } else if (state == TurnState::Failed) {
        pending_gathering_requests_--;
        if (pending_gathering_requests_ == 0) {
            onGatheringComplete();
        }
    }
}

void IceSession::sendStunBindingRequest(const struct sockaddr* server, const std::string& serverStr) {
    StunMessage msg(STUN_METHOD_BINDING, STUN_CLASS_REQUEST);
    std::weak_ptr<IceSession> weak_self = shared_from_this();
    agent_->StunRequest(msg, server, agent_->udpIo(), [weak_self, serverStr](StunMessage* resp, int code) {
        if (auto self = weak_self.lock()) {
            if (resp) {
                self->onGatheringResponse(*resp, serverStr);
            }
            self->pending_gathering_requests_--;
            if (self->pending_gathering_requests_ == 0) {
                self->onGatheringComplete();
            }
        }
    });
    pending_gathering_requests_++;
}

void IceSession::onGatheringResponse(const StunMessage& msg, const std::string& serverAddr) {
    // Extract XOR-MAPPED-ADDRESS for server-reflexive candidate
    struct sockaddr_storage mapped;
    if (!msg.getXorMappedAddress(&mapped)) {
        if (!msg.getMappedAddress(&mapped)) {
            return;
        }
    }

    // Create srflx candidate
    IceCandidate cand;
    cand.type = CandidateType::ServerReflexive;
    cand.protocol = TransportProtocol::UDP;
    cand.componentId = 1;
    memcpy(&cand.addr, &mapped, SOCKADDR_LEN((struct sockaddr*)&mapped));

    // Base is the host candidate address (local addr of agent)
    if (agent_) {
        memcpy(&cand.baseAddr, agent_->udpLocalAddr(), SOCKADDR_LEN(agent_->udpLocalAddr()));
    }
    memcpy(&cand.relatedAddr, &cand.baseAddr, sizeof(sockaddr_u));
    cand.update(serverAddr);
    addLocalCandidate(cand);
}

void IceSession::onGatheringComplete() {
    if (state_ == IceState::Gathering) {
        // If remote credentials already set, start checking
        if (!remote_ufrag_.empty()) {
            formPairs();
            startChecks();
        } else {
            setState(IceState::Checking); // Will start when remote arrives
        }
    }
}

void IceSession::formPairs() {
    checklist_ = IceCheckList(); // Reset
    for (const auto& local : local_candidates_) {
        for (const auto& remote : remote_candidates_) {
            // Only pair same protocol, component, and address family
            if (local.protocol != remote.protocol) continue;
            if (local.componentId != remote.componentId) continue;
            if (local.addr.sa.sa_family != remote.addr.sa.sa_family) continue;

            CandidatePairPtr pair = std::make_shared<CandidatePair>();
            pair->local = local;
            pair->remote = remote;
            pair->computePriority(role_);
            checklist_.addPair(pair);
        }
    }
    checklist_.sort();
    checklist_.unfreezeAll();
}

void IceSession::startChecks() {
    if (state_ == IceState::Closed || state_ == IceState::Failed) return;

    if (mode_ == IceMode::Lite) {
        // ICE-Lite doesn't initiate checks
        setState(IceState::Checking);
        return;
    }

    setState(IceState::Checking);

    // Start periodic check timer
    if (!check_timer_) {
        check_timer_ = htimer_add(loop_->loop(), [](htimer_t* timer) {
            IceSession* self = (IceSession*)hevent_userdata(timer);
            if (self) self->onCheckTimer();
        }, check_interval_ms_, INFINITE);
        hevent_set_userdata(check_timer_, this);
    }
}

void IceSession::onCheckTimer() {
    CandidatePairPtr pair = checklist_.getNextPair();
    if (pair) {
        sendConnectivityCheck(pair);
    } else if (checklist_.isComplete()) {
        // All checks done
        if (check_timer_) {
            htimer_del(check_timer_);
            check_timer_ = nullptr;
        }
        checkNominationComplete();
    }
}

void IceSession::sendConnectivityCheck(CandidatePairPtr pair) {
    StunMessage msg(STUN_METHOD_BINDING, STUN_CLASS_REQUEST);

    // USERNAME = remote_ufrag:local_ufrag
    std::string username = remote_ufrag_ + ":" + local_ufrag_;
    msg.addUsername(username);

    // PRIORITY
    uint32_t priority = computeCandidatePriority(
        CandidateType::PeerReflexive,
        computeLocalPreference(pair->local.addr, pair->local.protocol),
        pair->local.componentId);
    msg.addPriority(priority);

    // ICE-CONTROLLING or ICE-CONTROLLED
    if (role_ == IceRole::Controlling) {
        msg.addIceControlling(tiebreaker_);
        // Nomination
        if (nomination_ == NominationMode::Aggressive || pair->nominated) {
            msg.addUseCandidate();
        }
    } else {
        msg.addIceControlled(tiebreaker_);
    }

    // Encode with MESSAGE-INTEGRITY (using remote password)
    msg.setAuth(remote_pwd_);

    hio_t* io = nullptr;
    // Send via appropriate transport
    if (pair->local.type == CandidateType::Relay) {
        // Send via TURN relay
    } else if (pair->local.protocol == TransportProtocol::UDP) {
        io = agent_->udpIo();
    } else if (pair->local.protocol == TransportProtocol::TCP) {
        if (pair->io) {
            io = pair->io;
        } else if (pair->remote.tcpType == TcpType::Passive) {
            // Need to initiate TCP connection
            if (agent_) {
                agent_->connectTcp(&pair->remote.addr.sa, this);
            }
            return; // Will retry after connection established
        } else {
            // active-active or unsupported TCP type: cannot establish connection
            pair->state = PairState::Failed;
            return;
        }
    }

    // Track transaction
    pair->transactionId = msg.transactionId();
    pair->lastSendTime = hloop_now_ms(loop_->loop());
    pair->state = PairState::InProgress;

    std::weak_ptr<IceSession> weak_self = shared_from_this();
    agent_->StunRequest(msg, &pair->remote.addr.sa, io, [weak_self, this, pair](StunMessage* resp, int code) {
        auto self = weak_self.lock();
        if (!self) return;
        if (resp) {
            if(resp->cls() == STUN_CLASS_SUCCESS_RESPONSE) {
                onCheckSuccess(pair, *resp);
            } else if (resp->cls() == STUN_CLASS_ERROR_RESPONSE) {
                uint16_t errorCode = 0;
                std::string reason;
                resp->getErrorCode(&errorCode, &reason);

                if (errorCode == STUN_ERROR_ROLE_CONFLICT) {
                    // Switch role and retry
                    handleRoleConflict(role_ == IceRole::Controlled);
                    if (pair) {
                        pair->state = PairState::Waiting;
                        checklist_.addTriggeredCheck(pair);
                    }
                    return;
                }
                onCheckFailure(pair, code);
            }
        } else {
            onCheckFailure(pair, code);
        }
    });
}

void IceSession::onRecvData(const uint8_t* data, size_t len, const struct sockaddr* from) {
    if (onData) {
        onData(data, len);
    }
}

void IceSession::onStunRequest(StunMessage& msg, const struct sockaddr* from, hio_t* io) {
    if (msg.method() != STUN_METHOD_BINDING) return;

    // Verify MESSAGE-INTEGRITY with local password
    if (!msg.verifyIntegrity(local_pwd_)) {
        sendStunErrorResponse(msg, STUN_ERROR_UNAUTHORIZED, "Unauthorized", from, io);
        return;
    }

    // Check for role conflict
    if (role_ == IceRole::Controlling && msg.getAttribute(STUN_ATTR_ICE_CONTROLLING)) {
        // Both think they're controlling
        uint64_t remoteTie = msg.getIceControlling();
        if (tiebreaker_ >= remoteTie) {
            sendStunErrorResponse(msg, STUN_ERROR_ROLE_CONFLICT, "Role Conflict", from, io);
            return;
        } else {
            handleRoleConflict(false); // Switch to controlled
        }
    } else if (role_ == IceRole::Controlled && msg.getAttribute(STUN_ATTR_ICE_CONTROLLED)) {
        uint64_t remoteTie = msg.getIceControlled();
        if (tiebreaker_ >= remoteTie) {
            handleRoleConflict(true); // Switch to controlling
        } else {
            sendStunErrorResponse(msg, STUN_ERROR_ROLE_CONFLICT, "Role Conflict", from, io);
            return;
        }
    }

    // Send success response
    sendStunResponse(msg, from, io);

    // Triggered check (RFC 8445 Section 7.3.1.4)
    // Find or create pair for this check
    bool useCandidate = msg.hasUseCandidate();

    // Look for matching pair
    sockaddr_u fromAddr;
    memcpy(&fromAddr, from, SOCKADDR_LEN(from));

    CandidatePairPtr matchedPair = nullptr;
    for (auto& pair : checklist_.pairs()) {
        sockaddr_u remoteAddr = pair->remote.addr;
        if (sockaddr_compare(&remoteAddr, &fromAddr) == 0) {
            matchedPair = pair;
            break;
        }
    }

    if (matchedPair) {
        if (matchedPair->state == PairState::Succeeded) {
            // Already succeeded
            if (useCandidate && role_ == IceRole::Controlled) {
                matchedPair->nominated = true;
                setSelectPair(matchedPair);
            }
        } else if (matchedPair->state != PairState::InProgress) {
            // Trigger check
            checklist_.addTriggeredCheck(matchedPair);
        }
    } else {
        // Peer-reflexive candidate discovery (RFC 8445 Section 7.3.1.3)
        IceCandidate prflxCandidate;
        prflxCandidate.type = CandidateType::PeerReflexive;
        prflxCandidate.protocol = TransportProtocol::UDP;
        prflxCandidate.componentId = 1;
        memcpy(&prflxCandidate.addr, from, SOCKADDR_LEN(from));
        prflxCandidate.priority = msg.getPriority();
        prflxCandidate.foundation = generateFoundation(prflxCandidate.type, prflxCandidate.addr, prflxCandidate.protocol);
        remote_candidates_.push_back(prflxCandidate);

        // Create new pair
        if (!local_candidates_.empty()) {
            CandidatePairPtr newPair = std::make_shared<CandidatePair>();
            newPair->local = local_candidates_[0]; // Use first local candidate
            newPair->remote = prflxCandidate;
            newPair->computePriority(role_);
            newPair->state = PairState::Waiting;
            checklist_.addPair(newPair);
            checklist_.addTriggeredCheck(newPair);
        }
    }

    // ICE-Lite: if we receive USE-CANDIDATE, select the pair
    if (mode_ == IceMode::Lite && useCandidate && matchedPair) {
        matchedPair->nominated = true;
        matchedPair->state = PairState::Succeeded;
        matchedPair->valid = true;
        setSelectPair(matchedPair);
    }
}

void IceSession::onCheckSuccess(CandidatePairPtr pair, const StunMessage& response) {
    pair->state = PairState::Succeeded;
    pair->valid = true;

    // Register the valid pair for data routing
    if (agent_) {
        agent_->registerPair(pair->remote.addr, this);
    }

    // First successful check
    if (state_ == IceState::Checking) {
        setState(IceState::Connected);
    }

    // Handle nomination
    if (role_ == IceRole::Controlling) {
        if (nomination_ == NominationMode::Aggressive) {
            // Already sent USE-CANDIDATE
            pair->nominated = true;
            setSelectPair(pair);
        } else if (nomination_ == NominationMode::Regular && !selected_pair_) {
            // Nominate the first successful pair
            nominate(pair);
        }
    } else {
        // Controlled: wait for USE-CANDIDATE from request
        if (pair->nominated) {
            setSelectPair(pair);
        }
    }
}

void IceSession::setSelectPair(CandidatePairPtr pair) {
    selected_pair_ = pair;
    if (onSelectedPair) onSelectedPair(*selected_pair_);
    setState(IceState::Completed);
    startKeepalive();
}

void IceSession::onCheckFailure(CandidatePairPtr pair, uint16_t errorCode) {
    pair->state = PairState::Failed;

    if (checklist_.allFailed()) {
        setState(IceState::Failed);
    }
}

void IceSession::nominate(CandidatePairPtr pair) {
    pair->nominated = true;
    // Send another connectivity check with USE-CANDIDATE
    pair->state = PairState::InProgress;
    sendConnectivityCheck(pair);
}

void IceSession::checkNominationComplete() {
    if (selected_pair_) {
        setState(IceState::Completed);
    } else if (checklist_.allFailed()) {
        setState(IceState::Failed);
    }
}

void IceSession::handleRoleConflict(bool switchToControlling) {
    if (switchToControlling) {
        role_ = IceRole::Controlling;
    } else {
        role_ = IceRole::Controlled;
    }
    // Recompute pair priorities
    for (auto& pair : checklist_.pairs()) {
        pair->computePriority(role_);
    }
    checklist_.sort();
}

void IceSession::sendStunResponse(const StunMessage& request, const struct sockaddr* to, hio_t* io) {
    StunMessage response(STUN_METHOD_BINDING, STUN_CLASS_SUCCESS_RESPONSE);
    response.setTransactionId(request.transactionId());

    // XOR-MAPPED-ADDRESS
    response.addXorMappedAddress(to);

    // Encode with MESSAGE-INTEGRITY using local password
    auto buf = response.encodeWithAuth(local_pwd_);

    agent_->send(buf.data(), buf.size(), to, io);
}

void IceSession::sendStunErrorResponse(const StunMessage& request, uint16_t code,
                                        const std::string& reason, const struct sockaddr* to, hio_t* io) {
    StunMessage response(STUN_METHOD_BINDING, STUN_CLASS_ERROR_RESPONSE);
    response.setTransactionId(request.transactionId());
    response.addErrorCode(code, reason);

    auto buf = response.encodeWithAuth(local_pwd_);

    agent_->send(buf.data(), buf.size(), to, io);
}

int IceSession::send(const void* data, size_t len) {
    if (!selected_pair_ || !agent_) return -1;
    hio_t* io = nullptr;
    if (selected_pair_->local.type == CandidateType::Relay) {
    }
    else if(selected_pair_->local.protocol == TransportProtocol::UDP){
        io = agent_->udpIo();
    } else {
        io = selected_pair_->io;
        if (!io) {
            return -1;
        }
    }
    return agent_->send(data, len, &selected_pair_->remote.addr.sa, io);
}

void IceSession::onTcpConnected(hio_t* io) {
    // Associate TCP connection with the matching pair
    sockaddr_u* peeraddr = (sockaddr_u*)hio_peeraddr(io);
    for (auto& pair : checklist_.pairs()) {
        if (pair->local.protocol == TransportProtocol::TCP &&
            sockaddr_compare(&pair->remote.addr, peeraddr) == 0) {
            pair->io = io;
            // Now send the connectivity check
            sendConnectivityCheck(pair);
            break;
        }
    }
}

void IceSession::onTcpDisconnected(hio_t* io) {
    // Mark associated pairs as failed
    for (auto& pair : checklist_.pairs()) {
        if (pair->io == io) {
            pair->io = nullptr;
            if (pair->state == PairState::InProgress) {
                pair->state = PairState::Failed;
            }
        }
    }
}

void IceSession::startKeepalive() {
    if (keepalive_timer_) return;
    keepalive_timer_ = htimer_add(loop_->loop(), [](htimer_t* timer) {
        IceSession* self = (IceSession*)hevent_userdata(timer);
        if (self) self->sendKeepalive();
    }, 15000, INFINITE); // Every 15 seconds
    hevent_set_userdata(keepalive_timer_, this);
}

void IceSession::sendKeepalive() {
    if (!selected_pair_) return;

    // Send STUN binding indication (no transaction, no response expected)
    StunMessage msg(STUN_METHOD_BINDING, STUN_CLASS_INDICATION);
    auto buf = msg.encode();
    send(buf.data(), buf.size());
}

} // namespace ice
