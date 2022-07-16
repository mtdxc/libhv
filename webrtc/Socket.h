#ifndef SRC_SOCKET_TOOLKIT_H_
#define SRC_SOCKET_TOOLKIT_H_

#include "Channel.h"
#include "EventLoop.h"
//错误类型枚举
typedef enum {
    Err_success = 0, //成功
    Err_eof, //eof
    Err_timeout, //超时
    Err_refused,//连接被拒绝
    Err_dns,//dns解析失败
    Err_shutdown,//主动关闭
    Err_other = 0xFF,//其他错误
} ErrCode;

//错误信息类
class SockException : public std::exception {
public:
    SockException(ErrCode code = Err_success, const std::string& msg = "", int custom_code = 0) {
        _msg = msg;
        _code = code;
        _custom_code = custom_code;
    }

    //重置错误
    void reset(ErrCode code, const std::string& msg, int custom_code = 0) {
        _msg = msg;
        _code = code;
        _custom_code = custom_code;
    }

    //错误提示
    const char* what() const noexcept override {
        return _msg.c_str();
    }

    //错误代码
    ErrCode getErrCode() const {
        return _code;
    }

    //用户自定义错误代码
    int getCustomCode() const {
        return _custom_code;
    }

    //判断是否真的有错
    operator bool() const {
        return _code != Err_success;
    }

private:
    ErrCode _code;
    int _custom_code = 0;
    std::string _msg;
};


namespace mediakit {

using Session = hv::SocketChannel;
using SockInfo = hv::SocketChannel;
using EventPoller = hv::EventLoop;

class Timer {
    // return false to killtimer
    using TimeCB = std::function<bool()>;
    htimer_t* _timer;
    TimeCB _cb;
    std::weak_ptr<EventPoller> _loop;
public:
    using Ptr = std::shared_ptr<Timer>;
    Timer(float sec, TimeCB cb, std::shared_ptr<EventPoller> pool) : _cb(cb), _loop(pool){
        _timer = htimer_add(pool->loop(), [](htimer_t* timer) {
            Timer* pThis = (Timer*)hevent_userdata(timer);
            if (pThis && pThis->_cb())
                return;
            htimer_del(timer);
        }, sec * 1000, -1);
        hevent_set_userdata(_timer, this);
    }

    ~Timer() {
        if (auto loop = _loop.lock()) {
            auto timer = std::move(_timer);
            loop->runInLoop([timer] {
                htimer_del(timer);
            });
        }
    }
};
}
#define TraceP(ptr) TraceL << ptr->peeraddr()
#define DebugP(ptr) DebugL << ptr->peeraddr()
#define InfoP(ptr) InfoL << ptr->peeraddr()
#define WarnP(ptr) WarnL << ptr->peeraddr()
#define ErrorP(ptr) ErrorL << ptr->peeraddr()

#endif