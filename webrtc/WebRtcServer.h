/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */


#ifndef ZLMEDIAKIT_WEBRTCSESSION_H
#define ZLMEDIAKIT_WEBRTCSESSION_H

#include "UdpServer2.h"
#include "http/server/WebSocketServer.h"
class WebRtcInterface;
class WebRtcTransportImp;
typedef std::shared_ptr<WebRtcTransportImp> WebRtcTransportPtr;

class WebRtcSession : public hv::SocketChannel, 
    public std::enable_shared_from_this<WebRtcSession> {
    std::string _identifier;
    bool _find_transport = true;
    struct sockaddr* _peer_addr;
    std::shared_ptr<WebRtcTransportImp> _transport;
public:
    void onRecv(hv::Buffer* buf);

    WebRtcSession(hio_t* io);
    std::string getIdentifier() const {
        return _identifier;
    }
};

class Session {};
using WebRtcArgs = std::map<std::string, std::string>;

class WebRtcServer {
public:
    friend class WebRtcTransportImp;

    WebRtcServer();
    ~WebRtcServer();

    void start();
    void stop();

    static WebRtcServer& Instance();
    hv::EventLoopPtr getPoller() { return _udp.nextLoop(nullptr); }

    using onCreateRtc = std::function<void(const WebRtcInterface &rtc)>;
    using Plugin = std::function<void(Session* sender, const std::string &offer, const WebRtcArgs &args, const onCreateRtc &cb)>;

    void registerPlugin(const std::string &type, Plugin cb);
    WebRtcTransportPtr getItem(const std::string &key);

    std::string get_local_ip() {
        if (_local_ip.empty())
            detect_local_ip();
        return _local_ip;
    }
    void detect_local_ip();
private:
    void startHttp();
    void onRtcOfferReq(const HttpRequestPtr& req, const HttpResponseWriterPtr& writer);

    void startUdp();
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
    hv::UdpServerTmpl2<WebRtcSession> _udp;
    websocket_server_t _http;
    mutable std::mutex _mtx_creator;
    std::unordered_map<std::string, Plugin> _map_creator;
};


#endif //ZLMEDIAKIT_WEBRTCSESSION_H
