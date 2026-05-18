#ifndef ICE_CHECKLIST_H_
#define ICE_CHECKLIST_H_

#include <vector>
#include <deque>
#include <algorithm>
#include <functional>

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
    void prune();

    // Add triggered check (higher priority than ordinary checks)
    void addTriggeredCheck(CandidatePairPtr pair) {
        triggered_queue_.push_back(pair);
    }

    // Get next pair to check
    // Returns nullptr if no pairs available for checking
    CandidatePairPtr getNextPair() {
        // Triggered checks first
        if (!triggered_queue_.empty()) {
            // Move triggered to in-progress in main list
            CandidatePairPtr tp = triggered_queue_.front();
            for (auto& p : pairs_) {
                if (p->local.foundation == tp->local.foundation &&
                    p->remote.foundation == tp->remote.foundation &&
                    p->state != PairState::InProgress) {
                    p->state = PairState::InProgress;
                    triggered_queue_.pop_front();
                    return p;
                }
            }
            triggered_queue_.pop_front();
        }

        // Ordinary checks: find highest priority Waiting pair
        for (auto& p : pairs_) {
            if (p->state == PairState::Waiting) {
                p->state = PairState::InProgress;
                return p;
            }
        }

        // If no Waiting, unfreeze Frozen pairs
        for (auto& p : pairs_) {
            if (p->state == PairState::Frozen) {
                p->state = PairState::InProgress;
                return p;
            }
        }

        return nullptr;
    }

    // Find pair by transaction ID
    CandidatePairPtr findByTransaction(const TransactionId& tid) {
        for (auto& p : pairs_) {
            if (p->transactionId == tid && p->state == PairState::InProgress) {
                return p;
            }
        }
        return nullptr;
    }

    // Find pair by local and remote candidate addresses
    CandidatePairPtr findByAddresses(const sockaddr_u& localAddr, const sockaddr_u& remoteAddr);

    // Get the nominated pair (selected pair)
    CandidatePairPtr getNominatedPair() {
        for (auto& p : pairs_) {
            if (p->nominated && p->state == PairState::Succeeded) {
                return p;
            }
        }
        return nullptr;
    }

    // Get best valid pair
    CandidatePairPtr getBestValidPair() {
        for (auto& p : pairs_) {
            if (p->valid && p->state == PairState::Succeeded) {
                return p;
            }
        }
        return nullptr;
    }

    // Check if all pairs are in terminal states
    bool isComplete() const {
        for (const auto& p : pairs_) {
            if (p->state != PairState::Succeeded && p->state != PairState::Failed) {
                return false;
            }
        }
        return !pairs_.empty();
    }

    // Check if all pairs failed
    bool allFailed() const {
        for (const auto& p : pairs_) {
            if (p->state != PairState::Failed) return false;
        }
        return !pairs_.empty();
    }

    // Has any succeeded pair
    bool hasSucceeded() const {
        for (const auto& p : pairs_) {
            if (p->state == PairState::Succeeded) return true;
        }
        return false;
    }

    // Access pairs
    std::vector<CandidatePairPtr>& pairs() { return pairs_; }
    const std::vector<CandidatePairPtr>& pairs() const { return pairs_; }
    size_t size() const { return pairs_.size(); }
    bool empty() const { return pairs_.empty(); }

    // Set all Frozen to Waiting (initial unfreeze)
    void unfreezeAll() {
        for (auto p : pairs_) {
            if (p->state == PairState::Frozen) {
                p->state = PairState::Waiting;
            }
        }
    }

private:
    std::vector<CandidatePairPtr> pairs_;
    std::deque<CandidatePairPtr> triggered_queue_;
};

} // namespace ice

#endif // ICE_CHECKLIST_H_
