#include "RedisClient.h"
#include <async.h>
#include <hiredis.h>
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
        printf("%p connect error: %s\n", c->data, c->errstr);
        if (self && self->event_) self->event_->onDisconnect(status);
    }
    else {
        printf("%p Connected...\n", c->data);
        if (self && self->event_) self->event_->onConnect();
    }
}

void RedisClient::disconnectCallback(const redisAsyncContext *c, int status) {
    RedisClient *self = (RedisClient *)c->data;
    if (self && self->event_) self->event_->onDisconnect(status);
    if (status != REDIS_OK) {
        printf("Error: %s\n", c->errstr);
        return;
    }
    printf("Disconnected...\n");
}

bool RedisClient::open(const char* host, int port) {
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
