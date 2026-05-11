#include "ice_session.h"
#include "../transport/udp_transport.h"
#include "../transport/tcp_transport.h"
#include "../stun/stun_auth.h"

#include "hloop.h"

#include <cstdlib>
#include <ctime>
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

static std::string transactionIdToKey(const TransactionId& id) {
    std::ostringstream oss;
    for (auto b : id) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    }
    return oss.str();
}

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

std::string IceSession::generateUfrag() {
    return randomString(8);
}

std::string IceSession::generatePwd() {
    return randomString(24);
}

IceSession::IceSession(IceMode mode, hv::EventLoopPtr loop)
    : mode_(mode), loop_(loop) {
    local_ufrag_ = generateUfrag();
    local_pwd_ = generatePwd();
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
        // Cancel pending transactions
        for (auto& kv : transactions_) {
            if (kv.second.timer) {
                htimer_del(kv.second.timer);
            }
        }
    }
    check_timer_ = nullptr;
    keepalive_timer_ = nullptr;
    transactions_.clear();

    // Unregister from transport
    if (udp_transport_) {
        udp_transport_->unregisterSession(local_ufrag_);
    }
    if (tcp_transport_) {
        tcp_transport_->unregisterSession(local_ufrag_);
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

            CandidatePair pair;
            pair.local = local;
            pair.remote = candidate;
            pair.computePriority(role_);
            pair.state = PairState::Waiting; // New pairs start as Waiting (trickle)
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
    setState(IceState::Gathering);

    // Register with transport
    if (udp_transport_) {
        udp_transport_->registerSession(local_ufrag_, this);
    }
    if (tcp_transport_) {
        tcp_transport_->registerSession(local_ufrag_, this);
    }

    gatherHostCandidates();

    // If no STUN servers configured, gathering is complete
    if (pending_gathering_requests_ == 0) {
        onGatheringComplete();
    }
}

void IceSession::gatherHostCandidates() {
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
                if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6) continue;

                // Skip link-local IPv6
                if (sa->sa_family == AF_INET6) {
                    struct sockaddr_in6* sin6 = (struct sockaddr_in6*)sa;
                    if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) continue;
                }

                IceCandidate cand;
                cand.type = CandidateType::Host;
                cand.protocol = TransportProtocol::UDP;
                cand.componentId = 1;
                memcpy(&cand.addr, sa, SOCKADDR_LEN(sa));
                // Set port from transport
                if (udp_transport_) {
                    if (sa->sa_family == AF_INET) {
                        cand.addr.sin.sin_port = htons(udp_transport_->port());
                    } else {
                        cand.addr.sin6.sin6_port = htons(udp_transport_->port());
                    }
                }
                memcpy(&cand.baseAddr, &cand.addr, sizeof(sockaddr_u));
                cand.priority = computeCandidatePriority(
                    CandidateType::Host,
                    computeLocalPreference(cand.addr, TransportProtocol::UDP),
                    cand.componentId);
                cand.foundation = generateFoundation(CandidateType::Host, cand.baseAddr, TransportProtocol::UDP);
                addLocalCandidate(cand);

                // TCP passive candidate (if TCP transport available)
                if (tcp_transport_) {
                    IceCandidate tcpCand = cand;
                    tcpCand.protocol = TransportProtocol::TCP;
                    tcpCand.tcpType = TcpType::Passive;
                    if (sa->sa_family == AF_INET) {
                        tcpCand.addr.sin.sin_port = htons(tcp_transport_->port());
                    } else {
                        tcpCand.addr.sin6.sin6_port = htons(tcp_transport_->port());
                    }
                    memcpy(&tcpCand.baseAddr, &tcpCand.addr, sizeof(sockaddr_u));
                    tcpCand.priority = computeCandidatePriority(
                        CandidateType::Host,
                        computeLocalPreference(tcpCand.addr, TransportProtocol::TCP),
                        tcpCand.componentId);
                    tcpCand.foundation = generateFoundation(CandidateType::Host, tcpCand.baseAddr, TransportProtocol::TCP);
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

        IceCandidate cand;
        cand.type = CandidateType::Host;
        cand.protocol = TransportProtocol::UDP;
        cand.componentId = 1;
        memcpy(&cand.addr, sa, SOCKADDR_LEN(sa));
        if (udp_transport_) {
            if (sa->sa_family == AF_INET) {
                cand.addr.sin.sin_port = htons(udp_transport_->port());
            } else {
                cand.addr.sin6.sin6_port = htons(udp_transport_->port());
            }
        }
        memcpy(&cand.baseAddr, &cand.addr, sizeof(sockaddr_u));
        cand.priority = computeCandidatePriority(
            CandidateType::Host,
            computeLocalPreference(cand.addr, TransportProtocol::UDP),
            cand.componentId);
        cand.foundation = generateFoundation(CandidateType::Host, cand.baseAddr, TransportProtocol::UDP);
        addLocalCandidate(cand);

        if (tcp_transport_) {
            IceCandidate tcpCand = cand;
            tcpCand.protocol = TransportProtocol::TCP;
            tcpCand.tcpType = TcpType::Passive;
            if (sa->sa_family == AF_INET) {
                tcpCand.addr.sin.sin_port = htons(tcp_transport_->port());
            } else {
                tcpCand.addr.sin6.sin6_port = htons(tcp_transport_->port());
            }
            memcpy(&tcpCand.baseAddr, &tcpCand.addr, sizeof(sockaddr_u));
            tcpCand.priority = computeCandidatePriority(
                CandidateType::Host,
                computeLocalPreference(tcpCand.addr, TransportProtocol::TCP),
                tcpCand.componentId);
            tcpCand.foundation = generateFoundation(CandidateType::Host, tcpCand.baseAddr, TransportProtocol::TCP);
            addLocalCandidate(tcpCand);
        }
    }

    freeifaddrs(ifaddr);
#endif
}

void IceSession::sendStunBindingRequest(const struct sockaddr* server, const std::string& serverStr) {
    StunMessage msg(STUN_METHOD_BINDING, STUN_CLASS_REQUEST);

    auto buf = msg.encode(); // No auth for STUN server binding

    if (udp_transport_) {
        udp_transport_->sendTo(buf.data(), buf.size(), server);
    }

    // Track transaction
    StunTransaction txn;
    txn.id = msg.transactionId();
    memcpy(&txn.destAddr, server, SOCKADDR_LEN(server));
    txn.sentTime = hloop_now_ms(loop_->loop());
    txn.isGathering = true;
    txn.serverAddr = serverStr;
    addTransaction(txn);
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

    // Base is the host candidate address (local addr of transport)
    if (udp_transport_) {
        memcpy(&cand.baseAddr, udp_transport_->localAddr(),
               SOCKADDR_LEN(udp_transport_->localAddr()));
    }
    memcpy(&cand.relatedAddr, &cand.baseAddr, sizeof(sockaddr_u));

    cand.priority = computeCandidatePriority(
        CandidateType::ServerReflexive,
        computeLocalPreference(cand.addr, TransportProtocol::UDP),
        cand.componentId);
    cand.foundation = generateFoundation(CandidateType::ServerReflexive,
                                          cand.baseAddr, TransportProtocol::UDP, serverAddr);
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

            CandidatePair pair;
            pair.local = local;
            pair.remote = remote;
            pair.computePriority(role_);
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
    CandidatePair* pair = checklist_.getNextPair();
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

void IceSession::sendConnectivityCheck(CandidatePair* pair) {
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
    auto buf = msg.encodeWithAuth(remote_pwd_);

    // Send via appropriate transport
    if (pair->local.protocol == TransportProtocol::UDP) {
        if (udp_transport_) {
            udp_transport_->sendTo(buf.data(), buf.size(), &pair->remote.addr.sa);
        }
    } else if (pair->local.protocol == TransportProtocol::TCP) {
        if (pair->tcpIo) {
            tcp_transport_->send(pair->tcpIo, buf.data(), buf.size());
        } else if (pair->remote.tcpType == TcpType::Passive) {
            // Need to initiate TCP connection
            if (tcp_transport_) {
                tcp_transport_->connect(&pair->remote.addr.sa, this);
            }
            return; // Will retry after connection established
        }
    }

    // Track transaction
    pair->transactionId = msg.transactionId();
    pair->lastSendTime = hloop_now_ms(loop_->loop());
    pair->state = PairState::InProgress;

    StunTransaction txn;
    txn.id = msg.transactionId();
    txn.pair = pair;
    memcpy(&txn.destAddr, &pair->remote.addr, sizeof(pair->remote.addr));
    txn.sentTime = pair->lastSendTime;
    addTransaction(txn);
}

void IceSession::onStunPacket(const uint8_t* data, size_t len, const struct sockaddr* from) {
    StunMessage msg;
    if (!StunMessage::decode(data, len, &msg)) return;

    uint16_t cls = msg.cls();
    switch (cls) {
    case STUN_CLASS_REQUEST:
        handleStunRequest(msg, from);
        break;
    case STUN_CLASS_SUCCESS_RESPONSE:
        handleStunResponse(msg, from);
        break;
    case STUN_CLASS_ERROR_RESPONSE:
        handleStunErrorResponse(msg, from);
        break;
    case STUN_CLASS_INDICATION:
        // STUN indication (e.g., binding indication for keepalive) - ignore
        break;
    }
}

void IceSession::handleStunRequest(const StunMessage& msg, const struct sockaddr* from) {
    if (msg.method() != STUN_METHOD_BINDING) return;

    // Verify MESSAGE-INTEGRITY with local password
    if (!msg.verifyIntegrity(local_pwd_)) {
        sendStunErrorResponse(msg, STUN_ERROR_UNAUTHORIZED, "Unauthorized", from);
        return;
    }

    // Check for role conflict
    if (role_ == IceRole::Controlling && msg.getAttribute(STUN_ATTR_ICE_CONTROLLING)) {
        // Both think they're controlling
        uint64_t remoteTie = msg.getIceControlling();
        if (tiebreaker_ >= remoteTie) {
            sendStunErrorResponse(msg, STUN_ERROR_ROLE_CONFLICT, "Role Conflict", from);
            return;
        } else {
            handleRoleConflict(false); // Switch to controlled
        }
    } else if (role_ == IceRole::Controlled && msg.getAttribute(STUN_ATTR_ICE_CONTROLLED)) {
        uint64_t remoteTie = msg.getIceControlled();
        if (tiebreaker_ >= remoteTie) {
            handleRoleConflict(true); // Switch to controlling
        } else {
            sendStunErrorResponse(msg, STUN_ERROR_ROLE_CONFLICT, "Role Conflict", from);
            return;
        }
    }

    // Send success response
    sendStunResponse(msg, from);

    // Triggered check (RFC 8445 Section 7.3.1.4)
    // Find or create pair for this check
    bool useCandidate = msg.hasUseCandidate();

    // Look for matching pair
    sockaddr_u fromAddr;
    memcpy(&fromAddr, from, SOCKADDR_LEN(from));

    CandidatePair* matchedPair = nullptr;
    for (auto& pair : checklist_.pairs()) {
        sockaddr_u remoteAddr = pair.remote.addr;
        if (memcmp(&remoteAddr, &fromAddr, SOCKADDR_LEN(from)) == 0) {
            matchedPair = &pair;
            break;
        }
    }

    if (matchedPair) {
        if (matchedPair->state == PairState::Succeeded) {
            // Already succeeded
            if (useCandidate && role_ == IceRole::Controlled) {
                matchedPair->nominated = true;
                selected_pair_ = matchedPair;
                if (onSelectedPair) onSelectedPair(*selected_pair_);
                setState(IceState::Completed);
                startKeepalive();
            }
        } else if (matchedPair->state != PairState::InProgress) {
            // Trigger check
            checklist_.addTriggeredCheck(*matchedPair);
        }
    } else {
        // Peer-reflexive candidate discovery (RFC 8445 Section 7.3.1.3)
        IceCandidate prflxCandidate;
        prflxCandidate.type = CandidateType::PeerReflexive;
        prflxCandidate.protocol = TransportProtocol::UDP;
        prflxCandidate.componentId = 1;
        memcpy(&prflxCandidate.addr, from, SOCKADDR_LEN(from));
        prflxCandidate.priority = msg.getPriority();
        prflxCandidate.foundation = generateFoundation(CandidateType::PeerReflexive,
                                                        prflxCandidate.addr, TransportProtocol::UDP);
        remote_candidates_.push_back(prflxCandidate);

        // Create new pair
        if (!local_candidates_.empty()) {
            CandidatePair newPair;
            newPair.local = local_candidates_[0]; // Use first local candidate
            newPair.remote = prflxCandidate;
            newPair.computePriority(role_);
            newPair.state = PairState::Waiting;
            checklist_.addPair(newPair);
            checklist_.addTriggeredCheck(newPair);
        }
    }

    // ICE-Lite: if we receive USE-CANDIDATE, select the pair
    if (mode_ == IceMode::Lite && useCandidate && matchedPair) {
        matchedPair->nominated = true;
        matchedPair->state = PairState::Succeeded;
        matchedPair->valid = true;
        selected_pair_ = matchedPair;
        if (onSelectedPair) onSelectedPair(*selected_pair_);
        setState(IceState::Completed);
        startKeepalive();
    }
}

void IceSession::handleStunResponse(const StunMessage& msg, const struct sockaddr* from) {
    std::string key = transactionIdToKey(msg.transactionId());
    auto it = transactions_.find(key);
    if (it == transactions_.end()) return;

    StunTransaction& txn = it->second;

    if (txn.isGathering) {
        // This is a response to a gathering STUN request
        onGatheringResponse(msg, txn.serverAddr);
        pending_gathering_requests_--;
        if (txn.timer) htimer_del(txn.timer);
        transactions_.erase(it);
        if (pending_gathering_requests_ == 0) {
            onGatheringComplete();
        }
        return;
    }

    // Connectivity check response
    CandidatePair* pair = txn.pair;
    if (txn.timer) htimer_del(txn.timer);
    transactions_.erase(it);

    if (pair) {
        onCheckSuccess(pair, msg);
    }
}

void IceSession::handleStunErrorResponse(const StunMessage& msg, const struct sockaddr* from) {
    std::string key = transactionIdToKey(msg.transactionId());
    auto it = transactions_.find(key);
    if (it == transactions_.end()) return;

    StunTransaction& txn = it->second;
    CandidatePair* pair = txn.pair;
    if (txn.timer) htimer_del(txn.timer);
    transactions_.erase(it);

    uint16_t errorCode = 0;
    std::string reason;
    msg.getErrorCode(&errorCode, &reason);

    if (errorCode == STUN_ERROR_ROLE_CONFLICT) {
        // Switch role and retry
        handleRoleConflict(role_ == IceRole::Controlled);
        if (pair) {
            pair->state = PairState::Waiting;
            checklist_.addTriggeredCheck(*pair);
        }
        return;
    }

    if (pair) {
        onCheckFailure(pair, errorCode);
    }
}

void IceSession::onCheckSuccess(CandidatePair* pair, const StunMessage& response) {
    pair->state = PairState::Succeeded;
    pair->valid = true;

    // Register the valid pair for data routing
    if (udp_transport_) {
        udp_transport_->registerPair(pair->remote.addr, this);
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
            selected_pair_ = pair;
            if (onSelectedPair) onSelectedPair(*selected_pair_);
            setState(IceState::Completed);
            startKeepalive();
        } else if (nomination_ == NominationMode::Regular && !selected_pair_) {
            // Nominate the first successful pair
            nominate(pair);
        }
    } else {
        // Controlled: wait for USE-CANDIDATE from request
        if (pair->nominated) {
            selected_pair_ = pair;
            if (onSelectedPair) onSelectedPair(*selected_pair_);
            setState(IceState::Completed);
            startKeepalive();
        }
    }
}

void IceSession::onCheckFailure(CandidatePair* pair, uint16_t errorCode) {
    pair->state = PairState::Failed;

    if (checklist_.allFailed()) {
        setState(IceState::Failed);
    }
}

void IceSession::nominate(CandidatePair* pair) {
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
        pair.computePriority(role_);
    }
    checklist_.sort();
}

void IceSession::sendStunResponse(const StunMessage& request, const struct sockaddr* to) {
    StunMessage response(STUN_METHOD_BINDING, STUN_CLASS_SUCCESS_RESPONSE);
    response.setTransactionId(request.transactionId());

    // XOR-MAPPED-ADDRESS
    response.addXorMappedAddress(to);

    // Encode with MESSAGE-INTEGRITY using local password
    auto buf = response.encodeWithAuth(local_pwd_);

    if (udp_transport_) {
        udp_transport_->sendTo(buf.data(), buf.size(), to);
    }
}

void IceSession::sendStunErrorResponse(const StunMessage& request, uint16_t code,
                                        const std::string& reason, const struct sockaddr* to) {
    StunMessage response(STUN_METHOD_BINDING, STUN_CLASS_ERROR_RESPONSE);
    response.setTransactionId(request.transactionId());
    response.addErrorCode(code, reason);

    auto buf = response.encodeWithAuth(local_pwd_);

    if (udp_transport_) {
        udp_transport_->sendTo(buf.data(), buf.size(), to);
    }
}

void IceSession::addTransaction(const StunTransaction& txn) {
    std::string key = transactionIdToKey(txn.id);
    transactions_[key] = txn;

    // Set retransmit timer for UDP (not needed for TCP)
    StunTransaction& stored = transactions_[key];
    stored.timer = htimer_add(loop_->loop(), [](htimer_t* timer) {
        // Retransmit timeout
        IceSession* self = (IceSession*)hevent_userdata(timer);
        // TODO: implement retransmission logic
    }, txn.rto, 1); // one-shot
    hevent_set_userdata(stored.timer, this);
}

void IceSession::removeTransaction(const TransactionId& id) {
    std::string key = transactionIdToKey(id);
    auto it = transactions_.find(key);
    if (it != transactions_.end()) {
        if (it->second.timer) htimer_del(it->second.timer);
        transactions_.erase(it);
    }
}

StunTransaction* IceSession::findTransaction(const TransactionId& id) {
    std::string key = transactionIdToKey(id);
    auto it = transactions_.find(key);
    if (it != transactions_.end()) return &it->second;
    return nullptr;
}

int IceSession::send(const void* data, size_t len) {
    if (!selected_pair_) return -1;

    if (selected_pair_->local.protocol == TransportProtocol::UDP) {
        if (udp_transport_) {
            return udp_transport_->sendTo(data, len, &selected_pair_->remote.addr.sa);
        }
    } else if (selected_pair_->local.protocol == TransportProtocol::TCP) {
        if (tcp_transport_ && selected_pair_->tcpIo) {
            return tcp_transport_->send(selected_pair_->tcpIo, data, len);
        }
    }
    return -1;
}

void IceSession::onDataPacket(const uint8_t* data, size_t len, const struct sockaddr* from) {
    if (onData) {
        onData(data, len);
    }
}

void IceSession::onChannelData(const uint8_t* data, size_t len, const struct sockaddr* from) {
    // TURN ChannelData: strip 4-byte header and deliver
    if (len < 4) return;
    uint16_t dataLen = ((uint16_t)data[2] << 8) | data[3];
    if (4 + dataLen > len) return;
    if (onData) {
        onData(data + 4, dataLen);
    }
}

void IceSession::onTcpConnected(hio_t* io) {
    // Associate TCP connection with the matching pair
    struct sockaddr* peeraddr = hio_peeraddr(io);
    sockaddr_u peerAddr;
    memcpy(&peerAddr, peeraddr, SOCKADDR_LEN(peeraddr));

    for (auto& pair : checklist_.pairs()) {
        if (pair.local.protocol == TransportProtocol::TCP &&
            memcmp(&pair.remote.addr, &peerAddr, SOCKADDR_LEN(peeraddr)) == 0) {
            pair.tcpIo = io;
            // Now send the connectivity check
            sendConnectivityCheck(&pair);
            break;
        }
    }
}

void IceSession::onTcpDisconnected(hio_t* io) {
    // Mark associated pairs as failed
    for (auto& pair : checklist_.pairs()) {
        if (pair.tcpIo == io) {
            pair.tcpIo = nullptr;
            if (pair.state == PairState::InProgress) {
                pair.state = PairState::Failed;
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
    if (!selected_pair_ || !udp_transport_) return;

    // Send STUN binding indication (no transaction, no response expected)
    StunMessage msg(STUN_METHOD_BINDING, STUN_CLASS_INDICATION);
    auto buf = msg.encode();
    udp_transport_->sendTo(buf.data(), buf.size(), &selected_pair_->remote.addr.sa);
}

} // namespace ice
