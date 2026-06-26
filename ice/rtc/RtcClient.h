#ifndef __WHEP_CLIENT_H__
#define __WHEP_CLIENT_H__

#include "hv/requests.h"
#include "RtcTransportImp.hpp"

namespace ice {
class RtcClient : public WebRtcTransportImp {
    std::string url_;
public:
    RtcClient(const IceConfig* options) : WebRtcTransportImp(options) {
        setRole(Role::CLIENT);
    }

    const char* url() const { return url_.c_str(); }
    bool open(const char* url) {
        if (!url || !url[0]) return false;
        url_ = url;
        auto offer = createOffer();
        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_POST;
        req->url = url;
        req->headers["Content-Type"] = "application/sdp";
        req->body = offer;
        std::weak_ptr<WebRtcTransport> weak_self = shared_from_this();
        requests::async(req, [weak_self](const HttpResponsePtr& res) {
            auto self = weak_self.lock();
            if (!self) return;
            if (res && res->status_code == 200) {
                // Process the answer SDP
                self->setRemoteDescription(SdpType::answer, res->body);
            }
            else{
                //self->onError(res->status_code, res->body);
            }
        });
        return true;
    }
};

class WhepClient : public RtcClient {
public:
    WhepClient(const IceConfig* options) : RtcClient(options) {}
    void onRtcConfigure(RtcConfigure &configure) const override {
        RtcClient::onRtcConfigure(configure);
        configure.audio.direction = configure.video.direction = RtpDirection::recvonly;
    }
};

class WhipClient : public RtcClient {
protected:
    CodecId acodec_ = CodecInvalid;
    CodecId vcodec_ = CodecInvalid;
public:
    WhipClient(const IceConfig* options) : RtcClient(options) {}
    void setVideoCodec(CodecId c) { vcodec_ = c;}
    void setAudioCodec(CodecId c) { acodec_ = c; }
    void onRtcConfigure(RtcConfigure& configure) const override {
        RtcClient::onRtcConfigure(configure);
        configure.audio.direction = configure.video.direction = RtpDirection::sendonly;
        // configure.setPlayRtspInfo(sdp);
        configure.setPlayRtspInfo(acodec_, vcodec_);
    }
};

}
#endif
