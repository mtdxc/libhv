/*
 * TcpServer_test.cpp
 *
 * @build   make evpp
 * @server  bin/TcpServer_test 1234
 * @client  bin/TcpClient_test 1234
 *
 */

#include "mqtt_server.h"
using namespace hv;

#define TEST_TLS        0

int main(int argc, char* argv[]) {
    int port = 1883;
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    hlog_set_level(LOG_LEVEL_DEBUG);

    MqttServer srv;
    int listenfd = srv.listen(port);
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
