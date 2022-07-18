#ifndef SRC_SOCKET_TOOLKIT_H_
#define SRC_SOCKET_TOOLKIT_H_

#include "Channel.h"
#include "EventLoop.h"
namespace toolkit {
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

class Session : public hv::SocketChannel {
public:
    typedef std::shared_ptr<Session> Ptr;
    Session(hio_t* io) : hv::SocketChannel(io) {
        setHeartbeat(2000, [this]() {
            onManager();
        });
        if (hio_getcb_close(io) == NULL) {
            hio_setcb_close(io_, [](hio_t* io) {
                Session* channel = (Session*)hio_context(io);
                if (channel) {
                    channel->status = CLOSED;
                    channel->onError(SockException(Err_eof, "socket closed"));
                    if (channel->onclose) {
                        channel->onclose();
                    }
                }
            });
        }
    }

    virtual void onError(const SockException &err) {}
    virtual void onManager() {}
    void shutdown(const SockException& e, bool safe = false) {
        hlogi("%p shutdown %s", this, e.what());
        if (!isClosed())
            onError(e);
        close(safe);
    }
};

using SockInfo = hv::SocketChannel;
using EventPoller = hv::EventLoop;

class Timer {
    // return false to killtimer
    using TimeCB = std::function<bool()>;
    hv::TimerID _timer;
    TimeCB _cb;
    EventPoller* _loop;
public:
    using Ptr = std::shared_ptr<Timer>;
    Timer(float sec, TimeCB cb, std::shared_ptr<EventPoller> pool) : _cb(cb) {
        _loop = pool ? pool.get() : hv::tlsEventLoop();
        _timer = _loop->setInterval(sec * 1000, [this](hv::TimerID tid){
            if(!_cb()) {
                //hv::killTimer(tid);
                cancel();
            }
        });
    }
    void cancel() {
        if (_loop && _timer) {
            _loop->killTimer(_timer);
            _timer = 0;
        }
    }
    ~Timer() {
        cancel();
    }
};
}
#define TraceP(ptr) TraceL << ptr->peeraddr() << " "
#define DebugP(ptr) DebugL << ptr->peeraddr() << " "
#define InfoP(ptr) InfoL << ptr->peeraddr() << " "
#define WarnP(ptr) WarnL << ptr->peeraddr() << " "
#define ErrorP(ptr) ErrorL << ptr->peeraddr() << " "

#endif