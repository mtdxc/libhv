#pragma once
#include <map>
#include <string>
#include "reply.h"
#include "hv/hloop.h"
class RedisEvent {
public:
    virtual void onConnect() = 0;
    virtual void onDisconnect(int code) = 0;
    virtual void onMessage(const std::string& channel, const std::string& msg) = 0;
};

using MsgFunc = std::function<void(const std::string& channel, const std::string& msg)>;

class RedisClient {
    RedisEvent* event_ = nullptr;
    hloop_t* loop_ = nullptr;
    redisAsyncContext* ctx_ = nullptr;
    static void connectCallback(const redisAsyncContext* c, int status);
    static void disconnectCallback(const redisAsyncContext* c, int status);
    static void subsCallback(redisAsyncContext* ac, void* r, void* privdata);
    std::map<std::string, MsgFunc> subsMap;
    htimer_t* subs_timer_ = nullptr;
    MsgFunc getSubsCB(const std::string& channel) {
        MsgFunc ret;
        if (subsMap.count(channel)) ret = subsMap[channel];
        return ret;
    }
    void onConnect();
    void onDisconnect(int code);
public:
    RedisClient(hloop_t* loop) : loop_(loop) {}
    ~RedisClient() { close(); }
    void setEvent(RedisEvent* ev) {event_ = ev;}

    bool open(const char* host, int port);
    bool close();

    void auth(const char* key, const char* value, reply_cb_t cb);
    void get(const char* key, reply_cb_t cb);
    void set(const char* key, const std::string& value, reply_cb_t cb = nullptr);
    // ·¢²¼¶©ÔÄ
    void publish(const std::string& channel, const std::string& msg);
    void subscribe(const std::string& channel, MsgFunc cb);
    void unsubscribe(const std::string& channel);
};
