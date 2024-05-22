#include "reply.h"
#include "hiredis.h"

void reply::ReplyCB(redisAsyncContext *c, void *r, void *data) {
    if (!data) return;
    reply_cb_t *cb = (reply_cb_t *)data;
    if (cb) {
        if (auto rp = (redisReply *)r) {
            reply rpy = reply(rp);
            (*cb)(rpy);
        }
        delete cb;
    }
}

reply::reply(redisReply *c_reply) : _type(type_t::ERROR), _integer(0) {
    _type = static_cast<type_t>(c_reply->type);
    switch (_type) {
    case type_t::ERROR:
    case type_t::STRING:
    case type_t::STATUS:
    case type_t::BIGNUM: // str存放的是字符串数据
    case type_t::VERB: // 类型在vtype, 内容在str
        _str = std::string(c_reply->str, c_reply->len);
        break;
	case type_t::INTEGER:
        _integer = c_reply->integer;
        break;
    case type_t::DOUBLE: 
        _double = c_reply->dval; 
        break;
    case type_t::MAP: 
        for (size_t i = 0; i < c_reply->elements; i+= 2) {
            _maps.emplace(std::make_pair(c_reply->element[i]->str, reply(c_reply->element[i + 1])));
        }
        break;
    case type_t::SET:
    case type_t::ARRAY:
        for (size_t i=0; i < c_reply->elements; ++i) {
            _elements.push_back(reply(c_reply->element[i]));
        }
        break;
    default:
        break;
    }
}

reply::reply():_type(type_t::ERROR), _integer(0) {
}

reply2::~reply2() {
    free();
}

void reply2::free() {
    if (owner_ && reply_) {
        freeReplyObject(reply_);
        reply_ = nullptr;
        owner_ = false;
    }
}

void reply2::assign(redisReply *r, bool o) {
    if (reply_ == r) {
        owner_ = o;
        return;
    }
    free();
    reply_ = r;
    owner_ = o;
}


bool reply2::ok() const {
    return type() != REDIS_REPLY_NIL && type() != REDIS_REPLY_ERROR;
}

bool reply2::nil() const {
    return type() == REDIS_REPLY_NIL;
}

int reply2::type() const {
    return reply_ ? reply_->type : REDIS_REPLY_NIL;
}

long long reply2::intVal() const {
    if (reply_->type == REDIS_REPLY_INTEGER) return reply_->integer;
    return 0;
}

double reply2::doubleVal() const {
    if (reply_->type == REDIS_REPLY_DOUBLE) return reply_->dval;
    return 0;
}

std::string reply2::str() const {
    std::string ret;
    if (isEmpty()) return ret;
    if (reply_->type == REDIS_REPLY_VERB || reply_->type == REDIS_REPLY_ERROR || reply_->type == REDIS_REPLY_STRING || reply_->type == REDIS_REPLY_STATUS ||
        reply_->type == REDIS_REPLY_BIGNUM)
        ret.assign(reply_->str, reply_->len);
    return ret;
}

int reply2::size() const {
    return reply_ ? reply_->elements : 0;
}

reply2 reply2::get(int idx) const {
    reply2 ret;
    if (reply_ == nullptr || idx < 0 || idx >= reply_->elements) return ret;
    if (reply_->type == REDIS_REPLY_SET || reply_->type == REDIS_REPLY_MAP || reply_->type == REDIS_REPLY_ARRAY) 
        ret.assign(reply_->element[idx], false);
    return ret;
}

reply2 reply2::get(const char *name) const {
    reply2 ret;
    if (reply_ == nullptr || reply_->type != REDIS_REPLY_MAP || reply_->elements % 2) return ret;
    for (size_t i = 0; i < reply_->elements; i += 2) {
        if (!strncmp(reply_->element[i]->str, name, reply_->element[i]->len)) 
            ret.assign(reply_->element[i + 1], false);
    }
    return ret;
}

bool reply2::has(const char *name, int len) const {
    if (len <= 0) len = strlen(name);
    if (reply_ == nullptr || reply_->type != REDIS_REPLY_SET || reply_->type != REDIS_REPLY_ARRAY) return false;
    for (size_t i = 0; i < reply_->elements; i++) {
        if (reply_->element[i]->len == len && !strncmp(reply_->element[i]->str, name, len)) return true;
    }
    return false;
}
