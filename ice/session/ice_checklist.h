#ifndef ICE_CHECKLIST_H_
#define ICE_CHECKLIST_H_

#include <vector>
#include <deque>

#include <algorithm>

#include "candidate_pair.h"

namespace ice {

// ICE Check List (RFC 8445 Section 6.1.2)
class IceCheckList {
public:
    IceCheckList() = default;

    // Add a candidate pair
    void addPair(CandidatePairPtr pair) {
        pairs_.push_back(pair);
    }

    // Sort pairs by priority (descending)
    void sort() {
        std::sort(pairs_.begin(), pairs_.end(),
                  [](const CandidatePairPtr& a, const CandidatePairPtr& b) {
                      return a->priority > b->priority;
                  });
    }

    // Prune redundant pairs (same foundation pair)
    void prune() {
        std::vector<CandidatePairPtr> pruned;
        pruned.reserve(pairs_.size());
        for (const auto& pair : pairs_) {
            auto it = std::find_if(pruned.begin(), pruned.end(), [&pair](const CandidatePairPtr& kept) {
                return isSameFoundationPair(*kept, *pair);
            });
            if (it == pruned.end()) {
                pruned.push_back(pair);
                continue;
            }
            if ((*it)->priority < pair->priority) {
                *it = pair;
            }
        }
        pairs_.swap(pruned);
    }

    // Add triggered check (higher priority than ordinary checks)
    void addTriggeredCheck(CandidatePairPtr pair) {
        triggered_queue_.push_back(pair);
    }

    // Get next pair to check
    // Returns nullptr if no pairs available for checking
    CandidatePairPtr getNextPair() {
        while (!triggered_queue_.empty()) {
            CandidatePairPtr queued = triggered_queue_.front();
            triggered_queue_.pop_front();

            CandidatePairPtr pair = findCanonicalPair(queued);
            if (!pair) {
                pair = queued;
            }
            if (pair->state == PairState::InProgress) {
                continue;
            }
            pair->state = PairState::InProgress;
            return pair;
        }

        if (auto pair = claimNextPair(PairState::Waiting)) {
            return pair;
        }
        if (auto pair = claimNextPair(PairState::Frozen)) {
            return pair;
        }
        return nullptr;
    }

    // Find pair by transaction ID
    CandidatePairPtr findByTransaction(const TransactionId& tid) {
        auto it = std::find_if(pairs_.begin(), pairs_.end(), [&tid](const CandidatePairPtr& pair) {
            return pair->transactionId == tid && pair->state == PairState::InProgress;
        });
        if (it != pairs_.end()) {
            return *it;
        }
        return nullptr;
    }

    // Find pair by local and remote candidate addresses
    CandidatePairPtr findByAddresses(const sockaddr_u& localAddr, const sockaddr_u& remoteAddr) {
        auto it = std::find_if(pairs_.begin(), pairs_.end(), [&localAddr, &remoteAddr](const CandidatePairPtr& pair) {
            return hasMatchingAddresses(*pair, localAddr, remoteAddr);
        });
        if (it != pairs_.end()) {
            return *it;
        }
        return nullptr;
    }

    // Get the nominated pair (selected pair)
    CandidatePairPtr getNominatedPair() {
        auto it = std::find_if(pairs_.begin(), pairs_.end(), [](const CandidatePairPtr& pair) {
            return pair->nominated && pair->state == PairState::Succeeded;
        });
        if (it != pairs_.end()) {
            return *it;
        }
        return nullptr;
    }

    // Get best valid pair
    CandidatePairPtr getBestValidPair() {
        auto it = std::find_if(pairs_.begin(), pairs_.end(), [](const CandidatePairPtr& pair) {
            return pair->valid && pair->state == PairState::Succeeded;
        });
        if (it != pairs_.end()) {
            return *it;
        }
        return nullptr;
    }

    // Check if all pairs are in terminal states (no more work to do)
    // InProgress pairs are still waiting for response, not complete yet
    bool isComplete() const {
        if (pairs_.empty()) return false;
        if (!triggered_queue_.empty()) return false;
        return std::none_of(pairs_.begin(), pairs_.end(), [](const CandidatePairPtr& pair) {
            return isPendingState(pair->state);
        });
    }

    // Check if all pairs failed
    bool allFailed() const {
        return !pairs_.empty() && std::all_of(pairs_.begin(), pairs_.end(), [](const CandidatePairPtr& pair) {
            return pair->state == PairState::Failed;
        });
    }

    // Has any succeeded pair
    bool hasSucceeded() const {
        return std::any_of(pairs_.begin(), pairs_.end(), [](const CandidatePairPtr& pair) {
            return pair->state == PairState::Succeeded;
        });
    }

    // Access pairs
    std::vector<CandidatePairPtr>& pairs() { return pairs_; }
    const std::vector<CandidatePairPtr>& pairs() const { return pairs_; }
    size_t size() const { return pairs_.size(); }
    bool empty() const { return pairs_.empty(); }

    // Set all Frozen to Waiting (initial unfreeze)
    void unfreezeAll() {
        for (const auto& pair : pairs_) {
            if (pair->state == PairState::Frozen) {
                pair->state = PairState::Waiting;
            }
        }
    }

private:
    static bool isPendingState(PairState state) {
        return state == PairState::Waiting ||
               state == PairState::Frozen ||
               state == PairState::InProgress;
    }

    static bool hasMatchingAddresses(const CandidatePair& pair,
                                     const sockaddr_u& localAddr,
                                     const sockaddr_u& remoteAddr) {
        return sockaddr_compare(&pair.local.addr, &localAddr) == 0 &&
               sockaddr_compare(&pair.remote.addr, &remoteAddr) == 0;
    }

    static bool isSameFoundationPair(const CandidatePair& lhs, const CandidatePair& rhs) {
        return lhs.local.foundation == rhs.local.foundation &&
               lhs.remote.foundation == rhs.remote.foundation;
    }

    CandidatePairPtr claimNextPair(PairState state) {
        auto it = std::find_if(pairs_.begin(), pairs_.end(), [state](const CandidatePairPtr& pair) {
            return pair->state == state;
        });
        if (it == pairs_.end()) {
            return nullptr;
        }
        (*it)->state = PairState::InProgress;
        return *it;
    }

    CandidatePairPtr findCanonicalPair(const CandidatePairPtr& pair) {
        auto it = std::find_if(pairs_.begin(), pairs_.end(), [&pair](const CandidatePairPtr& current) {
            return hasMatchingAddresses(*current, pair->local.addr, pair->remote.addr);
        });
        if (it != pairs_.end()) {
            return *it;
        }
        return nullptr;
    }

    std::vector<CandidatePairPtr> pairs_;
    std::deque<CandidatePairPtr> triggered_queue_;
};

} // namespace ice

#endif // ICE_CHECKLIST_H_
