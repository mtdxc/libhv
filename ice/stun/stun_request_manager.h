#ifndef STUN_REQUEST_MANAGER_H_
#define STUN_REQUEST_MANAGER_H_

#include <functional>
#include <map>
#include <vector>

#include "EventLoop.h"
#include "../stun/stun_message.h"

namespace ice {
struct StunTransaction;
using StunCallback = std::function<void(StunMessage* resp, int code)>;
using SendFunc = std::function<int(const void* data, size_t len)>;

class StunRequestManager {
public:
    StunRequestManager(hv::EventLoopPtr loop);
    ~StunRequestManager();

    void request(const StunMessage& req, SendFunc sendFunc, StunCallback callback);
    bool handleResponse(StunMessage& response, int code = 0);
    bool popCallBack(const TransactionId& id, StunCallback& cb);
    void clear();

private:
    void onRetransmit(StunTransaction* txn);

    hv::EventLoopPtr loop_;
    std::map<TransactionId, StunTransaction*> transactions_;
};

} // namespace ice

#endif // STUN_REQUEST_MANAGER_H_
