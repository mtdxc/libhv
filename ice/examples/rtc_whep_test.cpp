#include "hlog.h"
#include "rtc/RtcClient.h"
#include "mp4/Mp4Writer.h"
using namespace ice;

int main(int argc, char* argv[]) {
    IceConfig config;
    auto client = std::make_shared<WhepClient>(&config);
    const char* url = "http://127.0.0.1:8080/index/api/whep?app=live&stream=test";
    if (argc > 1) {
        url = argv[1];
    }
    std::shared_ptr<Mp4Writer> mp4_writer;
    if (argc > 2) {
        const char* mp4_path = argv[2];
        mp4_writer = std::make_shared<Mp4Writer>();
        if (!mp4_writer->Open(mp4_path)) {
            printf("open mp4 file failed: %s\n", mp4_path);
            return -1;
        }
    }
    logger_enable_color(hlog, true);
    hlog_set_handler(stdout_logger);
    client->onFrame = [mp4_writer](Frame::Ptr frame) {
        printf("recv frame %s\n", frame->toString().c_str());
        if (mp4_writer) {
            mp4_writer->inputFrame(frame);
        }
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