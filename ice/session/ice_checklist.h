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
        while (!triggered_queue_.empty()) {
            CandidatePairPtr tp = triggered_queue_.front();
            triggered_queue_.pop_front();
            // Find the canonical pair in pairs_ by address (not foundation)
            for (auto& p : pairs_) {
                if (sockaddr_compare(&p->local.addr, &tp->local.addr) == 0 &&
                    sockaddr_compare(&p->remote.addr, &tp->remote.addr) == 0) {
                    if (p->state == PairState::InProgress) {
                        // Already in flight, skip – response will come
                        break;
                    }
                    p->state = PairState::InProgress;
                    return p;
                }
            }
            // tp not found in pairs_ (e.g. new prflx pair added directly)
            if (tp->state != PairState::InProgress) {
                tp->state = PairState::InProgress;
                return tp;
            }
        }

        // Ordinary checks: find highest priority Waiting pair
        for (auto& p : pairs_) {
            if (p->state == PairState::Waiting) {
                p->state = PairState::InProgress;
                return p;
            }
        }

        // If no Waiting, unfreeze Frozen pairs one at a time
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

    // Check if all pairs are in terminal states (no more work to do)
    // InProgress pairs are still waiting for response, not complete yet
    bool isComplete() const {
        if (pairs_.empty()) return false;
        if (!triggered_queue_.empty()) return false;
        for (const auto& p : pairs_) {
            if (p->state == PairState::Waiting ||
                p->state == PairState::Frozen ||
                p->state == PairState::InProgress) {
                return false;
            }
        }
        return true;
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
