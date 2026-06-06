#include "hlog.h"
#include "rtc/RtcHttpServer.h"

int main(int argc, char* argv[]) {
    RtcHttpConfig config;
    config.ice.tcpPort = 8000;
    config.ice.udpPort = 8000;
    config.ice.gatherTcp = true;
    config.http_port = 8080;
    config.https_port = 8081;
    config.cert_file = "cert/server.crt";
    config.key_file = "cert/server.key";
    if (argc > 1) {
        config.http_port = atoi(argv[1]);
    }
    logger_enable_color(hlog, true);
    hlog_set_handler(stdout_logger);
    
    RtcHttpServer server(config);
    server.start();
    printf("press q to quit loop\n");
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strcasecmp(line, "q\n") || !strcasecmp(line, "quit\n")) {
            printf("user break loop\n");
            break;
        }
    }
    server.stop();
    return 0;
}