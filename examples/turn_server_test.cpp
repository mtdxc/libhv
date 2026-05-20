#include "turn_server.h"
using namespace ice;

int main() {
    hlog_set_handler(stdout_logger);

    TurnServerOptions opts;
    opts.bindHost = "0.0.0.0";
    opts.udpPort = 3478;
    opts.tcpPort = 3478;
    opts.realm = "ling.com";
    opts.software = "libhv-ice/1.0";
    opts.users = {{"ling", "ling1234"}};

    TurnServer turn_server;
    turn_server.setOptions(opts);

    turn_server.start();
    printf("Turn server started, press 'q' to quit\n");
    while (getchar() != 'q') {
    }
    turn_server.stop();
    return 0;
}
