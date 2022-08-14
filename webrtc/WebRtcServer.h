/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */


#ifndef ZLMEDIAKIT_WEBRTCSERVER_H
#define ZLMEDIAKIT_WEBRTCSERVER_H

#include "Socket.h"
#include "UdpServer2.h"
#include "TcpServer.h"
#include "srt/SrtSession.hpp"
#include "webrtc/WebRtcSession.h"
#include "Rtmp/RtmpSession.h"
#include "Rtsp/RtspSession.h"
#include "http/server/WebSocketServer.h"
class WebRtcInterface;
class WebRtcTransportImp;
typedef std::shared_ptr<WebRtcTransportImp> WebRtcTransportPtr;

using WebRtcArgs = std::map<std::string, std::string>;

class WebRtcServer {
public:
    friend class WebRtcTransportImp;

    WebRtcServer();
    ~WebRtcServer();

    void start(const char* cfg);
    void stop();

    static WebRtcServer& Instance();
    hv::EventLoopPtr getPoller();

    using onCreateRtc = std::function<void(const WebRtcInterface &rtc)>;
    using Plugin = std::function<void(hv::SocketChannelPtr sender, const std::string &offer, const WebRtcArgs &args, const onCreateRtc &cb)>;

    void registerPlugin(const std::string &type, Plugin cb);
    WebRtcTransportPtr getItem(const std::string &key);

    std::string get_local_ip() {
        if (_local_ip.empty())
            detect_local_ip();
        return _local_ip;
    }
    void detect_local_ip();

    using RtcServer = hv::UdpServerEventLoopTmpl2<WebRtcSession>;
    using SrtServer = hv::UdpServerEventLoopTmpl2<SRT::SrtSession>;
    using RtmpServer = hv::TcpServerEventLoopTmpl<mediakit::RtmpSession>;
    using RtspServer = hv::TcpServerEventLoopTmpl<mediakit::RtspSession>;
    std::shared_ptr<RtcServer> newRtcServer(int port);
    std::shared_ptr<SrtServer> newSrtServer(int port);
    std::shared_ptr<RtmpServer> newRtmpServer(int port);
    std::shared_ptr<RtspServer> newRtspServer(int port);
private:
    std::shared_ptr<RtcServer> _udpRtc;
    std::shared_ptr<SrtServer> _udpSrt;
    std::shared_ptr<RtmpServer> _rtmp;
    std::shared_ptr<RtspServer> _rtsp;

    websocket_server_t _http;
    void startHttp();

    void onRtcOfferReq(const HttpRequestPtr& req, const HttpResponseWriterPtr& writer);

    void addItem(const std::string &key, const WebRtcTransportPtr &ptr) {
        std::lock_guard<std::mutex> lck(_mtx);
        _map[key] = ptr;
    }
    void removeItem(const std::string &key) {
        std::lock_guard<std::mutex> lck(_mtx);
        _map.erase(key);
    }
    mutable std::mutex _mtx;
    std::unordered_map<std::string, std::weak_ptr<WebRtcTransportImp> > _map;

    std::string _local_ip;
    mutable std::mutex _mtx_creator;
    std::unordered_map<std::string, Plugin> _map_creator;
};


#endif //ZLMEDIAKIT_WEBRTCSERVER_H
