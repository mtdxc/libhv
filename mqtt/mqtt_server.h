#ifndef _HV_MQTT_SERVER_H_
#define _HV_MQTT_SERVER_H_

#include "hexport.h"
#include "mqtt_protocol.h"
#include "TcpServer.h"
#include "WebSocketServer.h"

namespace hv {
class TopicTree;

struct MqttSession {
    using Ptr = std::shared_ptr<MqttSession>;
    hv::SocketChannelPtr tcp;
    WebSocketChannelPtr ws;
    std::string recv_buf; // ws recv buffer
    unsigned char version = MQTT_PROTOCOL_V311; // default MQTT protocol version
    std::string will_topic;
    std::string will_payload;
    uint16_t mid = 0;
    int publish(mqtt_message_t* msg);

    uint8_t* skipProp(uint8_t*) const;
    int sendAck(int type, unsigned short mid, unsigned char reason = 0);
    int send(int type, void* buff, int len);
    int write(const void* buff, int size);
    bool close();
};

class HV_EXPORT MqttServer {
    std::shared_ptr<TopicTree> topic_tree_;
    std::shared_ptr<WebSocketServer> ws_server;
    std::shared_ptr<TcpServer> tcp_server;
    void closeSession(MqttSession::Ptr set);
    void onMqttMessage(MqttSession::Ptr channel, mqtt_head_t* head, uint8_t* start, uint8_t* p);

public:
    MqttServer(EventLoopPtr loop = nullptr);

    int listen(int port = 1883, const char* host = "0.0.0.0") {
        return tcp_server->createsocket(port, host);
    }
    int wsListen(int port, int ssl_port);
    void start(bool wait_threads_started = true);
    void stop(bool wait_threads_stoped = true);
    void dump() const;

    int publish(const std::string& topic, const std::string& msg, uint8_t qos = 0, bool retain = false);
    int publish(mqtt_message_t* msg);
    typedef std::function<void(MqttSession::Ptr, mqtt_message_t*)> MqttMessageCallback;
    MqttMessageCallback onPublish;
    MqttMessageCallback onSubscribe;
    MqttMessageCallback onUnsubscribe;
    std::function<mqtt_connack_e(MqttSession::Ptr, mqtt_conn_t*)> onAuth;

};
} // namespace hv
#endif // _HV_MQTT_SERVER_H_