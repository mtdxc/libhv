#include "hlog.h"
#include "rtc/RtcClient.h"
using namespace ice;

int main(int argc, char* argv[]) {
    IceConfig config;
    auto client = std::make_shared<WhepClient>(&config);
    const char* url = "http://127.0.0.1:8080/index/api/whep?app=live&stream=test";
    if (argc > 1) {
        url = argv[1];
    }
    logger_enable_color(hlog, true);
    hlog_set_handler(stdout_logger);
    client->onFrame = [](Frame::Ptr frame) {
        printf("recv frame %s\n", frame->toString().c_str());
    };
    client->open(url);
    printf("press q to quit loop\n");
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strcasecmp(line, "q\n") || !strcasecmp(line, "quit\n")) {
            printf("user break loop\n");
            break;
        }
    }
    client->close();
    return 0;
}