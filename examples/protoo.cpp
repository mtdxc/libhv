#include "protoo.h"
#include "EventLoop.h"

void EmptyCB(int code, const json& resp) {

}

std::string createSuccessResponse(int id, const json& data)
{
    json root;
    root["response"] = true;
    root["id"] = id;
    root["ok"] = true;
    if (data)
        root["data"] = data;
    return root.dump();
}

std::string createErrorResponse(int id, int errorCode, const std::string& errorReason)
{
    json root;
    root["response"] = true;
    root["id"] = id;
    root["errorCode"] = errorCode;
    if (errorReason.length())
        root["errorReason"] = errorReason;
    return root.dump();
}

void Protoo::RequestItem::callback(int code, const json& data)
{
    if (cb) {
        cb(code, data);
        cb = nullptr;
    }
    if (-1 != timer) {
        hv::killTimer(timer);
        timer = -1;
    }
}

void Protoo::clearRequests()
{
    for (auto it = reqMap.begin(); it != reqMap.end(); it++) {
        it->second.callback(E_CONNECT_CLOSED, "connection close");
    }
    reqMap.clear();
}

void Protoo::recvMsg(const std::string& msg)
{
    json root = json::parse(msg);
    auto it = root.find("request");
    if (it != root.end() && it->get<bool>()) {
        ResponseCB cb = EmptyCB;
        it = root.find("id");
        if (it != root.end()) {
            int reqId = it->get<int>();
            cb = [reqId, this](int code, const json& resp) {
                if (code == REQUEST_OK) {
                    sendMsg(createSuccessResponse(reqId, resp));
                }
                else {
                    sendMsg(createErrorResponse(reqId, code, resp.dump()));
                }
            };
        }
        std::string method = root["method"].get<std::string>();
        auto itA = apiMap.find(method);
        if (itA != apiMap.end()) {
            try {
                itA->second(method.c_str(), root["data"], cb);
            }
            catch(std::exception e) {
                printf("process request %s error %s\n", method.c_str(), e.what()); 
                cb(E_EXCEPTION, e.what());
            }
        }
        else {
            cb(E_UNSUPPORT_METHOD, "unsupport method");
        }
        return;
    }

    it = root.find("response");
    if (it != root.end() && it->get<bool>()) {
        try {
            RequestItem ri;
            int reqId = root["id"].get<int>();
            auto itR = reqMap.find(reqId);
            if (itR != reqMap.end()) {
                ri = itR->second;
                // erase it
                reqMap.erase(itR);
            }
            // else return;

            it = root.find("ok");
            if (it != root.end() && it->get<bool>()) {
                ri.callback(REQUEST_OK, root["data"]);
            }
            else {
                int code = root["errorCode"].get<int>();
                ri.callback(code, root["reason"]);
            }
        } catch (std::exception e){
           printf("process notify error %s\n", e.what()); 
        }
        return;
    }
    
    it = root.find("notification");
    if (it != root.end() && it->get<bool>()) {
        std::string method = root["method"].get<std::string>();
        auto itN = notifyMap.find(method);
        if (itN != notifyMap.end())
            itN->second(method.c_str(), root["data"]);
        else
            printf("skip unknown notify %s\n", method.c_str());
        return;
    }
}

void Protoo::request(const std::string& method, const json& data, ResponseCB cb)
{
    json root;
    root["request"] = true;
    // root["id"] = ++curReq;
    root["method"] = method;
    if (data)
        root["data"] = data;
    if (cb) {
        int reqId = curReq++;
        root["id"] = reqId;
        RequestItem ri;
        ri.cb = cb;
        int timeout = 2000 * (15 + (0.1 * reqMap.size()));
        ri.timer = hv::setTimeout(timeout, [reqId, this](hv::TimerID id) {
            auto iter = reqMap.find(reqId);
            if (iter != reqMap.end()) {
                iter->second.cb(E_REQUEST_TIMEOUT, "timeout");
                reqMap.erase(iter);
            }
        });
        reqMap[reqId] = ri;
    }
    sendMsg(root.dump());
}

void Protoo::notify(const char* method, const json& data /*= nullptr*/)
{
    json root;
    root["notification"] = true;
    root["method"] = method;
    if (data)
        root["data"] = data;
    sendMsg(root.dump());
}

