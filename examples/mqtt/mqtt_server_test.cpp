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
#include "hendian.h"
#include <list>
#include <set>
using namespace hv;

#define TEST_TLS        0

class MqttServer : public TcpServer {
    using SubSet = std::set<std::string>;
    using Topic = std::list<SocketChannelPtr>;
    using TopicPtr = std::shared_ptr<Topic>;
    // 主题列表用subs进行管理
    std::recursive_mutex topicLock;
    std::map<std::string, TopicPtr> topicMap;
    TopicPtr getTopic(const std::string& name, bool create) { 
        TopicPtr ret;
        auto it = topicMap.find(name);
        if (it != topicMap.end()) 
            ret =  it->second;
        else if (create) {
            ret = std::make_shared<Topic>();
            topicMap[name] = ret;
        }
        return ret;
    }
    // 删除某个主题的订阅
    void delSubscribe(const std::string& name, SocketChannelPtr chann) { 
        std::unique_lock<std::recursive_mutex> l(topicLock);
        auto topic = getTopic(name, false);
        if (topic) {
            auto it = std::find(topic->begin(), topic->end(), chann);
            if (it != topic->end()) topic->erase(it);
            if (topic->empty())
                topicMap.erase(name);
        }
    }
    void addSubscribe(const std::string& name, SocketChannelPtr chann) {
        std::unique_lock<std::recursive_mutex> l(topicLock);
        auto topic = getTopic(name, true);
        auto it = std::find(topic->begin(), topic->end(), chann);
        if (it == topic->end()) topic->push_back(chann);
    }
    void publishMessage(const std::string& name, uint8_t* buff, int size) {
        TopicPtr t;
        { 
            std::unique_lock<std::recursive_mutex> l(topicLock);
            t = getTopic(name, false);
        }
        if (t) {
            for (auto it = t->begin(); it != t->end(); it++) {
                (**it).write(buff, size);
            }
        }
    }
    int send_head(const SocketChannelPtr& channel, int type, int length) {
        mqtt_head_t head;
        memset(&head, 0, sizeof(head));
        head.type = type;
        head.length = length;
        unsigned char headbuf[8] = {0};
        int headlen = mqtt_head_pack(&head, headbuf);
        return channel->write(headbuf, headlen);
    }

    int send_head_with_mid(const SocketChannelPtr& channel, int type, unsigned short mid) {
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
    typedef std::function<void(const SocketChannelPtr&, mqtt_message_t*)> MqttMessageCallback;
    MqttMessageCallback onPublish;
    MqttMessageCallback onSubscribe;
    MqttMessageCallback onUnsubscribe;
    std::function<mqtt_connack_e(const SocketChannelPtr&, mqtt_conn_t*)> onAuth;

    MqttServer(EventLoopPtr loop = NULL) : TcpServer(loop) {
        // 总是接受连接请求
        onAuth = [](const SocketChannelPtr&, mqtt_conn_t*){
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
        onMessage = [this](const TSocketChannelPtr& channel, Buffer* buf) {
            mqtt_head_t head;
            memset(&head, 0, sizeof(mqtt_head_t));
            unsigned char* p = (unsigned char*)buf->data();
            int headlen = mqtt_head_unpack(&head, p, buf->size());
            if (headlen <= 0) return;
            onMqttMessage(channel, &head, p, p+headlen);
        };
        onConnection = [this](const SocketChannelPtr& channel) {
            std::string peeraddr = channel->peeraddr();
            if (channel->isConnected()) {
                printf("%s connected! connfd=%d id=%d tid=%ld\n", peeraddr.c_str(), channel->fd(), channel->id(), currentThreadEventLoop->tid());
                channel->newContext<SubSet>();
            }
            else {
                printf("%s disconnected! connfd=%d id=%d tid=%ld\n", peeraddr.c_str(), channel->fd(), channel->id(), currentThreadEventLoop->tid());
                SubSet* set = channel->getContext<SubSet>();
                if (set) {
                    for (auto it : *set) {
                        delSubscribe(it, channel);
                    }
                    delete set;
                }
            }
        };
    }

    void onMqttMessage(const TSocketChannelPtr& channel, mqtt_head_t* head, uint8_t* start, uint8_t* p) {
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
            std::string topic(message.topic, message.topic_len);
            publishMessage(topic, start, end - start);

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
                channel->getContext<SubSet>()->insert(topic);
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
                channel->getContext<SubSet>()->erase(topic);
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
    printf("server listen on port %d, listenfd=%d ...\n", port, listenfd);
    srv.onAuth = [](const SocketChannelPtr&, mqtt_conn_t* msg) {
        printf("onAuth %.*s %.*s %.*s\n", 
            msg->client_id.len, msg->client_id.data,
            msg->user_name.len, msg->user_name.data, 
            msg->password.len, msg->password.data);
        return MQTT_CONNACK_ACCEPTED;
    };
    srv.onPublish = [](const SocketChannelPtr&, mqtt_message_t* msg) {
        printf("topic %.*s> %.*s\n", msg->topic_len, msg->topic, msg->payload_len, msg->payload);
    };
    srv.onSubscribe = [](const SocketChannelPtr&, mqtt_message_t* msg) {
        printf("onSubscribe %.*s qos %d\n", msg->topic_len, msg->topic, msg->qos);
    };
    srv.onUnsubscribe = [](const SocketChannelPtr&, mqtt_message_t* msg) {
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