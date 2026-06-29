#include "RtcHttpServer.h"
#include "RtcTransportImp.hpp"
#include "http/server/WebSocketServer.h"
#include "agent/ice_agent.h"
#include "mp4/Mp4Writer.h"
#include "mp4/Mp4Reader.h"
#include "EventLoop.h"
using namespace hv;
using namespace ice;

class WebRtcEcho : public WebRtcTransport {
public:
    using Ptr = std::shared_ptr<WebRtcEcho>;
    WebRtcEcho(const IceConfig *options, IceAgent *agent) : WebRtcTransport(options, agent) {}
    void onRtcConfigure(RtcConfigure &configure) const override{
        WebRtcTransport::onRtcConfigure(configure);
        configure.audio.direction = configure.video.direction = RtpDirection::sendrecv;
        configure.audio.extmap.emplace(RtpExtType::sdes_mid, RtpDirection::sendrecv);
        configure.video.extmap.emplace(RtpExtType::sdes_mid, RtpDirection::sendrecv);
    }
    void onRtp(const char *buf, size_t len, uint64_t stamp_ms) override {
        sendRtp(buf, len, nullptr);
    }

    void onRtcp(const char *buf, size_t len) override {
        sendRtcp(buf, len, nullptr);
    }
};

class WebRtcEcho2 : public WebRtcTransportImp {
public:
    using Ptr = std::shared_ptr<WebRtcEcho2>;
    WebRtcEcho2(const IceConfig *options, IceAgent *agent) : WebRtcTransportImp(options, agent) {}
    void onRtcConfigure(RtcConfigure &configure) const override{
        WebRtcTransportImp::onRtcConfigure(configure);
        configure.audio.direction = configure.video.direction = RtpDirection::sendrecv;
        configure.audio.extmap.emplace(RtpExtType::sdes_mid, RtpDirection::sendrecv);
        configure.video.extmap.emplace(RtpExtType::sdes_mid, RtpDirection::sendrecv);
    }
    void onRecvFrame(MediaTrack &track, const std::string &rid, Frame::Ptr rtp) override {
        // hlogi("%s onRecvFrame %s", rid.c_str(), rtp->toString().c_str());
        sendFrame(rtp);
    }
};

std::string record_dir = "record";
class WebRtcPusher : public WebRtcTransportImp {
public:
    using Ptr = std::shared_ptr<WebRtcPusher>;
    WebRtcPusher(const IceConfig *options, IceAgent *agent) : WebRtcTransportImp(options, agent) {}
    void onRtcConfigure(RtcConfigure &configure) const override{
        WebRtcTransportImp::onRtcConfigure(configure);
        configure.audio.direction = configure.video.direction = RtpDirection::recvonly;
    }
    void onStartWebRTC() override {
        WebRtcTransportImp::onStartWebRTC();
        auto dispatcher = std::make_shared<FrameDispatcher>();
        dispatcher->setGopCache(true);
        if (auto track = getTrack(TrackAudio)) {
            dispatcher->aCodec = track->getCodec();
        }
        if (auto track = getTrack(TrackVideo)) {
            dispatcher->vCodec = track->getCodec();
        }
        dispatcher->sdp = _answer_sdp ? _answer_sdp->toRtspSdp() : "";
        auto stream = getStream();
        hlogi("WebRtcPusher %s onStartWebRTC stream: %s, sdp: %s", getIdentifier(), stream.c_str(), dispatcher->sdp.c_str());
        RtcHttpServer::setDispatcher(stream, dispatcher);
        auto it = params_.find("record");
        if (it!=params_.end() && it->second == "1") {
            auto writer = std::make_shared<Mp4Writer>();
            auto now = datetime_now();
            char path[MAX_PATH] = {0};
            snprintf(path, sizeof(path), "%s/%s_%02d%02d_%02d%02d%02d.mp4", record_dir.c_str(), stream.c_str(), 
                now.month, now.day, now.hour, now.min, now.sec);
            if (writer->Open(path)) {
                dispatcher->addDelegate(writer);
                hlogi("WebRtcPusher %s startRecord %s", getIdentifier(), path);
            }

        }
    }

    void onRecvFrame(MediaTrack &track, const std::string &rid, Frame::Ptr rtp) override {
        // hlogi("%s onRecvFrame %s", rid.c_str(), rtp->toString().c_str());
        auto dispatcher = RtcHttpServer::getDispatcher(getStream());
        if (dispatcher) {
            dispatcher->inputFrame(rtp);
        }
    }
    void onClose() override {
        WebRtcTransportImp::onClose();
        RtcHttpServer::setDispatcher(getStream(), nullptr);
    }
};

class WebRtcPlayer : public WebRtcTransportImp {
public:
    using Ptr = std::shared_ptr<WebRtcPlayer>;
    WebRtcPlayer(const IceConfig *options, IceAgent *agent) : WebRtcTransportImp(options, agent) {}
    void onRtcConfigure(RtcConfigure &configure) const override{
        WebRtcTransportImp::onRtcConfigure(configure);
        configure.audio.direction = configure.video.direction = RtpDirection::sendonly;

        // configure.setPlayRtspInfo(sdp);
        auto dispatcher = RtcHttpServer::getDispatcher(getStream());
        if (dispatcher) {
            if (dispatcher->sdp.empty()) {
                configure.setPlayRtspInfo(dispatcher->aCodec, dispatcher->vCodec);
            } else {
                configure.setPlayRtspInfo(dispatcher->sdp);
            }
        }
    }
    void onStartWebRTC() override {
        WebRtcTransportImp::onStartWebRTC();
        auto dispatcher = RtcHttpServer::getDispatcher(getStream());
        if (dispatcher) {
            dispatcher->addDelegate(std::dynamic_pointer_cast<FrameWriterInterface>(shared_from_this()));
        }
    }

    void onClose() override {
        WebRtcTransportImp::onClose();
        auto dispatcher = RtcHttpServer::getDispatcher(getStream());
        if (dispatcher) {
            dispatcher->delDelegate(this);
        }
    }
};

WebRtcTransport::Ptr RtcHttpServer::createSession(const char* type, IceAgent *agent) {
    if (strcmp(type, RTC_CLASS_ECHO) == 0)
        return WebRtcTransport::Ptr(new WebRtcEcho(nullptr, agent));
    else if (strcmp(type, RTC_CLASS_PUSH) == 0)
        return WebRtcTransport::Ptr(new WebRtcPusher(nullptr, agent));
    else if (strcmp(type, RTC_CLASS_PLAY) == 0)
        return WebRtcTransport::Ptr(new WebRtcPlayer(nullptr, agent));
    else if (strcasecmp(type, RTC_CLASS_TALK) == 0)
        return WebRtcTransport::Ptr(new WebRtcEcho2(nullptr, agent));
    else
        return WebRtcTransport::Ptr(new WebRtcTransportImp(nullptr, agent));
}

std::mutex g_stream_map_mtx;
std::map<std::string, FrameDispatcher::Ptr> g_stream_map;
FrameDispatcher::Ptr RtcHttpServer::getDispatcher(const std::string &stream) {
    std::lock_guard<std::mutex> lck(g_stream_map_mtx);
    auto it = g_stream_map.find(stream);
    if (it != g_stream_map.end()) {
        return it->second;
    } else if(hv::endswith(stream, ".mp4")) {
        auto reader = std::make_shared<Mp4Reader>(currentThreadEventLoop);
        std::string path = record_dir + "/" + stream;
        if (reader->Open(path.c_str())) {
            g_stream_map[stream] = reader;
            hlogi("Mp4Reader open %s", stream.c_str());
            return reader;
        }
    }
    return nullptr;
}

void RtcHttpServer::setDispatcher(std::string name, FrameDispatcher::Ptr dispatcher){
    std::lock_guard<std::mutex> lck(g_stream_map_mtx);
    if (dispatcher == nullptr) {
        g_stream_map.erase(name);
    } else {
        g_stream_map[name] = dispatcher;
    }
}

RtcHttpServer::RtcHttpServer(const RtcHttpConfig& config) : config_(config) {
#if defined(_WIN32)
    _mkdir(record_dir.c_str());
#else
    mkdir(record_dir.c_str(), 0755);
#endif
}

RtcHttpServer::~RtcHttpServer() {
    stop();
}

class MyContext {
public:
    MyContext() {
        printf("MyContext::MyContext()\n");
    }
    ~MyContext() {
        printf("MyContext::~MyContext()\n");
    }

    int handleMessage(const std::string& msg, enum ws_opcode opcode) {
        printf("onmessage(type=%s len=%d): %.*s\n", opcode == WS_OPCODE_TEXT ? "text" : "binary",
            (int)msg.size(), (int)msg.size(), msg.data());
        return msg.size();
    }
};

void RtcHttpServer::start() {

    static HttpService http;
    http.document_root = "html";
    http.Any("/index/api/whep", [this](const HttpContextPtr& ctx) {
        auto sdp = ctx->body();
        auto transport = createSession(RTC_CLASS_PLAY, agent_.get());
        transport->setParam(ctx->params());
        ctx->setHeader("Content-Type", "application/sdp");
        return ctx->sendString(transport->getAnswerSdp(sdp));
    });
    http.Any("/index/api/whip", [this](const HttpContextPtr& ctx) {
        auto sdp = ctx->body();
        auto transport = createSession(RTC_CLASS_PUSH, agent_.get());
        transport->setParam(ctx->params());
        ctx->setHeader("Content-Type", "application/sdp");
        return ctx->sendString(transport->getAnswerSdp(sdp));
    });
    http.Any("/index/api/webrtc", [this](const HttpContextPtr& ctx) {
        auto type = ctx->param("type");
        auto sdp = ctx->body();
        auto transport = createSession(type.c_str(), agent_.get());
        transport->setParam(ctx->params());
        Json val;
        val["sdp"] = transport->getAnswerSdp(sdp);
        val["id"] = transport->getIdentifier();
        val["token"] = transport->getIdentifier();
        val["type"] = "answer";
        val["code"] = 0;
        return ctx->sendJson(val);
    });
    static WebSocketService ws;
    // ws.setPingInterval(10000);
    ws.onopen = [](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
        printf("onopen: GET %s\n", req->Path().c_str());
        auto ctx = channel->newContextPtr<MyContext>();
    };
    ws.onmessage = [](const WebSocketChannelPtr& channel, const std::string& msg) {
        auto ctx = channel->getContextPtr<MyContext>();
        ctx->handleMessage(msg, channel->opcode);
    };
    ws.onclose = [](const WebSocketChannelPtr& channel) {
        printf("onclose\n");
        auto ctx = channel->getContextPtr<MyContext>();
        // channel->deleteContextPtr();
    };

    http_.reset(new hv::WebSocketServer());
    http_->port = config_.http_port;
#if TEST_WSS
    http_->https_port = config_.https_port;
    hssl_ctx_opt_t param;
    memset(&param, 0, sizeof(param));
    param.crt_file = config_.cert_file;
    param.key_file = config_.key_file;
    param.endpoint = HSSL_SERVER;
    if (http_->newSslCtx(&param) != 0) {
        fprintf(stderr, "new SSL_CTX failed!\n");
        return -20;
    }
#endif
    http_->registerHttpService(&http);
    http_->registerWebSocketService(&ws);
    http_->start();
    http_->onWorkerStart = [this]() {
        if (!agent_) {
            agent_ = std::make_shared<ice::IceAgent>(http_.get());
            agent_->setConfig(config_.ice);
            agent_->start();
        }
    };
}

void RtcHttpServer::stop() {
    if (agent_) {
        agent_->stop();
        agent_.reset();
    }
    if (http_) {
        http_->stop();
        http_.reset();
    }
}