#include "stun_request_manager.h"

#include "hlog.h"

#include <algorithm>

namespace ice {

static constexpr int MAX_RETRANSMIT = 4;  // ICE check: 4 retransmits (~1.5s max)
static constexpr uint32_t MAX_RTO = 800;  // Cap RTO at 800ms
struct StunTransaction {
    TransactionId id;
    StunCallback callback;
    SendFunc sendFunc;
    std::vector<uint8_t> msg;
    StunRequestManager* manager = nullptr;

    uint64_t sentTime = 0;
    int retransmitCount = 0;
    uint32_t rto = 50;
    htimer_t* timer = nullptr;

    ~StunTransaction();
};

StunTransaction::~StunTransaction() {
    if (timer) {
        htimer_del(timer);
        timer = nullptr;
    }
}

StunRequestManager::StunRequestManager(hv::EventLoopPtr loop)
    : loop_(loop) {}

StunRequestManager::~StunRequestManager() {
    clear();
}

void StunRequestManager::request(const StunMessage& req, SendFunc sendFunc, StunCallback callback) {
    if (!sendFunc) return;

    auto encoded = req.encode();
    sendFunc(encoded.data(), encoded.size());
    if (!callback) return;

    auto* txn = new StunTransaction();
    txn->id = req.transactionId();
    txn->msg = std::move(encoded);
    txn->callback = std::move(callback);
    txn->manager = this;
    txn->sendFunc = std::move(sendFunc);
    txn->sentTime = hloop_now_ms(loop_->loop());
    txn->rto = 50; // RFC 5389 initial RTO (50ms for ICE checks)

    // StunTransaction* itself is the timer userdata.
    htimer_t* timer = htimer_add(loop_->loop(), [](htimer_t* t) {
        auto* txn = (StunTransaction*)hevent_userdata(t);
        if (txn && txn->manager) {
            txn->manager->onRetransmit(txn);
        }
    }, txn->rto, 0);
    hevent_set_userdata(timer, txn);
    txn->timer = timer;

    transactions_[txn->id] = txn;
}

bool StunRequestManager::popCallBack(const TransactionId& id, StunCallback& cb) {
    auto it = transactions_.find(id);
    if (it != transactions_.end()) {
        StunTransaction* txn = it->second;
        cb = std::move(txn->callback);
        transactions_.erase(it);
        delete txn;
        return true;
    }
    return false;
}

bool StunRequestManager::handleResponse(StunMessage& response, int code) {
    StunCallback cb;
    bool ret = popCallBack(response.transactionId(), cb);
    if (ret) {
        cb(&response, code);
    }
    return ret;
}

void StunRequestManager::clear() {
    for (auto& kv : transactions_) {
        delete kv.second;
    }
    transactions_.clear();
}

void StunRequestManager::onRetransmit(StunTransaction* txn) {
    auto it = transactions_.find(txn->id);
    if (it == transactions_.end() || it->second != txn) return;

    auto id = TransactionIdStr(txn->id);
    if (txn->retransmitCount >= MAX_RETRANSMIT) {
        hlogi("StunRequestManager onRetransmit %s: timed out after %d retries",
              id.c_str(), txn->retransmitCount);
        if (txn->callback) {
            txn->callback(nullptr, -1);
        }
        transactions_.erase(it);
        delete txn;
        return;
    }

    if (txn->sendFunc) {
        txn->sendFunc(txn->msg.data(), txn->msg.size());
    }
    txn->retransmitCount++;
    hlogd("StunRequestManager onRetransmit %s: retransmit #%d rto=%u",
          id.c_str(), txn->retransmitCount, txn->rto);
    if (txn->timer) {
        htimer_del(txn->timer);
    }
    txn->rto = (std::min)(txn->rto * 2, MAX_RTO);
    txn->timer = htimer_add(loop_->loop(), [](htimer_t* t) {
        auto* txn = (StunTransaction*)hevent_userdata(t);
        if (txn && txn->manager) {
            txn->manager->onRetransmit(txn);
        }
    }, txn->rto, 0);
    hevent_set_userdata(txn->timer, txn);
}

} // namespace ice
