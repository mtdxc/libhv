#include "hlog.h"
#include "RtcHttpServer.h"

int main(int argc, char* argv[]) {
    int port = 8080;
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    logger_enable_color(hlog, true);
    hlog_set_handler(stdout_logger);
    RtcHttpServer server;
    server.start(port);
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