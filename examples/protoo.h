#pragma once
#include <map>
#include <string>
#include "json.hpp"
using json = nlohmann::json;

#define REQUEST_OK 0
#define E_REQUEST_TIMEOUT -100
#define E_CONNECT_CLOSED -101
#define E_EXCEPTION -103
#define E_UNSUPPORT_METHOD -104

/*
请求的响应回调
@arg code 错误码, 0表示成功，其他为失败
@arg data 响应携带数据或错误原因
*/
using ResponseCB = std::function<void(int code, const json& data)>;
/*
API处理函数
@arg method 请求方法(@see request)
@arg data 请求携带数据(@see request)
@arg cb 响应回调，既可异步发送也可同步发送
*/
using ApiCB = std::function<void(const char* method, const json& data, ResponseCB cb)>;
// 通知回调，携带通知内容
using NotifyCB = std::function<void(const char* method, const json& data)>;

class Protoo {
    struct RequestItem
    {
        ResponseCB cb;
        uint64_t timer = -1;
        void callback(int code, const json& data);
    };
    int curReq = 0;
    std::map<int, RequestItem> reqMap;
    std::map<std::string, NotifyCB> notifyMap;
    std::map<std::string, ApiCB> apiMap;
public:
    // send json msg to protoo
    void recvMsg(const std::string& msg);
    // call when protoo need to send msg
    std::function<void (const std::string&)> sendMsg;

    Protoo() {}
    // create from templ
    Protoo(const Protoo& proto) {
        notifyMap = proto.notifyMap;
        apiMap = proto.apiMap;        
    }
    ~Protoo() { 
        clearRequests();
        apiMap.clear();
        notifyMap.clear();
    }
    void clearRequests();

    void request(const std::string& method, const json& data = nullptr, ResponseCB cb = nullptr);
    void notify(const char* method, const json& data = nullptr);

    void on_notify(const char* method, NotifyCB cb) {
        notifyMap[method] = cb;
    }
    int off_notify(const char* method) {
        return notifyMap.erase(method);
    }
    int off_notify() {
        int ret = notifyMap.size();
        notifyMap.clear();
        return ret;
    }

    void on_requset(const char* method, ApiCB cb) {
        apiMap[method] = cb;
    }

    int off_requset(const char* method) {
        return apiMap.erase(method);
    }
    int off_requset() {
        int ret = apiMap.size();
        apiMap.clear();
        return ret;
    }
};
