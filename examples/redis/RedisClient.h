#pragma once
#include <map>
#include <string>
#include "reply.h"
#include "hloop.h"
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
    std::string auth_;
    std::string host_;
    int port_ = 6379;
    int connect_timeout = 10000;
    reconn_setting_t* reconn_setting = nullptr;
    void onConnect();
    void onDisconnect(int code);
    int startReconnect();
    int startConnect();

    static void connectCallback(const redisAsyncContext* c, int status);
    static void disconnectCallback(const redisAsyncContext* c, int status);
    static void subsCallback(redisAsyncContext* ac, void* r, void* privdata);
    static void authCallback(redisAsyncContext* ac, void* r, void* privdata);
    htimer_t* subs_timer_ = nullptr;
    std::map<std::string, MsgFunc> subsMap;
    void checkSubsTimer();
    MsgFunc getSubsCB(const std::string& channel) {
        MsgFunc ret;
        if (subsMap.count(channel)) ret = subsMap[channel];
        return ret;
    }
public:
    RedisClient(hloop_t* loop) : loop_(loop) {}
    ~RedisClient();
    void setEvent(RedisEvent* ev) {event_ = ev;}
    void setConnectTimeout(int ms) {
        connect_timeout = ms;
    }

    void setReconnect(reconn_setting_t* setting);
    bool isReconnect() {
        return reconn_setting && reconn_setting->cur_retry_cnt > 0;
    }

    bool open(const char* host, int port = 6379, const char* auth = nullptr);
    bool close();

    void get(const char* key, reply_cb_t cb);
    void set(const char* key, const std::string& value, reply_cb_t cb = nullptr);

    // pub sub
    void publish(const std::string& channel, const std::string& msg);
    void subscribe(const std::string& channel, MsgFunc cb);
    void unsubscribe(const std::string& channel);
};
