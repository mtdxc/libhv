﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "IceServer.hpp"
#include "WebRtcServer.h"
#include "WebRtcTransport.h"
#include "hstring.h"
#include "hbase.h"
#include "iniparser.h"
#include "Common/config.h"
#define SK_APP "rtc"

using namespace mediakit;

static std::string getUserName(void* buf, int len) {
    if (!RTC::StunPacket::IsStun((const uint8_t *) buf, len)) {
        return "";
    }
    std::unique_ptr<RTC::StunPacket> packet(RTC::StunPacket::Parse((const uint8_t *) buf, len));
    if (!packet) {
        return "";
    }
    if (packet->GetClass() != RTC::StunPacket::Class::REQUEST ||
        packet->GetMethod() != RTC::StunPacket::Method::BINDING) {
        return "";
    }
    //收到binding request请求
    auto vec = hv::split(packet->GetUsername(), ':');
    return vec[0];
}

//////////////////////////////////////////////////////////////////////////////////
WebRtcServer::WebRtcServer() {
}

WebRtcServer::~WebRtcServer() {
    stop();
}

WebRtcServer& WebRtcServer::Instance()
{
    static WebRtcServer instance;
    return instance;
}

void WebRtcServer::start()
{
    startHttp();
    startUdp();
}

void WebRtcServer::stop()
{
    websocket_server_stop(&_http);
    _udp.stop();
}

void WebRtcServer::detect_local_ip()
{
    sockaddr_u localAddr, remoteAddr;
    sockaddr_set_ipport(&remoteAddr, "8.8.8.8", 1234);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    connect(fd, (sockaddr*)&remoteAddr, sockaddr_len(&remoteAddr));
    int len = sizeof(localAddr);
    getsockname(fd, (sockaddr*)&localAddr, &len);
    char ip[32];
    this->_local_ip = sockaddr_ip(&localAddr, ip, 32);
    LOGI("detect localAddr %s", ip);
}


void WebRtcServer::registerPlugin(const std::string &type, Plugin cb) {
    std::lock_guard<std::mutex> lck(_mtx_creator);
    _map_creator[type] = std::move(cb);
}

WebRtcTransportPtr WebRtcServer::getItem(const std::string &key) {
    WebRtcTransportImp::Ptr ret;
    if (key.empty()) {
        return ret;
    }
    std::lock_guard<std::mutex> lck(_mtx);
    auto it = _map.find(key);
    if (it != _map.end()) {
        ret = it->second.lock();
    }
    return ret;
}

void WebRtcServer::startHttp()
{
    auto& cfg = IniParser::Instance();
    int port = 80;
    static HttpService http;
    http.document_root = cfg.Get<std::string>("DocRoot", SK_APP, "html");
    http.POST("/echo", [](const HttpContextPtr& ctx) {
        return ctx->send(ctx->body(), ctx->type());
    });
    http.POST("/index/api/webrtc", [this](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
        onRtcOfferReq(req, writer);
    });

    static WebSocketService ws;
#if 0
    ws.onopen = [](const WebSocketChannelPtr& channel, const std::string& url) {
        printf("onopen: GET %s\n", url.c_str());
        MyContext* ctx = channel->newContext<MyContext>();
        // send(time) every 1s
        ctx->timerID = setInterval(1000, [channel](TimerID id) {
            if (channel->isConnected() && channel->isWriteComplete()) {
                char str[DATETIME_FMT_BUFLEN] = { 0 };
                datetime_t dt = datetime_now();
                datetime_fmt(&dt, str);
                channel->send(str);
            }
        });
    };
    ws.onmessage = [](const WebSocketChannelPtr& channel, const std::string& msg) {
        MyContext* ctx = channel->getContext<MyContext>();
        ctx->handleMessage(msg);
    };
    ws.onclose = [](const WebSocketChannelPtr& channel) {
        printf("onclose\n");
        MyContext* ctx = channel->getContext<MyContext>();
        if (ctx->timerID != INVALID_TIMER_ID) {
            killTimer(ctx->timerID);
        }
        channel->deleteContext<MyContext>();
    };
    _http.set
#endif

    _http.port = cfg.Get<int>("HttpPort", SK_APP, 80);
    _http.https_port = cfg.Get<int>("HttpsPort", SK_APP, 443);
    if (_http.https_port) {
        hssl_ctx_init_param_t param;
        memset(&param, 0, sizeof(param));
        auto cert = cfg.Get<std::string>("cert_file", SK_APP, "cert/server.crt");
        auto key = cfg.Get<std::string>("key_file", SK_APP, "cert/server.key");
        param.crt_file = cert.c_str();
        param.key_file = key.c_str();
        param.endpoint = HSSL_SERVER;
        if (hssl_ctx_init(&param) == NULL) {
            fprintf(stderr, "hssl_ctx_init failed!\n");
        }
    }
    _http.service = &http;
    _http.ws = &ws;
    websocket_server_run(&_http, 0);
}

void WebRtcServer::onRtcOfferReq(const HttpRequestPtr& req, const HttpResponseWriterPtr& writer)
{
    auto type = req->GetParam("type");
    auto offer = req->Body();
    CHECK(!offer.empty(), "http body(webrtc offer sdp) is empty");
    WebRtcArgs args;
    Plugin plugin;
    {
        std::lock_guard<std::mutex> lck(_mtx_creator);
        auto it = _map_creator.find(type);
        if (it == _map_creator.end()) {
            HttpResponse resp;
            resp.Set("code", "exception");
            resp.Set("msg", "the type can not supported");
            WarnL << "onRtcOfferReq type unsupported: " << type;
            writer->WriteResponse(&resp);
            return;
        }
        plugin = it->second;
    }

    plugin(nullptr, offer, args,
        [writer, offer](const WebRtcInterface &exchanger) mutable {
            HttpResponse resp;
            //设置返回类型
            resp.SetHeader("Content-Type", "application/json");
            //设置跨域
            resp.SetHeader("Access-Control-Allow-Origin", "*");

            try {
                resp.Set("code", 0);
                resp.Set("sdp", const_cast<WebRtcInterface &>(exchanger).getAnswerSdp(offer));
                resp.Set("id", exchanger.getIdentifier());
                resp.Set("type", "answer");
            }
            catch (std::exception &ex) {
                WarnL << "onRtcOfferReq exception: " << ex.what();
                resp.Set("code", "exception");
                resp.Set("msg", ex.what());
            }
            writer->WriteResponse(&resp);
        });
}

void WebRtcSession::onRecv(hv::Buffer* buf) {
    if (_find_transport) {
        //只允许寻找一次transport
        _find_transport = false;
        auto user_name = getUserName(buf->data(), buf->size());
        _identifier = std::to_string(fd()) + '-' + user_name;
        auto transport = WebRtcServer::Instance().getItem(user_name);
        CHECK(transport && transport->getPoller()->isInLoopThread());
        transport->setSession(shared_from_this());
        _transport = std::move(transport);
        //InfoP(this);
    }
    CHECK(_transport);
    try {
        _transport->inputSockData((char*)buf->data(), buf->size(), _peer_addr);
    }
    catch (...) {
    }
}

WebRtcSession::WebRtcSession(hio_t* io) : hv::SocketChannel(io)
{
    _peer_addr = hio_peeraddr(io);
    this->onconnect = []() {
    };

    this->onclose = [this]() {
    };

    this->onread = [this](hv::Buffer* buf) {
        onRecv(buf);
    };
}

void WebRtcServer::startUdp()
{
    GET_CONFIG(uint16_t, local_port, RTC::kPort);
    if (!local_port) {
        IniParser::Instance().SetValue(RTC::kPort, "8000");
        local_port = 8000;
    }
    int listenfd = _udp.createsocket(local_port);
    if (listenfd < 0) {
        return;
    }
    LOGI("udp listen on %d, listenfd=%d ...\n", local_port, listenfd);

    _udp.onNewClient = [this](hio_t* io, hv::Buffer* data) {
        // @todo select loop with data or peerAddr
        auto user_name = getUserName(data->data(), data->size());
        if (user_name.empty()) {
            return;
        }
        if(auto ret = getItem(user_name))
            _udp.accept(io, ret->getPoller());
    };
    _udp.onMessage = [this](const std::shared_ptr<WebRtcSession> & session, hv::Buffer* buf) {
        session->onRecv(buf);
    };
    _udp.setThreadNum(4);
    _udp.setLoadBalance(LB_LeastConnections);

    _udp.start();
}
//////////////////////////////////////////////////////////////////////////
int main(int argc, char** argv) {
    char path[200];
    char* cfgPath = get_executable_path(path, sizeof(path));
    if (argc > 1) {
        cfgPath = argv[1];
    }
    else {
        char* dot = strrchr(path, '.');
        strcpy(dot, ".ini");
    }
    IniParser::Instance().LoadFromFile(cfgPath);

    hlog_set_level(IniParser::Instance().Get<int>("LogLevel", SK_APP, LOG_LEVEL_DEBUG));
    hlog_set_handler(stdout_logger);

    WebRtcServer::Instance().start();

    printf("press any key to exit\n");
    getchar();
    
    WebRtcServer::Instance().stop();
}