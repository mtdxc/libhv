#include "topic_tree.h"
#include "mqtt_server.h"
#include "hendian.h"
#include "hlog.h"
#include "hmath.h"
using namespace hv;

int MqttSession::write(const void* buff, int size) {
    int ret = 0;
    if (tcp)
        ret = tcp->write(buff, size);
    else if (ws)
        ret = ws->send((const char*)buff, size);
    return ret;
}

bool MqttSession::close() {
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

uint8_t* MqttSession::skipProp(uint8_t* p) const {
    if (version != MQTT_PROTOCOL_V5) return p;
    int prop_bytes = 0;
    int prop_len = (int)varint_decode(p, &prop_bytes);
    if (prop_bytes <= 0) return nullptr;
    return p + prop_bytes + prop_len;
}

int MqttSession::send(int type, void* data, int length) {
    mqtt_head_t head;
    memset(&head, 0, sizeof(head));
    head.type = type;
    head.length = length;
    unsigned char headbuf[8] = {0};
    int headlen = mqtt_head_pack(&head, headbuf);
    int ret = write(headbuf, headlen);
    if (length) {
        ret += write(data, length);
    }
    return ret;
}

int MqttSession::publish(mqtt_message_t* msg) {
    mqtt_head_t head;
    memset(&head, 0, sizeof(head));
    head.qos = msg->qos;
    head.retain = msg->retain;
    head.type = MQTT_TYPE_PUBLISH;
    head.length = 2 + msg->topic_len + msg->payload_len;
    if (msg->qos) head.length += 2;
    if (version == MQTT_PROTOCOL_V5) {
        head.length++;
    }
    std::vector<uint8_t> buf(head.length - msg->payload_len + 12);
    uint8_t* p = &buf[0]; 
    p += mqtt_head_pack(&head, p);
    PUSH16(p, msg->topic_len);
    PUSH_N(p, msg->topic, msg->topic_len);
    if (head.qos) {
        mid++;
        PUSH16(p, mid);
    }
    if (version == MQTT_PROTOCOL_V5) {
        *p++ = 0; // property
    }
    int ret = write(buf.data(), p - buf.data());
    if (msg->payload_len) {
        ret += write(msg->payload, msg->payload_len);
    }
    return 0;
}

int MqttSession::sendAck(int type, unsigned short mid, unsigned char reason) {
    mqtt_head_t head;
    memset(&head, 0, sizeof(head));
    head.type = type;
    if (type == MQTT_TYPE_PUBREL) {
        head.qos = 1;
    }
    unsigned char buf[12] = {0};
    unsigned char* p = buf;
    if (version == MQTT_PROTOCOL_V5) {
        head.length = 4; // mid(2) + reason(1) + prop_len(1)
    } else {
        head.length = 2;
    }
    int headlen = mqtt_head_pack(&head, p);
    p += headlen;
    PUSH16(p, mid);
    if (version == MQTT_PROTOCOL_V5) {
        *p++ = reason;
        *p++ = 0; // property length
    }
    return write(buf, p - buf);
}

MqttServer::MqttServer(EventLoopPtr loop) {
    topic_tree_ = std::make_shared<TopicTree>();
    tcp_server = std::make_shared<TcpServer>(loop);
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
    tcp_server->setUnpack(&mqtt_unpack_setting);

    tcp_server->onMessage = [this](const SocketChannelPtr& channel, Buffer* buf) {
        mqtt_head_t head;
        memset(&head, 0, sizeof(mqtt_head_t));
        unsigned char* p = (unsigned char*)buf->data();
        int headlen = mqtt_head_unpack(&head, p, buf->size());
        if (headlen <= 0 || buf->size() < headlen + head.length)
            return;
        auto set = channel->getContextPtr<MqttSession>();
        onMqttMessage(set, &head, p, p+headlen);
    };

    tcp_server->onConnection = [this](const SocketChannelPtr& channel) {
        std::string peeraddr = channel->peeraddr();
        if (channel->isConnected()) {
            hlogi("%s connected! connfd=%d id=%d", peeraddr.c_str(), channel->fd(), channel->id());
            channel->newContextPtr<MqttSession>()->tcp = channel;
        }
        else {
            hlogi("%s disconnected! connfd=%d id=%d", peeraddr.c_str(), channel->fd(), channel->id());
            auto set = channel->getContextPtr<MqttSession>();
            closeSession(set);
        }
    };
}

void MqttServer::dump() const {
    return topic_tree_->dump_tree([](const std::string& line) { printf("%s\n", line.c_str()); });
}

int MqttServer::publish(const std::string& topic, const std::string& payload, uint8_t qos, bool retain) {
    mqtt_message_t msg;
    msg.payload = payload.c_str();
    msg.payload_len = payload.length();
    msg.topic = topic.c_str();
    msg.topic_len = topic.length();
    msg.qos = qos;
    msg.retain = retain;
    return publish(&msg);
}

int MqttServer::publish(mqtt_message_t * msg) {
    int ret = 0;
    auto matchs = topic_tree_->get_subscribers(std::string(msg->topic, msg->topic_len));
    for (auto it : matchs) {
        if (auto cli = it.session.lock()) {
            cli->publish(msg);
            ret++;
        }
    }
    return ret;
}

void MqttServer::closeSession(MqttSession::Ptr set) {
    if (set) {
        if (set->will_topic.size() > 0) {
            publish(set->will_topic, set->will_payload);
            set->will_payload.clear();
            set->will_topic.clear();
        }
        // topic_.remove_session(set);
    }
}

int MqttServer::wsListen(int port, int ssl_port) {
    static WebSocketService ws;
    ws.onopen = [](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
        hlogi("%s %s onopen! connfd=%d id=%d", req->url.c_str(), 
            channel->peeraddr().c_str(), channel->fd(), channel->id());
        channel->newContextPtr<MqttSession>()->ws = channel;
    };
    ws.onmessage = [this](const WebSocketChannelPtr& channel, const std::string& msg) {
        auto set = channel->getContextPtr<MqttSession>();
        set->recv_buf.append(msg.c_str(), msg.size());
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
        if (size > 0) {
            memcpy(&set->recv_buf[0], p, size);
        }
        set->recv_buf.resize(size);
    };
    ws.onclose = [this](const WebSocketChannelPtr& channel) {
        hlogi("%s onclose! connfd=%d id=%d", channel->peeraddr().c_str(), channel->fd(), channel->id());
        auto ctx = channel->getContextPtr<MqttSession>();
        closeSession(ctx);
    };
    ws_server = std::make_shared<WebSocketServer>();
    ws_server->worker_processes = 0;
    ws_server->worker_threads = 1;
    if (ssl_port) {
        ws_server->https_port = ssl_port;
        hssl_ctx_opt_t param;
        memset(&param, 0, sizeof(param));
        param.crt_file = "cert/server.crt";
        param.key_file = "cert/server.key";
        param.endpoint = HSSL_SERVER;
        if (ws_server->newSslCtx(&param) != 0) {
            hloge("new SSL_CTX failed!");
            return -20;
        }
    }
    ws_server->port = port;
    ws_server->registerWebSocketService(&ws);
    return true;
}

void MqttServer::start(bool wait_threads_started) {
    tcp_server->start(wait_threads_started);
    if (ws_server) {
        ws_server->start();
    }
}

void MqttServer::stop(bool wait_threads_stoped) {
    tcp_server->stop(wait_threads_stoped);
    if (ws_server) {
        ws_server->stop();
    }
}

void MqttServer::onMqttMessage(MqttSession::Ptr channel, mqtt_head_t* head, uint8_t* start, uint8_t* p) {
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
        channel->version = conn.version;
        // MQTT v5 adds a property block after keepalive; skip it if present.
        p = channel->skipProp(p);

        POPSTR(p, conn.client_id);
        if (p > end) return;
        if (conn.conn_flag & MQTT_CONN_HAS_WILL) {
            // skip will_proprties
            p = channel->skipProp(p);
            POPSTR(p, conn.will_topic);
            if (p > end) return;
            POPSTR(p, conn.will_payload);
            if (p > end) return;
            channel->will_topic.assign(conn.will_topic.data, conn.will_topic.len);
            channel->will_payload.assign(conn.will_payload.data, conn.will_payload.len);
        }
        if (conn.conn_flag & MQTT_CONN_HAS_USERNAME) {
            POPSTR(p, conn.user_name);
            if (p > end) return;
        }
        if (conn.conn_flag & MQTT_CONN_HAS_PASSWORD) {
            POPSTR(p, conn.password);
            if (p > end) return;
        }

        uint8_t resp[3] = {0};
        p = resp;
        *p++ = 0; // conn_flag
        *p++ = onAuth(channel, &conn);
        if (conn.version == MQTT_PROTOCOL_V5) {
            // remaining length: connack flags + reason code + property length(0)
            *p++ = 0; // property length
        }
        channel->send(MQTT_TYPE_CONNACK, resp, p - resp);
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
        // skip properties for MQTT v5
        p = channel->skipProp(p);
        message.payload_len = end - p;
        if (message.payload_len > 0) {
            // NOTE: Not deep copy
            message.payload = (char*)p;
        }
        message.qos = head->qos;
        if (message.qos == 0) {
            // Do nothing
        } else if (message.qos == 1) {
            channel->sendAck(MQTT_TYPE_PUBACK, mid);
        } else if (message.qos == 2) {
            channel->sendAck(MQTT_TYPE_PUBREC, mid);
        }
        std::string topic(message.topic, message.topic_len);
        // 保留retain消息
        if (start[0] & 0x01) {
            std::vector<uint8_t> retain;
            if (message.payload_len > 0) {
                retain.resize(end - start);
                memcpy(retain.data(), start, end - start);
            }
            topic_tree_->set_retained_message(topic, retain, message.qos, head->dup);
            // 清除retain标记
            start[0] &= ~0x01;
        }
#if 1
        publish(&message);
#else
        auto matchs = topic_tree_->get_subscribers(topic);
        for (auto it : matchs) {
            if (auto cli = it.session.lock()) {
                cli->write(start, end - start);
            }
        }
#endif
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
            channel->sendAck(MQTT_TYPE_PUBREL, mid);
        }
        else if (head->type == MQTT_TYPE_PUBREL) {
            channel->sendAck(MQTT_TYPE_PUBCOMP, mid);
        }
    } break;
    case MQTT_TYPE_SUBSCRIBE:
        if (head->length>1) {
            POP16(p, mid);
            // skip properties for MQTT v5
            p = channel->skipProp(p);

            mqtt_message_t message;
            memset(&message, 0, sizeof(mqtt_message_t));
            POP16(p, message.topic_len);
            // NOTE: Not deep copy
            message.topic = (char*)p;
            p += message.topic_len;
            POP8(p, message.qos);

            uint8_t resp[4] = {0};
            p = resp;
            PUSH16(p, mid);
            *p++ = message.qos;
            if (channel->version == MQTT_PROTOCOL_V5) {
                *p++ = 0; // prop
            }
            channel->send(MQTT_TYPE_SUBACK, resp, p - resp);

            if (onSubscribe) onSubscribe(channel, &message);

            std::string topic(message.topic, message.topic_len);
            topic_tree_->subscribe(topic, channel, message.qos);
            for (auto it : topic_tree_->get_retained_messages(topic)) {
                auto& retained_msg = it.second;
                // 发送保留消息
                mqtt_message_t msg;
                msg.payload = (const char*)retained_msg.payload.data();
                msg.payload_len = retained_msg.payload.size();
                msg.topic = it.first.c_str();
                msg.topic_len = it.first.length();
                msg.retain = 1;
                msg.qos = (std::min)(message.qos, retained_msg.qos);
                channel->publish(&msg);
            }
        }
        break;
    case MQTT_TYPE_UNSUBSCRIBE: 
        if (head->length>1) {
            POP16(p, mid);
            // skip properties for MQTT v5
            p = channel->skipProp(p);

            channel->sendAck(MQTT_TYPE_UNSUBACK, mid);

            mqtt_message_t message;
            memset(&message, 0, sizeof(mqtt_message_t));
            POP16(p, message.topic_len);
            // NOTE: Not deep copy
            message.topic = (char*)p;
            p += message.topic_len;
            if (onUnsubscribe) onUnsubscribe(channel, &message);

            std::string topic(message.topic, message.topic_len);
            topic_tree_->unsubscribe(topic, channel);
        }        
        break;
    case MQTT_TYPE_PINGREQ: 
        channel->send(MQTT_TYPE_PINGRESP, nullptr, 0);
        break;
    case MQTT_TYPE_DISCONNECT: 
        channel->close();
        break;
    }
}
