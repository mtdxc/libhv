/*
 * TcpServer_test.cpp
 *
 * @build   make evpp
 * @server  bin/TcpServer_test 1234
 * @client  bin/TcpClient_test 1234
 *
 */

#include <iostream>
#include <algorithm>
#include "mqtt_protocol.h"
#include "mqtt_client.h"
#include "TcpServer.h"
#include "http/server/WebSocketServer.h"
#include "hendian.h"
#include "hstring.h"
#include <list>
#include <set>
using namespace hv;

#define TEST_TLS        0
struct MqttSession {
    using Ptr = MqttSession*;
    std::set<std::string> subs;
    SocketChannelPtr tcp;
    WebSocketChannelPtr ws;
    HVLBuf recv; // 用于ws的接收分包

    int write(const void* buff, int size) {
        int ret = 0;
        if (tcp)
            ret = tcp->write(buff, size);
        else if (ws)
            ret = ws->send((const char*)buff, size);
        return ret;
    }

    bool close() {
        int ret = -1;
        if (tcp) {
            ret = tcp->close(true);
            tcp = nullptr;
        } 
        if (ws) {
            ret = ws->close();
            ws = nullptr;
        }
        recv.clear();
        return ret;
    }
};

class Subscribe {
    hv::StringList _tokens; // 订阅路径
    int _type;              // 0 完整匹配, 1 + 局部匹配, 2 # 尾部匹配
public:
    int type() const {return _type;}
    std::set<MqttSession::Ptr> socks;
    std::string path;

    using Ptr = std::shared_ptr<Subscribe>;
    Subscribe(const std::string& n) : path(n) {
        _type = 0;
        _tokens = hv::split(n, '/');
        for (int i = 0; i < _tokens.size(); i++) {
            auto& t = _tokens[i];
            if (t == "#" && i + 1 == _tokens.size()) { // #通配符只能出现在尾部
                _type = 2;
                //#可匹配0路径
                _tokens.pop_back();
                path.pop_back();
                if (path.length() && path.back() == '/')
                    path.pop_back();
            }
            else if (t == "+")
                _type = 1;
        }
    }

    bool match(const std::string& topic) {
        if (_type == 0)
            return path == topic;
        else if (_type == 2) {
            if (path.empty()) return true;
            bool ret = hv::startswith(topic, path);
            if (ret) {
                ret = topic.length() == path.length() || topic[path.length()] == '/';
            }
            return ret;
        }
        hv::StringList sl = hv::split(topic, '/');
        return match(sl);
    }
    bool match(const hv::StringList& lst) {
        if (_type == 2) { //#匹配
            if (lst.size() < _tokens.size()) return false;
        }
        else if (lst.size() != _tokens.size()) {
            return false;
        }

        int total = min(_tokens.size(), lst.size());
        for (int i = 0; i < total; i++) {
            auto t = _tokens.at(i);
            if (t == "+")
                continue;
            else if (t != lst.at(i))
                return false;
        }
        return true;
    }
    
    void publish(uint8_t* buff, int size) {
        for (auto it : socks) {
            it->write(buff, size);
        }
    }
};

class MqttServer : public TcpServer {
    using SubSet = std::set<std::string>;
    std::recursive_mutex lock;
    // 所有订阅，包含所有type的
    std::map<std::string, Subscribe::Ptr> subsMap;
    // type > 0 的订阅Map，需要模糊匹配
    std::map<std::string, Subscribe::Ptr> typeSubs;
    // 保留retain消息
    std::map<std::string, hv::Buffer> retainMap;
    std::shared_ptr<WebSocketServer> wsServer;
    // 删除订阅
    bool delSubscribe(const std::string& name, MqttSession::Ptr chann) { 
        std::unique_lock<std::recursive_mutex> l(lock);
        auto it = subsMap.find(name);
        if (it != subsMap.end()) {
            auto subs = it->second;
            subs->socks.erase(chann);
            if (subs->socks.empty()) {
                subsMap.erase(it);
                if (subs->type())
                    typeSubs.erase(subs->path);
            }
            return true;
        }
        return false;
    }
    bool addSubscribe(const std::string& name, MqttSession::Ptr chann) {
        std::unique_lock<std::recursive_mutex> l(lock);
        Subscribe::Ptr subs;
        auto it = subsMap.find(name);
        if (it != subsMap.end()) {
            subs = it->second;
        }
        else {
            subs = std::make_shared<Subscribe>(name);
            subsMap[name] = subs;
            if (subs->type())
                typeSubs[name] = subs;
        }
        subs->socks.insert(chann);
        // 处理retain消息的订阅
        if (subs->type()) {
            for (auto rit : retainMap) {
                if (subs->match(rit.first)) {
                    chann->write(rit.second.data(), rit.second.size());
                }
            }
        }
        else {
            auto rit = retainMap.find(name);
            if (rit!=retainMap.end())
                chann->write(rit->second.data(), rit->second.size());
        }
        return true;
    }

    int send_head(MqttSession::Ptr channel, int type, int length) {
        mqtt_head_t head;
        memset(&head, 0, sizeof(head));
        head.type = type;
        head.length = length;
        unsigned char headbuf[8] = {0};
        int headlen = mqtt_head_pack(&head, headbuf);
        return channel->write(headbuf, headlen);
    }

    int send_head_with_mid(MqttSession::Ptr channel, int type, unsigned short mid) {
        mqtt_head_t head;
        memset(&head, 0, sizeof(head));
        head.type = type;
        if (head.type == MQTT_TYPE_PUBREL) {
            head.qos = 1;
        }
        head.length = 2;
        unsigned char headbuf[8] = {0};
        unsigned char* p = headbuf;
        int headlen = mqtt_head_pack(&head, p);
        p += headlen;
        PUSH16(p, mid);
        return channel->write(headbuf, headlen + 2);
    }
public:
    typedef std::function<void(MqttSession::Ptr, mqtt_message_t*)> MqttMessageCallback;
    MqttMessageCallback onPublish;
    MqttMessageCallback onSubscribe;
    MqttMessageCallback onUnsubscribe;
    std::function<mqtt_connack_e(MqttSession::Ptr, mqtt_conn_t*)> onAuth;

    MqttServer(EventLoopPtr loop = NULL) : TcpServer(loop) {
        // 总是接受连接请求
        onAuth = [](MqttSession::Ptr, mqtt_conn_t*){
            return MQTT_CONNACK_ACCEPTED;
        };
        static unpack_setting_t mqtt_unpack_setting;
        mqtt_unpack_setting.mode = UNPACK_BY_LENGTH_FIELD;
        mqtt_unpack_setting.package_max_length = DEFAULT_MQTT_PACKAGE_MAX_LENGTH;
        mqtt_unpack_setting.body_offset = 2;
        mqtt_unpack_setting.length_field_offset = 1;
        mqtt_unpack_setting.length_field_bytes = 1;
        mqtt_unpack_setting.length_field_coding = ENCODE_BY_VARINT;
        setUnpack(&mqtt_unpack_setting);
        onMessage = [this](const SocketChannelPtr& channel, Buffer* buf) {
            mqtt_head_t head;
            memset(&head, 0, sizeof(mqtt_head_t));
            unsigned char* p = (unsigned char*)buf->data();
            int headlen = mqtt_head_unpack(&head, p, buf->size());
            if (headlen <= 0 || buf->size() < headlen + head.length)
                return;
            MqttSession* set = channel->getContext<MqttSession>();
            onMqttMessage(set, &head, p, p+headlen);
        };
        onConnection = [this](const SocketChannelPtr& channel) {
            std::string peeraddr = channel->peeraddr();
            if (channel->isConnected()) {
                printf("%s connected! connfd=%d id=%d tid=%ld\n", peeraddr.c_str(), channel->fd(), channel->id(), currentThreadEventLoop->tid());
                channel->newContext<MqttSession>()->tcp = channel;
            }
            else {
                printf("%s disconnected! connfd=%d id=%d tid=%ld\n", peeraddr.c_str(), channel->fd(), channel->id(), currentThreadEventLoop->tid());
                MqttSession* set = channel->getContext<MqttSession>();
                closeSession(set);
            }
        };
    }
    void closeSession(MqttSession::Ptr set) {
        if (set) {
            for (auto it : set->subs) {
                delSubscribe(it, set);
            }
            delete set;
        }
    }

    int wsListen(int port, int ssl_port) {
        static WebSocketService ws;
        ws.onopen = [](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
            printf("onopen: GET %s\n", req->url.c_str());
            channel->newContext<MqttSession>()->ws = channel;
        };
        ws.onmessage = [this](const WebSocketChannelPtr& channel, const std::string& msg) {
            MqttSession* set = channel->getContext<MqttSession>();
            set->recv.append((void*)msg.c_str(), msg.size());
            int size = set->recv.size();
            unsigned char* p = (unsigned char*)set->recv.data();
            while (size > 1) {
                mqtt_head_t head;
                memset(&head, 0, sizeof(mqtt_head_t));
                int headlen = mqtt_head_unpack(&head, p, size);
                if (headlen <= 0 || size < headlen + head.length) 
                    break;
                onMqttMessage(set, &head, p, p + headlen);
                p += head.length + headlen;
                size -= head.length + headlen;
            }
            set->recv.remove(p - (unsigned char*)set->recv.data());
        };
        ws.onclose = [this](const WebSocketChannelPtr& channel) {
            printf("onclose\n");
            MqttSession* ctx = channel->getContext<MqttSession>();
            closeSession(ctx);
        };
        wsServer = std::make_shared<WebSocketServer>();
        wsServer->worker_processes = 0;
        wsServer->worker_threads = 1;
        if (ssl_port) {
            wsServer->https_port = ssl_port;
            hssl_ctx_opt_t param;
            memset(&param, 0, sizeof(param));
            param.crt_file = "cert/server.crt";
            param.key_file = "cert/server.key";
            param.endpoint = HSSL_SERVER;
            if (wsServer->newSslCtx(&param) != 0) {
                fprintf(stderr, "new SSL_CTX failed!\n");
                return -20;
            }
        }
        wsServer->port = port;
        wsServer->registerWebSocketService(&ws);
        return wsServer->start();
    }

    void onMqttMessage(MqttSession::Ptr channel, mqtt_head_t* head, uint8_t* start, uint8_t* p) {
        int mid = 0;
        switch (head->type) {
        case MQTT_TYPE_CONNECT:
        {//MQTT_TYPE_CONNACK       = 2,
            // 2 + protocol_name + 1 protocol_version + 1 conn_flags + 2 keepalive + 2 + [client_id] +
            // [2 + will_topic + 2 + will_payload] + * [2 + username] + [2 + password]
            uint8_t* end = p + head->length;
            mqtt_conn_t conn;
            memset(&conn, 0, sizeof conn);
            POPSTR(p, conn.protocol);
            POP8(p, conn.version);
            POP8(p, conn.conn_flag);
            POP16(p, conn.keepalive);
            POPSTR(p, conn.client_id);
            if (p > end) return;
            if (conn.conn_flag & MQTT_CONN_HAS_WILL) {
                POPSTR(p, conn.will_topic);
                if (p > end) return;
                POPSTR(p, conn.will_payload);
                if (p > end) return;
            }
            if (conn.conn_flag & MQTT_CONN_HAS_USERNAME) {
                POPSTR(p, conn.user_name);
                if (p > end) return;
            }
            if (conn.conn_flag & MQTT_CONN_HAS_PASSWORD) {
                POPSTR(p, conn.password);
                if (p > end) return;
            }
            send_head(channel, MQTT_TYPE_CONNACK, 2);
            unsigned char body[2];
            body[0] = 0; //conn_flag
            body[1] = onAuth(channel, &conn);
            channel->write(body, 2);
        } break;
        case MQTT_TYPE_PUBLISH:
        {
            if (head->length < 2) {
                hloge("MQTT PUBLISH malformed!");
                channel->close();
                return;
            }
            uint8_t* end = p + head->length;
            mqtt_message_t message;
            memset(&message, 0, sizeof(mqtt_message_t));
            message.retain = head->retain;
            POP16(p, message.topic_len);
            // NOTE: Not deep copy
            message.topic = (char*)p;
            p += message.topic_len;
            if (head->qos > 0) {
                if (end - p < 2) {
                    hloge("MQTT PUBLISH malformed!");
                    channel->close();
                    return;
                }
                POP16(p, mid);
            }
            message.payload_len = end - p;
            if (message.payload_len > 0) {
                // NOTE: Not deep copy
                message.payload = (char*)p;
            }
            message.qos = head->qos;
            if (message.qos == 0) {
                // Do nothing
            } else if (message.qos == 1) {
                send_head_with_mid(channel, MQTT_TYPE_PUBACK, mid);
            } else if (message.qos == 2) {
                send_head_with_mid(channel, MQTT_TYPE_PUBREC, mid);
            }
            std::set<MqttSession::Ptr> clients;
            {
                std::string topic(message.topic, message.topic_len);
                std::unique_lock<std::recursive_mutex> l(lock);
                // 保留retain消息
                if (start[0] & 0x01) {
                    if (message.payload_len > 0) {
                        retainMap[topic].copy(start, end - start);
                    } else {
                        retainMap.erase(topic);
                        return ;
                    }
                    // 清除retain标记
                    start[0] &= ~0x01;
                }

                // publish type == 0
                auto it = subsMap.find(topic);
                if (it != subsMap.end()) {
                    clients = it->second->socks;
                    //it->second->publish(start, end - start);
                }
                else { // 通配符查找
                    for (auto it : typeSubs) {
                        auto subs = it.second;
                        if (subs->match(topic)) {
                            clients.insert(subs->socks.begin(), subs->socks.end());
                            //subs->publish(start, end - start);
                        }
                    }
                }
            }
            // 转发消息
            for (auto chann : clients) {
                chann->write(start, end - start);
            }

            if (onPublish)
                onPublish(channel, &message);
        } break;
        case MQTT_TYPE_PUBACK:
        case MQTT_TYPE_PUBREC:
        case MQTT_TYPE_PUBREL:
        case MQTT_TYPE_PUBCOMP: {
            if (head->length < 2) {
                hloge("MQTT PUBACK malformed!");
                channel->close();
                return;
            }
            POP16(p, mid);
            if (head->type == MQTT_TYPE_PUBREC) {
                send_head_with_mid(channel, MQTT_TYPE_PUBREL, mid);
            }
            else if (head->type == MQTT_TYPE_PUBREL) {
                send_head_with_mid(channel, MQTT_TYPE_PUBCOMP, mid);
            }
        } break;
        case MQTT_TYPE_SUBSCRIBE:
            if (head->length>1) {
                POP16(p, mid);
                send_head_with_mid(channel, MQTT_TYPE_SUBACK, mid);

                mqtt_message_t message;
                memset(&message, 0, sizeof(mqtt_message_t));
                POP16(p, message.topic_len);
                // NOTE: Not deep copy
                message.topic = (char*)p;
                p += message.topic_len;
                POP8(p, message.qos);

                std::string topic(message.topic, message.topic_len);
                addSubscribe(topic, channel);
				channel->subs.insert(topic);
                if (onSubscribe) onSubscribe(channel, &message);
            }
            break;
        case MQTT_TYPE_UNSUBSCRIBE: 
            if (head->length>1) {
                POP16(p, mid);
                send_head_with_mid(channel, MQTT_TYPE_UNSUBACK, mid);

                mqtt_message_t message;
                memset(&message, 0, sizeof(mqtt_message_t));
                POP16(p, message.topic_len);
                // NOTE: Not deep copy
                message.topic = (char*)p;
                p += message.topic_len;

                std::string topic(message.topic, message.topic_len);
                delSubscribe(topic, channel);
				channel->subs.erase(topic);
                if (onUnsubscribe) onUnsubscribe(channel, &message);
            }        
            break;
        case MQTT_TYPE_PINGREQ: 
            send_head(channel, MQTT_TYPE_PINGRESP, 0);
            break;
        case MQTT_TYPE_DISCONNECT: 
            channel->close();
            break;
        }
    }
};

int main(int argc, char* argv[]) {
    int port = 1883;
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    hlog_set_level(LOG_LEVEL_DEBUG);

    MqttServer srv;
    int listenfd = srv.createsocket(port);
    if (listenfd < 0) {
        return -20;
    }
    srv.wsListen(8883, 0);
    printf("server listen on port %d, listenfd=%d ...\n", port, listenfd);
    srv.onAuth = [](MqttSession::Ptr, mqtt_conn_t* msg) {
        printf("onAuth %.*s %.*s %.*s\n", 
            msg->client_id.len, msg->client_id.data,
            msg->user_name.len, msg->user_name.data, 
            msg->password.len, msg->password.data);
        return MQTT_CONNACK_ACCEPTED;
    };
    srv.onPublish = [](MqttSession::Ptr, mqtt_message_t* msg) {
        printf("topic %.*s %d> %.*s\n", msg->topic_len, msg->topic, msg->qos, msg->payload_len, msg->payload);
    };
    srv.onSubscribe = [](MqttSession::Ptr, mqtt_message_t* msg) {
        printf("onSubscribe %.*s qos %d\n", msg->topic_len, msg->topic, msg->qos);
    };
    srv.onUnsubscribe = [](MqttSession::Ptr, mqtt_message_t* msg) {
        printf("onUnsubscribe %.*s\n", msg->topic_len, msg->topic);
    };
    srv.setThreadNum(4);
    srv.setLoadBalance(LB_LeastConnections);

#if TEST_TLS
    hssl_ctx_opt_t ssl_opt;
    memset(&ssl_opt, 0, sizeof(hssl_ctx_opt_t));
    ssl_opt.crt_file = "cert/server.crt";
    ssl_opt.key_file = "cert/server.key";
    ssl_opt.verify_peer = 0;
    srv.withTLS(&ssl_opt);
#endif

    srv.start();

    std::string str;
    while (std::getline(std::cin, str)) {
        if (str == "close") {
            srv.closesocket();
        } else if (str == "start") {
            srv.start();
        } else if (str == "stop") {
            srv.stop();
            break;
        } else {
            srv.broadcast(str.data(), str.size());
        }
    }

    return 0;
}
