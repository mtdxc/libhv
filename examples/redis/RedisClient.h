#pragma once
#include "reply.h"
#include "hv/hloop.h"
class RedisEvent {
public:
    virtual void onConnect() = 0;
    virtual void onDisconnect(int code) = 0;
    // subscribe
    virtual void onMessage(const char* channel, const char* msg);
};

class RedisClient {
    RedisEvent* event_ = nullptr;
    hloop_t* loop_ = nullptr;
    redisAsyncContext* ctx_ = nullptr;
    static void connectCallback(const redisAsyncContext* c, int status);
    static void disconnectCallback(const redisAsyncContext* c, int status);
public:
    RedisClient(hloop_t* loop) : loop_(loop) {}
    void setEvent(RedisEvent* ev) {event_ = ev;}

    bool open(const char* host, int port);
    bool close();

    void auth(const char* key, const char* value, reply_cb_t cb);
    void get(const char* key, reply_cb_t cb);
    void set(const char* key, const std::string& value, reply_cb_t cb = nullptr);
};
