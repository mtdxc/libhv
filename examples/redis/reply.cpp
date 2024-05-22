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