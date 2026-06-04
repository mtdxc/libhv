#include "RtcHttpServer.h"
#include "WebRtcTransport.hpp"
using namespace hv;
using namespace ice;

RtcHttpServer::RtcHttpServer() {
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

void RtcHttpServer::start(int port) {

    static HttpService http;
    http.document_root = "html";
    http.Any("/index/api/whep", [this](const HttpContextPtr& ctx) {
        auto type = ctx->param("type");
        auto sdp = ctx->body();
        WebRtcOptions opt;
        opt.iceMode = IceMode::Lite;
        auto transport = std::make_shared<WebRtcTransport>(opt, agent_.get());
        transport->setRemoteDescription(sdp);
        // delay free
        setTimeout(10000, [transport](TimerID) {});
        return ctx->send(transport->createAnswer());
    });
    http.Any("/index/api/whip", [this](const HttpContextPtr& ctx) {
        auto type = ctx->param("type");
        auto sdp = ctx->body();
        WebRtcOptions opt;
        opt.iceMode = IceMode::Lite;
        auto transport = std::make_shared<WebRtcTransport>(opt, agent_.get());
        transport->setRemoteDescription(sdp);
        // delay free
        setTimeout(10000, [transport](TimerID) {});
        return ctx->send(transport->createAnswer());
    });
    http.Any("/index/api/webrtc", [this](const HttpContextPtr& ctx) {
        auto type = ctx->param("type");
        auto sdp = ctx->body();
        WebRtcOptions opt;
        opt.iceMode = IceMode::Lite;
        auto transport = std::make_shared<WebRtcTransport>(opt, agent_.get());
        transport->setRemoteDescription(sdp);
        // delay free
        setTimeout(10000, [transport](TimerID) {});
        Json val;
        val["sdp"] = transport->createAnswer();
        val["id"] = transport->getIdentifier();
        val["token"] = transport->getIdentifier();
        val["type"] = "answer";
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

    http_ = std::make_unique<hv::WebSocketServer>();
    http_->port = port;
#if TEST_WSS
    http_->https_port = port + 1;
    hssl_ctx_opt_t param;
    memset(&param, 0, sizeof(param));
    param.crt_file = "cert/server.crt";
    param.key_file = "cert/server.key";
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
            ice::IceConfig config;
            config.gatherTcp = true;
            agent_->setConfig(config);
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