#include  "RedisClient.h"
#include "EventLoopThread.h"
#include "hlog.h"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    int port = 6379;
    const char* host = "192.168.4.12";
    if (argc > 1) host = argv[1];
    if (argc > 2) port = atoi(argv[2]);
    hlog_set_handler(stdout_logger);

    hv::EventLoopThread loop;
    loop.start();

    RedisClient conn(loop.hloop());
    reconn_setting_t reconn;
    reconn_setting_init(&reconn);
    conn.setReconnect(&reconn);
    conn.open(host, port);
    conn.set("aa", "bb", [&](reply& r) {
        printf("set return %s\n", r.str().c_str());
        conn.get("aa", [](reply& r) {
            printf("get return %s\n", r.str().c_str());
        });
    });

    const char* topic = "topic";
    RedisClient subs(loop.hloop());
    subs.setReconnect(&reconn);
    subs.open(host, port);
    subs.subscribe(topic, [](const std::string& channel, const std::string& msg) { 
        std::cout << channel << "> " << msg << std::endl;
    });
    
    while (1) {
        std::string s;
        getline(std::cin, s);
        if (s == "q" || s == "quit") {
            break;
        }
        conn.publish(topic, s);
    }
    conn.close();
    subs.close();
    loop.stop();
}
