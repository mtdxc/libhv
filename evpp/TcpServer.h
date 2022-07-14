#ifndef HV_TCP_SERVER_HPP_
#define HV_TCP_SERVER_HPP_

#include "hsocket.h"
#include "hssl.h"
#include "hlog.h"

#include "EventLoopThreadPool.h"
#include "Channel.h"

namespace hv {

template<class TSocketChannel = SocketChannel>
class TcpServerEventLoopTmpl {
public:
    typedef std::shared_ptr<TSocketChannel> TSocketChannelPtr;

    TcpServerEventLoopTmpl(EventLoopPtr loop = NULL, bool new_worker = false) {
        owner_worker = new_worker;
        if (owner_worker) {
            worker_threads = std::make_shared<EventLoopThreadPool>();
        }
        else {
            worker_threads = EventLoopThreadPool::Instance();
            if (!loop) loop = worker_threads->loop(0);
        }
        acceptor_loop = loop ? loop : std::make_shared<EventLoop>();
        listenfd = -1;
        tls = false;
        unpack_setting.mode = UNPACK_MODE_NONE;
        max_connections = 0xFFFFFFFF;
        load_balance = LB_RoundRobin;
    }

    virtual ~TcpServerEventLoopTmpl() {
    }
    EventLoopThreadPool* loops() { return worker_threads.get(); }
    EventLoopPtr loop(int idx = -1) {
        return worker_threads->loop(idx);
    }
    EventLoopPtr nextLoop(sockaddr_u* addr) {
        return worker_threads->nextLoop(load_balance, addr);
    }
    //@retval >=0 listenfd, <0 error
    int createsocket(int port, const char* host = "0.0.0.0") {
        listenfd = Listen(port, host);
        return listenfd;
    }
    // closesocket thread-safe
    void closesocket() {
        if (listenfd >= 0) {
            hio_close_async(hio_get(acceptor_loop->loop(), listenfd));
            listenfd = -1;
        }
    }

    void setMaxConnectionNum(uint32_t num) {
        max_connections = num;
    }

    void setLoadBalance(load_balance_e lb) {
        load_balance = lb;
    }

    // NOTE: totalThreadNum = 1 acceptor_thread + N worker_threads (N can be 0)
    void setThreadNum(int num) {
        worker_threads->setThreadNum(num);
    }

    int startAccept() {
        assert(listenfd >= 0);
        hio_t* listenio = haccept(acceptor_loop->loop(), listenfd, onAccept);
        hevent_set_userdata(listenio, this);
        if (tls) {
            hio_enable_ssl(listenio);
        }
        return 0;
    }

    // start thread-safe
    void start(bool wait_threads_started = true) {
        if (worker_threads->threadNum() > 0) {
            worker_threads->start(wait_threads_started);
        }
        acceptor_loop->runInLoop(std::bind(&TcpServerEventLoopTmpl::startAccept, this));
    }
    // stop thread-safe
    void stop(bool wait_threads_stopped = true) {
        if (owner_worker && worker_threads->threadNum() > 0) {
            worker_threads->stop(wait_threads_stopped);
        }
    }

    int withTLS(hssl_ctx_opt_t* opt = NULL) {
        tls = true;
        if (opt) {
            opt->endpoint = HSSL_SERVER;
            if (hssl_ctx_init(opt) == NULL) {
                fprintf(stderr, "hssl_ctx_init failed!\n");
                return -1;
            }
        }
        return 0;
    }

    void setUnpack(unpack_setting_t* setting) {
        if (setting) {
            unpack_setting = *setting;
        } else {
            unpack_setting.mode = UNPACK_MODE_NONE;
        }
    }

    // channel
    const TSocketChannelPtr& addChannel(hio_t* io) {
        uint32_t id = hio_id(io);
        auto channel = TSocketChannelPtr(new TSocketChannel(io));
        std::lock_guard<std::mutex> locker(mutex_);
        channels[id] = channel;
        return channels[id];
    }

    TSocketChannelPtr getChannelById(uint32_t id) {
        std::lock_guard<std::mutex> locker(mutex_);
        auto iter = channels.find(id);
        return iter != channels.end() ? iter->second : NULL;
    }

    void removeChannel(const TSocketChannelPtr& channel) {
        uint32_t id = channel->id();
        std::lock_guard<std::mutex> locker(mutex_);
        channels.erase(id);
    }

    size_t connectionNum() {
        std::lock_guard<std::mutex> locker(mutex_);
        return channels.size();
    }

    int foreachChannel(std::function<void(const TSocketChannelPtr& channel)> fn) {
        std::lock_guard<std::mutex> locker(mutex_);
        for (auto& pair : channels) {
            fn(pair.second);
        }
        return channels.size();
    }

    // broadcast thread-safe
    int broadcast(const void* data, int size) {
        return foreachChannel([data, size](const TSocketChannelPtr& channel) {
            channel->write(data, size);
        });
    }

    int broadcast(const std::string& str) {
        return broadcast(str.data(), str.size());
    }

private:
    static void newConnEvent(hio_t* connio) {
        TcpServerEventLoopTmpl* server = (TcpServerEventLoopTmpl*)hevent_userdata(connio);
        if (server->connectionNum() >= server->max_connections) {
            hlogw("over max_connections");
            hio_close(connio);
            return;
        }

        // NOTE: attach to worker loop
        EventLoop* worker_loop = currentThreadEventLoop;
        assert(worker_loop != NULL);
        hio_attach(worker_loop->loop(), connio);

        const TSocketChannelPtr& channel = server->addChannel(connio);
        channel->status = SocketChannel::CONNECTED;

        channel->onread = [server, &channel](Buffer* buf) {
            if (server->onMessage) {
                server->onMessage(channel, buf);
            }
        };
        channel->onwrite = [server, &channel](Buffer* buf) {
            if (server->onWriteComplete) {
                server->onWriteComplete(channel, buf);
            }
        };
        channel->onclose = [server, &channel]() {
            EventLoop* worker_loop = currentThreadEventLoop;
            assert(worker_loop != NULL);
            --worker_loop->connectionNum;

            channel->status = SocketChannel::CLOSED;
            if (server->onConnection) {
                server->onConnection(channel);
            }
            server->removeChannel(channel);
            // NOTE: After removeChannel, channel may be destroyed,
            // so in this lambda function, no code should be added below.
        };

        if (server->unpack_setting.mode != UNPACK_MODE_NONE) {
            channel->setUnpack(&server->unpack_setting);
        }
        channel->startRead();
        if (server->onConnection) {
            server->onConnection(channel);
        }
    }

    static void onAccept(hio_t* connio) {
        TcpServerEventLoopTmpl* server = (TcpServerEventLoopTmpl*)hevent_userdata(connio);
        // NOTE: detach from acceptor loop
        hio_detach(connio);
        EventLoopPtr worker_loop = server->nextLoop((sockaddr_u*)hio_peeraddr(connio));
        if (worker_loop == NULL) {
            worker_loop = server->acceptor_loop;
        }
        ++worker_loop->connectionNum;
        worker_loop->runInLoop(std::bind(&TcpServerEventLoopTmpl::newConnEvent, connio));
    }

public:
    int                     listenfd;
    bool                    tls;
    unpack_setting_t        unpack_setting;
    // Callback
    std::function<void(const TSocketChannelPtr&)>           onConnection;
    std::function<void(const TSocketChannelPtr&, Buffer*)>  onMessage;
    // NOTE: Use Channel::isWriteComplete in onWriteComplete callback to determine whether all data has been written.
    std::function<void(const TSocketChannelPtr&, Buffer*)>  onWriteComplete;

    uint32_t                max_connections;
    load_balance_e          load_balance;

private:
    // id => TSocketChannelPtr
    std::map<uint32_t, TSocketChannelPtr>   channels; // GUAREDE_BY(mutex_)
    std::mutex                              mutex_;

    EventLoopPtr             acceptor_loop;
    EventLoopThreadPool::Ptr worker_threads;
    bool owner_worker;
};

template<class TSocketChannel = SocketChannel>
class TcpServerTmpl : private EventLoopThread, public TcpServerEventLoopTmpl<TSocketChannel> {
public:
    TcpServerTmpl(bool new_worker = false) : EventLoopThread()
        , TcpServerEventLoopTmpl<TSocketChannel>(EventLoopThread::loop(), new_worker)
    {}
    virtual ~TcpServerTmpl() {
        stop(true);
    }

    EventLoopPtr loop(int idx = -1) {
        return TcpServerEventLoopTmpl<TSocketChannel>::loop(idx);
    }

    // start thread-safe
    void start(bool wait_threads_started = true) {
        TcpServerEventLoopTmpl<TSocketChannel>::start(wait_threads_started);
        EventLoopThread::start(wait_threads_started);
    }

    // stop thread-safe
    void stop(bool wait_threads_stopped = true) {
        EventLoopThread::stop(wait_threads_stopped);
        TcpServerEventLoopTmpl<TSocketChannel>::stop(wait_threads_stopped);
    }
};

typedef TcpServerTmpl<SocketChannel> TcpServer;

}

#endif // HV_TCP_SERVER_HPP_
