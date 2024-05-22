#include "RedisClient.h"
#include <async.h>
#include <hiredis.h>
#include "hlog.h"
#include <adapters/libhv.h>

void debugCallback(redisAsyncContext *c, void *r, void *privdata) {
    redisReply *rp = (redisReply*)r;
    if (rp == NULL) {
        printf("`DEBUG SLEEP` error: %s\n", c->errstr ? c->errstr : "unknown error");
        return;
    }
}

void RedisClient::connectCallback(const redisAsyncContext *c, int status) {
    RedisClient *self = (RedisClient*)c->data;
    if (status != REDIS_OK) {
        hlogi("%p connect error: %s\n", c->data, c->errstr);
        if (self) self->onDisconnect(status);
    }
    else {
        hlogi("%p Connected...\n", c->data);
        if (self) self->onConnect();
    }
}

void RedisClient::disconnectCallback(const redisAsyncContext *c, int status) {
    RedisClient *self = (RedisClient *)c->data;
    if (self) self->onDisconnect(status);
    if (status != REDIS_OK) {
        hlogi("%p Error: %s\n", c->data, c->errstr);
    }
    else {
        hlogi("%p Disconnected...\n", c->data);
    }
}

void RedisClient::onDisconnect(int code) {
    if (event_) event_->onDisconnect(code);
    // reconnect
    if (code) {
    }
}

void RedisClient::onConnect() {
    for (auto it : subsMap) {
        redisAsyncCommand(ctx_, &subsCallback, nullptr, "subscribe %s", it.first.c_str());
    }
    if (event_) event_->onConnect();
}

bool RedisClient::open(const char *host, int port) {
    if (port <= 0) port = 6379;
    ctx_ = redisAsyncConnect(host, port);
    if (ctx_->err) {
        /* Let *c leak for now... */
        printf("Error: %s\n", ctx_->errstr);
        return false;
    }

    redisLibhvAttach(ctx_, loop_);
    ctx_->data = this;
    timeval tv = {0, 500000};
    redisAsyncSetTimeout(ctx_, tv);
    redisAsyncSetConnectCallback(ctx_, connectCallback);
    redisAsyncSetDisconnectCallback(ctx_, disconnectCallback);
    return true;
}

bool RedisClient::close() {
    if (subs_timer_) {
        htimer_del(subs_timer_);
        subs_timer_ = nullptr;
    }
    if (auto c = ctx_) {
        c->data = nullptr;
        redisAsyncDisconnect(c);
        redisAsyncFree(c);
        c = nullptr;
        return true;
    }
    return false;
}

void RedisClient::auth(const char *key, const char *value, reply_cb_t cb) {
    if (!ctx_) return;
    redisAsyncCommand(ctx_, &reply::ReplyCB, reply::CbData(cb), "auth %s %s", key, value);
}

void RedisClient::get(const char *key, reply_cb_t cb) {
    if (!ctx_) return;
    redisAsyncCommand(ctx_, &reply::ReplyCB, reply::CbData(cb), "get %s", key);
}

void RedisClient::set(const char *key, const std::string& value, reply_cb_t cb) {
    if (!ctx_) return;
    redisAsyncCommand(ctx_, &reply::ReplyCB, reply::CbData(cb), "set %s %b", key, value.c_str(), value.size());
}


void RedisClient::publish(const std::string &channel, const std::string &msg) {
    if (ctx_) {
        hlogi("publish %s> %s", channel.c_str(), msg.c_str());
        redisAsyncCommand(ctx_, nullptr, nullptr, "publish %s %s", channel.c_str(), msg.c_str());
    }
}

void RedisClient::subsCallback(redisAsyncContext *ac, void *r, void *privdata) {
    redisReply *reply = (redisReply *)r;
    if (reply && reply->type == REDIS_REPLY_ARRAY && reply->elements >= 3) {
        if (reply->element[2]->type == REDIS_REPLY_INTEGER) {
            hlogi("redis %s %s %lld", reply->element[0]->str, reply->element[1]->str, reply->element[2]->integer);
            return;
        }

        std::string channel, msg;
        if (0 == strcasecmp(reply->element[0]->str, "message")) {
            channel = reply->element[1]->str;
            msg.assign(reply->element[2]->str, reply->element[2]->len);
        }
        else if (reply->elements == 4 && !strcasecmp(reply->element[0]->str, "pmessage")) {
            channel = reply->element[2]->str;
            msg.assign(reply->element[3]->str, reply->element[3]->len);
        }

        // subsCount++;
        hlogi("%s %s< %s", reply->element[0]->str, channel.c_str(), msg.c_str());
        if (auto redis = (RedisClient*)ac->data) {
            // MS_DEBUG("%s< %s", channel.c_str(), msg.c_str());
            try {
                if (redis->event_)
                    redis->event_->onMessage(channel, msg);
                MsgFunc cb = redis->getSubsCB(channel);
                if(cb)
                    cb(channel, msg);
            }
            catch (std::exception &e) {
                hlogi(">>> subs %s callback %s exception %s", channel.c_str(), msg.c_str(), e.what());
            }
        }
    }
    else {
        hlogi("subsCallback error %p", reply);
    }
}

void RedisClient::subscribe(const std::string &channel, MsgFunc func) {
    hlogi("subscribe %s", channel.c_str());
    subsMap[channel] = func;
    if (ctx_) {
        redisAsyncCommand(ctx_, &subsCallback, nullptr, "subscribe %s", channel.c_str());
    }    
    if (!subs_timer_) {
        float timeout = 60;
        subs_timer_ = htimer_add(
            loop_, [](htimer_t *timer) { 
                auto client = (RedisClient*)hevent_userdata(timer);
                if (client && client->ctx_) redisAsyncCommand(client->ctx_, nullptr, nullptr, "ping");
            }, 
            timeout * 1000);
        hevent_set_userdata(subs_timer_, this);
        hlogi("start subsChecker with interval %fs", timeout);
    }
}

void RedisClient::unsubscribe(const std::string &channel) {
    hlogi("unsubscribe %s", channel.c_str());
    subsMap.erase(channel);
    if (ctx_) {
        redisAsyncCommand(ctx_, nullptr, nullptr, "unsubscribe %s", channel.c_str());
    }
}