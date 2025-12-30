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
#include "topic_tree.h"
#include "hendian.h"
#include "hstring.h"
#include <list>
#include <set>
using namespace hv;

#define TEST_TLS        0
struct MqttSession {
    using Ptr = std::shared_ptr<MqttSession>;
    SocketChannelPtr tcp;
    WebSocketChannelPtr ws;
    HVLBuf recv_buf; // 用于ws的接收分包

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
        recv_buf.clear();
        return ret;
    }
};

class MqttServer : public TcpServer {
    TopicTree topic_tree_;
    std::shared_ptr<WebSocketServer> wsServer;
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
    const TopicTree& topic_tree() const {
        return topic_tree_;
    }
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
            auto set = channel->getContextPtr<MqttSession>();
            onMqttMessage(set, &head, p, p+headlen);
        };

        onConnection = [this](const SocketChannelPtr& channel) {
            std::string peeraddr = channel->peeraddr();
            if (channel->isConnected()) {
                hlogi("%s connected! connfd=%d id=%d\n", peeraddr.c_str(), channel->fd(), channel->id());
                channel->newContextPtr<MqttSession>()->tcp = channel;
            }
            else {
                hlogi("%s disconnected! connfd=%d id=%d\n", peeraddr.c_str(), channel->fd(), channel->id());
                auto set = channel->getContextPtr<MqttSession>();
                closeSession(set);
            }
        };
    }

    void closeSession(MqttSession::Ptr set) {
        if (set) {
            // topic_.remove_session(set);
        }
    }

    int wsListen(int port, int ssl_port) {
        static WebSocketService ws;
        ws.onopen = [](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
            printf("onopen: GET %s\n", req->url.c_str());
            channel->newContextPtr<MqttSession>()->ws = channel;
        };
        ws.onmessage = [this](const WebSocketChannelPtr& channel, const std::string& msg) {
            auto set = channel->getContextPtr<MqttSession>();
            set->recv_buf.append((void*)msg.c_str(), msg.size());
            int size = set->recv_buf.size();
            unsigned char* p = (unsigned char*)set->recv_buf.data();
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
            set->recv_buf.remove(p - (unsigned char*)set->recv_buf.data());
        };
        ws.onclose = [this](const WebSocketChannelPtr& channel) {
            printf("onclose\n");
            auto ctx = channel->getContextPtr<MqttSession>();
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
            std::string topic(message.topic, message.topic_len);
            // 保留retain消息
            if (start[0] & 0x01) {
                std::vector<uint8_t> retain;
                if (message.payload_len > 0) {
                    retain.resize(end - start);
                    memcpy(retain.data(), start, end - start);
                }
                topic_tree_.set_retained_message(topic, retain, message.qos, head->dup);
                // 清除retain标记
                start[0] &= ~0x01;
            }
            

            auto matchs = topic_tree_.get_subscribers(topic);
            for (auto it : matchs) {
                if (auto cli = it.session.lock()) {
                    cli->write(start, end - start);
                }
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
                if (onSubscribe) onSubscribe(channel, &message);

                std::string topic(message.topic, message.topic_len);
                topic_tree_.subscribe(topic, channel, message.qos);
                for (auto it : topic_tree_.get_retained_messages(topic)) {
                    // 发送保留消息
                    auto& retained_msg = it.second;
                    std::vector<uint8_t> buff(10 + it.first.size() + retained_msg.payload.size());
                    mqtt_head_t retain_head;
                    memset(&retain_head, 0, sizeof(retain_head));
                    retain_head.type = MQTT_TYPE_PUBLISH;
                    retain_head.retain = 1;
                    retain_head.qos = std::min(message.qos, retained_msg.qos);
                    retain_head.length = 2 + it.first.size() + retained_msg.payload.size();
                    int len = mqtt_head_pack(&retain_head, &buff[0]);
                    unsigned char* p = &buff[len];
                    // topic
                    len = it.first.size();
                    if (len) {
                        PUSH16(p, len);
                        memcpy(p, it.first.data(), len);
                        p += len;
                    }
                    // payload
                    len = retained_msg.payload.size();
                    if (len) {
                        memcpy(p, retained_msg.payload.data(), len);
                        p += len;
                    }
                    channel->write(buff.data(), p - buff.data());
                }
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
                if (onUnsubscribe) onUnsubscribe(channel, &message);

                std::string topic(message.topic, message.topic_len);
                topic_tree_.unsubscribe(topic, channel);
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
        if (str == "dump" || str == "d") {
            srv.topic_tree().dump_tree([](const std::string& line){
                printf("%s\n", line.c_str());
            });
        } else if (str == "stat" || str == "s") {
            auto stat = srv.topic_tree().get_statistics();
            printf("Statistics: %zu %zu %zu %zu\n", stat.total_nodes, stat.total_subscribers, stat.total_retained_messages, stat.max_depth);
        } else if (str == "exit" || str == "quit" || str == "q") {
            srv.closesocket();
            break;
        } else {
            srv.broadcast(str.data(), str.size());
        }
    }

    return 0;
}
