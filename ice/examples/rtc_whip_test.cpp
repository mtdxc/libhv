#include "hlog.h"
#include "rtc/RtcClient.h"
#include "mp4/Mp4Reader.h"
using namespace ice;

class WhipMp4Client : public WhipClient {
    std::shared_ptr<Mp4Reader> mp4_reader_;
public:
    WhipMp4Client(const IceConfig* options) : WhipClient(options) {}
    bool Open(const char* path) {
        mp4_reader_ = std::make_shared<Mp4Reader>(loop().get());
        bool ret = mp4_reader_->Open(path);
        if (ret) {
            setAudioCodec(mp4_reader_->aCodec);
            setVideoCodec(mp4_reader_->vCodec);
        }
        return ret;
    }
    void onStartWebRTC() override {
        WhipClient::onStartWebRTC();
        mp4_reader_->addDelegate(std::dynamic_pointer_cast<FrameWriterInterface>(shared_from_this()));
    }
    void onClose() override {
        WhipClient::onClose();
        if (mp4_reader_) {
            mp4_reader_->delDelegate(this);
            mp4_reader_->Close();
            mp4_reader_.reset();
        }
    }
    bool seek(int64_t tsp) {
        if (!mp4_reader_) return false;
        return mp4_reader_->Seek(tsp, true) >= 0;
    }
};

int main(int argc, char* argv[]) {
    IceConfig config;
    auto client = std::make_shared<WhipMp4Client>(&config);
    const char* url = "http://127.0.0.1:8080/index/api/whip?app=live&stream=test";
    if (argc > 1) {
        const char* mp4_path = argv[1];
        if (!client->Open(mp4_path)) {
            printf("open mp4 file failed: %s\n", mp4_path);
            return -1;
        }
        if (argc > 2) {
            url = argv[2];
        }
    }
    else {
        printf("usage: %s <mp4_file> [whip_url]\n", argv[0]);
        return -1;
    }

    logger_enable_color(hlog, true);
    hlog_set_handler(stdout_logger);
    
    client->open(url);
    printf("press q to quit loop\n");
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strcasecmp(line, "q\n") || !strcasecmp(line, "quit\n")) {
            printf("user break loop\n");
            break;
        }
        if (line[0] == 's' && line[1] == ' ') {
            int sec = atoi(line + 2);
            if (sec > 0) {
                client->seek(sec * 1000);
            }
        }
        else {
            printf("unknown command: %s", line);
        }
    }
    client->close();
    return 0;
}