#include "hlog.h"
#include "AvCapture.h"
#include "rtc/RtcClient.h"
using namespace ice;

class SdlPusher : public WhipClient {
    AvCapture cap;
    void onStartWebRTC() override {
        WhipClient::onStartWebRTC();
        cap.start();
        cap.addDelegate(std::dynamic_pointer_cast<FrameWriterInterface>(shared_from_this()));
    }
    void onKeyFrameReq(MediaTrack &track, uint32_t ssrc) override {
        cap.RequestKeyFrame();
    }
    void onClose() override {
        WhipClient::onClose();
        cap.delDelegate(this);
        cap.stopAll();
    }
public:
    SdlPusher(IceConfig* config) : WhipClient(config) {}
    void open(const char* url) {
        setAudioCodec(CodecOpus);
        setVideoCodec(CodecH264);
        cap.setupAudio(acodec_, 64000);
        // 改成800x600否则在windows中会出现粉屏...
        cap.setupVideo(vcodec_, 800, 600, 30, 400000);
        // cap.setPreview(true);
        WhipClient::open(url);
    }
};

int main(int argc, char* argv[]) {
    logger_enable_color(hlog, true);
    hlog_set_handler(stdout_logger);

    IceConfig config;
    auto client = std::make_shared<SdlPusher>(&config);
    const char* url = "http://127.0.0.1:8080/index/api/whip?app=live&stream=test";
    if (argc > 1) {
        url = argv[1];
    }
    client->open(url);
    printf("press any key to quit\n");
    getchar();
    client->safeClose();
}