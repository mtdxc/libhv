#ifndef ICE_CANDIDATE_PAIR_H_
#define ICE_CANDIDATE_PAIR_H_

#include "ice_candidate.h"
#include "../stun/stun_message.h"
#include <cstdint>
#include <memory>

namespace ice {

// Candidate Pair State (RFC 8445 Section 6.1.2.6)
enum class PairState {
    Frozen,       // Initial state, not yet triggered
    Waiting,      // Ready to be checked
    InProgress,   // STUN check sent, waiting response
    Succeeded,    // Check succeeded
    Failed        // Check failed
};

// ICE Role
enum class IceRole {
    Controlling,
    Controlled
};

// Candidate Pair (RFC 8445 Section 6.1.2)
struct CandidatePair {
    IceCandidate local;
    IceCandidate remote;
    PairState state = PairState::Frozen;
    uint64_t priority = 0;
    bool nominated = false;   // USE-CANDIDATE flag
    bool valid = false;       // Appears in valid list

    // For TCP connections
    hio_t* tcpIo = nullptr;

    // Transaction tracking
    TransactionId transactionId;
    int retransmitCount = 0;
    uint64_t lastSendTime = 0; // ms

    // Compute pair priority (RFC 8445 Section 6.1.2.3)
    static uint64_t computePairPriority(uint32_t controllingPriority,
                                         uint32_t controlledPriority,
                                         IceRole localRole) {
        uint64_t g, d;
        if (localRole == IceRole::Controlling) {
            g = controllingPriority;
            d = controlledPriority;
        } else {
            g = controlledPriority;
            d = controllingPriority;
        }
        // pair priority = 2^32 * MIN(G,D) + 2 * MAX(G,D) + (G>D ? 1 : 0)
        uint64_t minGD = (g < d) ? g : d;
        uint64_t maxGD = (g > d) ? g : d;
        return (minGD << 32) + 2 * maxGD + ((g > d) ? 1 : 0);
    }

    void computePriority(IceRole role) {
        priority = computePairPriority(local.priority, remote.priority, role);
    }

    // Comparison for sorting (higher priority first)
    bool operator>(const CandidatePair& other) const {
        return priority > other.priority;
    }

    bool operator<(const CandidatePair& other) const {
        return priority < other.priority;
    }
};

} // namespace ice

#endif // ICE_CANDIDATE_PAIR_H_
