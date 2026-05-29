#include "ice_session.h"
#include "../agent/ice_agent.h"
#include "../stun/stun_auth.h"
#include "../turn/turn_client.h"
#include "hloop.h"
#include "hlog.h"

#include <cstdlib>
#include <ctime>
#include <cinttypes>
#include <memory>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif

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

static bool canPairCandidates(const IceCandidate& local, const IceCandidate& remote) {
    return local.protocol == remote.protocol &&
           local.componentId == remote.componentId &&
           local.addr.sa.sa_family == remote.addr.sa.sa_family;
}

static const IceCandidate* findBestLocalForPrflx(
    const std::vector<IceCandidate>& localCandidates,
    hio_t* io, TransportProtocol protocol) {
    if (io) {
        const sockaddr* localAddr = hio_localaddr(io);
        for (const auto& local : localCandidates) {
            if (local.protocol != protocol) continue;
            if (sockaddr_compare(&local.baseAddr, (const sockaddr_u*)localAddr) == 0) {
                return &local;
            }
        }
    }

    for (const auto& local : localCandidates) {
        if (local.protocol == protocol) {
            return &local;
        }
    }
    return nullptr;
}

IceSession::IceSession(IceMode mode, IceAgent* agent, hv::EventLoopPtr loop)
    : mode_(mode), agent_(agent), loop_(loop) {
    local_ufrag_ = randomString(8);
    local_pwd_ = randomString(24);
    tiebreaker_ = ((uint64_t)rand() << 32) | rand();
    hlogi("IceSession %s created, local_pwd=%s, tiebreaker=%llu", id(), local_pwd_.c_str(), tiebreaker_);
}

IceSession::~IceSession() {
    hlogi("IceSession %s destroyed", id());
    close();
}
hio_t* IceSession::udpIo() const { 
    return udp_io_ ? udp_io_ : agent_->udpIo(); 
}

void IceSession::close() {
    if (state_ == IceState::Closed) return;
    hlogi("IceSession %s closed", id());

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
        if (gathering_timer_) {
            htimer_del(gathering_timer_);
        }
        if (connectivity_timer_) {
            htimer_del(connectivity_timer_);
        }
    }
    if (udp_io_) {
        hio_close(udp_io_);
        udp_io_ = nullptr;
    }
    auto ios = ios_;
    ios_.clear();
    for (auto io : ios) {
        if (io) {
            // Avoid dangling session pointer in IceAgent::onTcpClose callback.
            hio_set_context(io, nullptr);
            hio_close(io);
        }
    }

    check_timer_ = nullptr;
    keepalive_timer_ = nullptr;
    gathering_timer_ = nullptr;
    connectivity_timer_ = nullptr;

    // Unregister from agent
    if (agent_) {
        agent_->unregisterSession(local_ufrag_);
    }
}

void IceSession::setState(IceState state) {
    if (state_ == state) return;
    hlogi("IceSession %s state changed from %s to %s", id(), iceStateString(state_), iceStateString(state));
    state_ = state;
    if (onStateChange) {
        onStateChange(state);
    }
}

void IceSession::setRemoteCredentials(const std::string& ufrag, const std::string& pwd) {
    hlogi("IceSession %s setRemoteCredentials %s/%s", id(), ufrag.c_str(), pwd.c_str());
    remote_ufrag_ = ufrag;
    remote_pwd_ = pwd;
}

void IceSession::addLocalCandidate(const IceCandidate& candidate) {
    hlogi("IceSession %s addLocalCandidate %s", id(), candidate.toSdp().c_str());
    local_candidates_.push_back(candidate);
    if (onLocalCandidate) {
        onLocalCandidate(candidate);
    }
}

void IceSession::addRemoteCandidate(const IceCandidate& candidate) {
    hlogi("IceSession %s addRemoteCandidate %s", id(), candidate.toSdp().c_str());
    remote_candidates_.push_back(candidate);

    // If already checking, form new pairs with this candidate
    if (state_ == IceState::Checking || state_ == IceState::Connected) {
        int newPairs = 0;
        if (candidate.protocol == TransportProtocol::TCP) {
            // For ICE-TCP, remote active/SO are connection-scoped in our stack.
            // Do not pre-form checklist pairs here; pair will be created/reused on inbound STUN/prflx path.
            if (candidate.tcpType != TcpType::Passive) {
                hlogi("IceSession %s addRemoteCandidate tcp-active trickle: skip pre-pairing", id());
                return;
            }

            const IceCandidate* localForConnect = nullptr;
            for (const auto& local : local_candidates_) {
                if (canPairCandidates(local, candidate)) {
                    localForConnect = &local;
                    break;
                }
            }
            if (!localForConnect) {
                hlogi("IceSession %s addRemoteCandidate tcp-passive trickle: no compatible local candidate", id());
                return;
            }

            CandidatePairPtr pair = std::make_shared<CandidatePair>();
            pair->local = *localForConnect;
            pair->local.protocol = TransportProtocol::TCP; // Override local protocol to TCP for correct role assignment
            pair->local.tcpType = TcpType::Active; // Override local TCP type to passive for correct role assignment
            pair->local.componentId = candidate.componentId; // Override local component ID to match remote
            pair->remote = candidate;
            pair->computePriority(role_);
            pair->state = PairState::Waiting; // New pairs start as Waiting (trickle)
            checklist_.addPair(pair);
            pair->io = agent_->connectTcp(&candidate.addr.sa, this);
            hlogi("IceSession %s addRemoteCandidate new tcp-passive pair %s io=%p",
                  id(), pair->toString().c_str(), (void*)pair->io);
            ++newPairs;
        } else {
            for (const auto& local : local_candidates_) {
                if (!canPairCandidates(local, candidate)) continue;

                CandidatePairPtr pair = std::make_shared<CandidatePair>();
                pair->local = local;
                pair->remote = candidate;
                pair->computePriority(role_);
                pair->state = PairState::Waiting; // New pairs start as Waiting (trickle)
                checklist_.addPair(pair);
                hlogi("IceSession %s addRemoteCandidate new pair %s", id(), pair->toString().c_str());
                ++newPairs;
            }
        }
        checklist_.sort();
        if (newPairs > 0) {
            hlogi("IceSession %s addRemoteCandidate formed %d new pairs (trickle)", id(), newPairs);
        }
    }
}

void IceSession::setRemoteCandidatesDone() {
    hlogi("IceSession %s setRemoteCandidatesDone", id());
    remote_candidates_done_ = true;
}


// addLoacalIceCandidate helper api
void IceSession::addHostCandidates(int componentId) {
    uint16_t udp_port = udp_io_?sockaddr_port((sockaddr_u*)hio_localaddr(udp_io_)):agent_->udpPort();
    uint16_t tcp_port = agent_->tcpPort();
#ifdef _WIN32
    // Windows: Use GetAdaptersAddresses
    ULONG bufLen = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &bufLen);
    std::vector<uint8_t> buf(bufLen);
    PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)buf.data();
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, addrs, &bufLen) == NO_ERROR) {
        for (auto adapter = addrs; adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) continue;
            if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

            for (auto ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
                struct sockaddr* sa = ua->Address.lpSockaddr;
                if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)
                    continue;

                // Skip link-local IPv6
                if (sa->sa_family == AF_INET6) {
                    struct sockaddr_in6* sin6 = (struct sockaddr_in6*)sa;
                    if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr))
                        continue;
                }
                if (udp_port) {
                    IceCandidate cand;
                    cand.type = CandidateType::Host;
                    cand.protocol = TransportProtocol::UDP;
                    cand.componentId = componentId;
                    memcpy(&cand.addr, sa, SOCKADDR_LEN(sa));
                    // Set port from agent
                    sockaddr_set_port(&cand.addr, udp_port);
                    memcpy(&cand.baseAddr, &cand.addr, sizeof(sockaddr_u));
                    cand.update();
                    
                    addLocalCandidate(cand);
                }

                // TCP passive candidate (if TCP enabled)
                if (tcp_port) {
                    IceCandidate tcpCand;
                    tcpCand.type = CandidateType::Host;
                    tcpCand.protocol = TransportProtocol::TCP;
                    tcpCand.tcpType = TcpType::Passive;
                    tcpCand.componentId = componentId;
                    memcpy(&tcpCand.addr, sa, SOCKADDR_LEN(sa));
                    sockaddr_set_port(&tcpCand.addr, tcp_port);
                    memcpy(&tcpCand.baseAddr, &tcpCand.addr, sizeof(sockaddr_u));
                    tcpCand.update();

                    addLocalCandidate(tcpCand);
                }
            }
        }
    }
#else
    // Unix/Linux/macOS: Use getifaddrs
    struct ifaddrs* ifaddr;
    if (getifaddrs(&ifaddr) == -1) return;

    for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        struct sockaddr* sa = ifa->ifa_addr;
        if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6) continue;

        // Skip link-local IPv6
        if (sa->sa_family == AF_INET6) {
            struct sockaddr_in6* sin6 = (struct sockaddr_in6*)sa;
            if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) continue;
        }
        if (udp_port) {
            IceCandidate cand;
            cand.type = CandidateType::Host;
            cand.protocol = TransportProtocol::UDP;
            cand.componentId = componentId;
            memcpy(&cand.addr, sa, SOCKADDR_LEN(sa));
            sockaddr_set_port(&cand.addr, udp_port);
            memcpy(&cand.baseAddr, &cand.addr, sizeof(sockaddr_u));
            cand.update();
            addLocalCandidate(cand);
        }

        if (tcp_port) {
            IceCandidate tcpCand;
            tcpCand.type = CandidateType::Host;
            tcpCand.protocol = TransportProtocol::TCP;
            tcpCand.tcpType = TcpType::Passive;
            tcpCand.componentId = componentId;
            memcpy(&tcpCand.addr, sa, SOCKADDR_LEN(sa));
            sockaddr_set_port(&tcpCand.addr, tcp_port);
            memcpy(&tcpCand.baseAddr, &tcpCand.addr, sizeof(sockaddr_u));
            tcpCand.update();
            addLocalCandidate(tcpCand);
        }
    }

    freeifaddrs(ifaddr);
#endif
}

void IceSession::gatherCandidates(bool bandUdp) {
    hlogi("IceSession %s gatherCandidates", id());
    if (state_ != IceState::New) return;
    if (!agent_) return;
    if (bandUdp && !udp_io_) {
        udp_io_ = agent_->bindUdp(this);
    }
    setState(IceState::Gathering);

    // Register with agent
    agent_->registerSession(local_ufrag_, this);

    // gather host candidates
    addHostCandidates(1);

    auto config = agent_->config();
    // Send STUN binding requests to configured servers
    if (config.gatherSrflx && config.stunServers.size() > 0) {
        for (const auto& server : config.stunServers) {
            sockaddr_u addr;
            if (0 == sockaddr_set_ipport(&addr, server.host.c_str(), server.port)) {
                sendStunBindingRequest(&addr.sa, server.toString());
            }
        }        
    }

    if (config.gatherRelay && config.turnServers.size() > 0) {
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
    } else {
        // Start gathering timeout timer (RFC 8445: gathering should not hang forever)
        int timeoutMs = config.gatheringTimeoutMs > 0 ? config.gatheringTimeoutMs : 10000;
        gathering_timer_ = htimer_add(loop_->loop(), [](htimer_t* timer) {
            IceSession* self = (IceSession*)hevent_userdata(timer);
            if (self) {
                self->gathering_timer_ = nullptr;
                self->onGatheringComplete();
            }
        }, timeoutMs, 0);
        hevent_set_userdata(gathering_timer_, this);
    }
}

void IceSession::onTurnStateChanged(TurnState state) {
    hlogi("IceSession %s onTurnStateChanged %s", id(), turnStateString(state));
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
    hlogi("IceSession %s sendStunBindingRequest to %s", id(), serverStr.c_str());
    agent_->StunRequest(msg, server, udpIo(), [weak_self, serverStr](StunMessage* resp, int code) {
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
    hlogi("IceSession %s onGatheringResponse from %s", id(), serverAddr.c_str());

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
        // Cancel gathering timeout timer
        if (gathering_timer_) {
            htimer_del(gathering_timer_);
            gathering_timer_ = nullptr;
        }

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
    int pairCount = 0;
    for (const auto& local : local_candidates_) {
        for (const auto& remote : remote_candidates_) {
            if (!canPairCandidates(local, remote)) continue;

            CandidatePairPtr pair = std::make_shared<CandidatePair>();
            pair->local = local;
            pair->remote = remote;
            pair->computePriority(role_);
            checklist_.addPair(pair);
            hlogi("IceSession %s formPairs [%d] %s priority=%" PRIu64,
                  id(), pairCount, pair->toString().c_str(), pair->priority);
            ++pairCount;
        }
    }
    checklist_.sort();
    checklist_.unfreezeAll();
    hlogi("IceSession %s formPairs total=%d local_cands=%zu remote_cands=%zu",
          id(), pairCount,
          local_candidates_.size(), remote_candidates_.size());
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

    // Start connectivity timeout timer
    if (!connectivity_timer_ && agent_) {
        int timeoutMs = agent_->config().connectivityTimeoutMs;
        if (timeoutMs <= 0) timeoutMs = 30000;
        connectivity_timer_ = htimer_add(loop_->loop(), [](htimer_t* timer) {
            IceSession* self = (IceSession*)hevent_userdata(timer);
            if (!self) return;
            self->connectivity_timer_ = nullptr;
            // Timeout: if not completed/connected, mark as failed
            if (self->state_ == IceState::Checking || self->state_ == IceState::Connected) {
                if (!self->selected_pair_) {
                    hlogi("IceSession %s onCheckTimer timeout", self->id());
                    self->setState(IceState::Failed);
                }
            }
        }, timeoutMs, 0);
        hevent_set_userdata(connectivity_timer_, this);
    }
}

void IceSession::onCheckTimer() {
    CandidatePairPtr pair = checklist_.getNextPair();
    if (pair) {
        sendConnectivityCheck(pair);
    } else if (checklist_.isComplete()) {
        // All pairs reached terminal state (Succeeded/Failed), stop timer
        if (check_timer_) {
            htimer_del(check_timer_);
            check_timer_ = nullptr;
        }
        checkNominationComplete();
    }
    // else: some pairs still InProgress, waiting for response/timeout
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
    bool useCandidate = false;
    if (role_ == IceRole::Controlling) {
        msg.addIceControlling(tiebreaker_);
        // Nomination
        if (nomination_ == NominationMode::Aggressive || pair->nominated) {
            msg.addUseCandidate();
            useCandidate = true;
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
        io = udpIo();
    } else if (pair->local.protocol == TransportProtocol::TCP) {
        if (pair->io) {
            io = pair->io;
            if (!hio_is_opened(io))
                return;
        } else if (pair->remote.tcpType == TcpType::Passive) {
            // Need to initiate TCP connection
            pair->state = PairState::Waiting;
            pair->io = agent_->connectTcp(&pair->remote.addr.sa, this);
            return; // Will retry after connection established
        } else {
            // active-active or unsupported TCP type: cannot establish connection
            pair->state = PairState::Failed;
            hlogi("IceSession %s sendConnectivityCheck %s: unsupported TCP type, mark failed",
                  id(), pair->toString().c_str());
            return;
        }
    }

    // Track transaction
    pair->transactionId = msg.transactionId();
    pair->lastSendTime = hloop_now_ms(loop_->loop());
    // Only change state to InProgress if not already Succeeded (e.g. nomination re-check)
    if (pair->state != PairState::Succeeded) {
        pair->state = PairState::InProgress;
    }

    hlogi("IceSession %s sendConnectivityCheck %s role=%s useCandidate=%d nominated=%d",
          id(), pair->toString().c_str(),
          iceRoleString(role_), (int)useCandidate, (int)pair->nominated);

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
    if (msg.cls() != STUN_CLASS_REQUEST) return;

    char fromStr[SOCKADDR_STRLEN] = {0};
    SOCKADDR_STR(from, fromStr);
    hlogi("IceSession %s onStunRequest from=%s useCandidate=%d",
          id(), fromStr, (int)msg.hasUseCandidate());

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
        if (useCandidate && role_ == IceRole::Controlled) {
            // Keep nomination intent even when the check is already InProgress.
            matchedPair->nominated = true;
        }
        if (matchedPair->state == PairState::Succeeded) {
            // Already succeeded
            if (matchedPair->nominated && role_ == IceRole::Controlled) {
                hlogi("IceSession %s onStunRequest USE-CANDIDATE, select pair %s",
                      id(), matchedPair->toString().c_str());
                setSelectPair(matchedPair);
            }
        } else if (matchedPair->state != PairState::InProgress) {
            // Trigger check
            hlogi("IceSession %s onStunRequest triggered check for pair %s state=%s",
                  id(), matchedPair->toString().c_str(),
                  pairStateString(matchedPair->state));
            checklist_.addTriggeredCheck(matchedPair);
        }
    } else {
        // Peer-reflexive candidate discovery (RFC 8445 Section 7.3.1.3)
        IceCandidate prflxCandidate;
        prflxCandidate.type = CandidateType::PeerReflexive;
        // Determine protocol from the transport the request arrived on
        bool tcp = io && hio_type(io) == HIO_TYPE_TCP;
        if (tcp) {
            prflxCandidate.protocol = TransportProtocol::TCP;
            prflxCandidate.tcpType = TcpType::Active;
        }
        else {
            prflxCandidate.protocol = TransportProtocol::UDP;
        }
        prflxCandidate.componentId = 1;
        memcpy(&prflxCandidate.addr, from, SOCKADDR_LEN(from));
        prflxCandidate.priority = msg.getPriority();
        prflxCandidate.foundation = generateFoundation(prflxCandidate.type, prflxCandidate.addr, prflxCandidate.protocol);
        remote_candidates_.push_back(prflxCandidate);
        hlogi("IceSession %s onStunRequest discovered peer-reflexive candidate %s",
              id(), prflxCandidate.addrString().c_str());

        // Create new pair from the best local candidate for this inbound request.
        const IceCandidate* bestLocal = findBestLocalForPrflx(local_candidates_, io, prflxCandidate.protocol);

        if (bestLocal) {
            CandidatePairPtr newPair = std::make_shared<CandidatePair>();
            newPair->local = *bestLocal;
            newPair->remote = prflxCandidate;
            if (tcp) {
                newPair->io = io;
            }
            newPair->computePriority(role_);
            newPair->state = PairState::Waiting;
            checklist_.addPair(newPair);
            checklist_.addTriggeredCheck(newPair);
            hlogi("IceSession %s onStunRequest created prflx pair %s, triggered check",
                  id(), newPair->toString().c_str());
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

    hlogi("IceSession %s onCheckSuccess %s nominated=%d",
          id(), pair->toString().c_str(), (int)pair->nominated);

    // Register the valid pair for data routing
    if (agent_ && pair->local.protocol == TransportProtocol::UDP && !udp_io_) {
        agent_->registerPair(pair->remote.addr, this);
    }

    // First successful check
    if (state_ == IceState::Checking) {
        setState(IceState::Connected);
    }

    // If already selected, nothing more to do
    if (selected_pair_) return;

    // Handle nomination
    if (role_ == IceRole::Controlling) {
        if (nomination_ == NominationMode::Aggressive) {
            // Already sent USE-CANDIDATE in the check
            pair->nominated = true;
            setSelectPair(pair);
        } else if (nomination_ == NominationMode::Regular) {
            if (pair->nominated) {
                // This is the nomination check response (with USE-CANDIDATE)
                setSelectPair(pair);
            } else {
                // First success: nominate this pair
                nominate(pair);
            }
        }
    } else {
        // Controlled: wait for USE-CANDIDATE from peer's request
        if (pair->nominated) {
            setSelectPair(pair);
        }
    }
}

void IceSession::setSelectPair(CandidatePairPtr pair) {
    if (selected_pair_ != pair) {
        selected_pair_ = pair;
        hlogi("IceSession %s setSelectPair %s", id(), pair->toString().c_str());
        if (pair->local.type == CandidateType::Relay && agent_->turnClient()) {
            // If selected pair is relay, we may want to tear down direct sockets to save resources
            // (optional optimization, not implemented here)
            agent_->turnClient()->channelBind(&pair->remote.addr.sa, 0);
        }
    }
    if (onSelectedPair) onSelectedPair(*selected_pair_);
    setState(IceState::Completed);
    startKeepalive();

    // Cancel connectivity timeout timer since we have a selected pair
    if (connectivity_timer_) {
        htimer_del(connectivity_timer_);
        connectivity_timer_ = nullptr;
    }
}

void IceSession::onCheckFailure(CandidatePairPtr pair, uint16_t errorCode) {
    pair->state = PairState::Failed;
    hlogi("IceSession %s onCheckFailure %s", id(), pair->toString().c_str());

    if (checklist_.allFailed()) {
        setState(IceState::Failed);
    }
}

void IceSession::nominate(CandidatePairPtr pair) {
    hlogi("IceSession %s nominate %s", id(), pair->toString().c_str());
    pair->nominated = true;
    // Send a new connectivity check with USE-CANDIDATE
    // Keep pair->state as Succeeded so isComplete() is not blocked
    // The nomination check shares the same pair but is a new transaction
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
    hlogi("IceSession %s handleRoleConflict %s", id(), iceRoleString(role_));
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

void IceSession::sendStunErrorResponse(const StunMessage& request, 
  uint16_t code, const std::string& reason, 
  const struct sockaddr* to, hio_t* io) {
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
    else if(selected_pair_->local.protocol == TransportProtocol::UDP) {
        io = udpIo();
    } else {
        io = selected_pair_->io;
        if (!io || !hio_is_opened(io))
            return -1;
    }
    return agent_->send(data, len, &selected_pair_->remote.addr.sa, io);
}

bool IceSession::onTcpAccepted(hio_t* io) {
    if (state_ == IceState::Closed) {
        return false;
    }
    // Associate TCP connection with the matching pair
    sockaddr_u* peeraddr = (sockaddr_u*)hio_peeraddr(io);
    char peerStr[SOCKADDR_STRLEN] = {0};
    SOCKADDR_STR((struct sockaddr*)peeraddr, peerStr);
    hlogi("IceSession %s onTcpAccepted peer=%s", id(), peerStr);
    ios_.insert(io);
    return true;
}

void IceSession::onTcpConnected(hio_t* io) {
    // Associate TCP connection with the matching pair
    sockaddr_u* peeraddr = (sockaddr_u*)hio_peeraddr(io);
    sockaddr_u* localaddr = (sockaddr_u*)hio_localaddr(io);
    char peerStr[SOCKADDR_STRLEN] = {0};
    char localStr[SOCKADDR_STRLEN] = {0};
    SOCKADDR_STR((struct sockaddr*)peeraddr, peerStr);
    SOCKADDR_STR((struct sockaddr*)localaddr, localStr);
    hlogi("IceSession %s onTcpConnected %s->%s", id(), localStr, peerStr);

    CandidatePairPtr pair = checklist_.findByIO(io);
    if (!pair) {
        hlogw("IceSession %s onTcpConnected no matching pair closing io", id());
        hio_close(io);
        return;
    }
    ios_.insert(io);

    auto& localCand = pair->local;
    localCand.type = CandidateType::Host;
    localCand.tcpType = TcpType::Active;
    memcpy(&localCand.addr, localaddr, SOCKADDR_LEN((struct sockaddr*)localaddr));
    memcpy(&localCand.baseAddr, &localCand.addr, sizeof(sockaddr_u));
    localCand.update();
    pair->computePriority(role_);
    hlogi("IceSession %s onTcpConnected upgraded pair %s and sending check", id(), pair->toString().c_str());

    // Now send the connectivity check
    sendConnectivityCheck(pair);
}

void IceSession::onTcpDisconnected(hio_t* io) {
    ios_.erase(io);
    // Mark associated pairs as failed
    CandidatePairPtr pair = checklist_.findByIO(io);
    if (pair) {
        pair->io = nullptr;
        hlogi("IceSession %s onTcpDisconnected pair=%s state=%s",
              id(), pair->toString().c_str(),
              pairStateString(pair->state));
        if (pair->state == PairState::InProgress) {
            pair->state = PairState::Failed;
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
