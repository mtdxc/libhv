#ifndef _HV_MQTT_SERVER_H_
#define _HV_MQTT_SERVER_H_

#include "mqtt_protocol.h"
#include "topic_tree.h"
#include "TcpServer.h"
#include "http/server/WebSocketServer.h"

namespace hv {

class MqttServer : public TcpServer {
    TopicTree topic_tree_;
    std::shared_ptr<WebSocketServer> wsServer;
    void closeSession(MqttSession::Ptr set);
    void onMqttMessage(MqttSession::Ptr channel, mqtt_head_t* head, uint8_t* start, uint8_t* p);

public:
    MqttServer(EventLoopPtr loop = nullptr);

    int listen(int port = 1883, const char* host = "0.0.0.0") {
        return TcpServer::createsocket(port, host);
    }
    int wsListen(int port, int ssl_port);

    const TopicTree& topic_tree() const {
        return topic_tree_;
    }

    typedef std::function<void(MqttSession::Ptr, mqtt_message_t*)> MqttMessageCallback;
    MqttMessageCallback onPublish;
    MqttMessageCallback onSubscribe;
    MqttMessageCallback onUnsubscribe;
    std::function<mqtt_connack_e(MqttSession::Ptr, mqtt_conn_t*)> onAuth;

};
} // namespace hv
#endif // _HV_MQTT_SERVER_H_