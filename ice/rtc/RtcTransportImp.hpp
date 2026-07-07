#include <memory>
#include <functional>
#include <unordered_map>
#include "WebRtcTransport.hpp"
#include "Frame.h"
#include "rtp/RtpPacket.h"

namespace ice {
class RtpChannel;
class RtcpContext;
class TwccContext;
class NackList;
struct FCI_NACK;

class MediaTrack {
public:
    using Ptr = std::shared_ptr<MediaTrack>;
    const RtcCodecPlan *plan_rtp;
    const RtcCodecPlan *plan_rtx;
    uint32_t offer_ssrc_rtp = 0;
    uint32_t offer_ssrc_rtx = 0;
    uint32_t answer_ssrc_rtp = 0;
    uint32_t answer_ssrc_rtx = 0;
    uint16_t seq = 0, rtx_seq = 0;
    const RtcMedia *media;
    RtpExtContext::Ptr rtp_ext_ctx;
    CodecId getCodec() const { return plan_rtp ? getCodecId(plan_rtp->codec) : CodecId::CodecInvalid; }
    TrackType getTrackType() const { return media ? media->type : TrackInvalid; }
    //for send rtp
    std::shared_ptr<NackList> nack_list;
    std::shared_ptr<RtcpContext> rtcp_context_send;

    //for recv rtp
    std::unordered_map<std::string/*rid*/, std::shared_ptr<RtpChannel> > rtp_channel;
    std::shared_ptr<RtpChannel> getRtpChannel(uint32_t ssrc) const;
};

struct WrappedMediaTrack {
    MediaTrack::Ptr track;
    explicit WrappedMediaTrack(MediaTrack::Ptr ptr): track(std::move(ptr)) {}
    virtual ~WrappedMediaTrack() {}
    virtual void inputRtp(RtpPacket::Ptr rtp, uint64_t stamp_ms) = 0;
};

struct WrappedRtxTrack: public WrappedMediaTrack {
    explicit WrappedRtxTrack(MediaTrack::Ptr ptr)
        : WrappedMediaTrack(std::move(ptr)) {}
    void inputRtp(RtpPacket::Ptr rtp, uint64_t stamp_ms) override;
};

class WebRtcTransportImp;

struct WrappedRtpTrack : public WrappedMediaTrack {
    explicit WrappedRtpTrack(MediaTrack::Ptr ptr, TwccContext& twcc, WebRtcTransportImp& t)
        : WrappedMediaTrack(std::move(ptr))
        , _twcc_ctx(twcc)
        , _transport(t) {}
    TwccContext& _twcc_ctx;
    WebRtcTransportImp& _transport;
    void inputRtp(RtpPacket::Ptr rtp, uint64_t stamp_ms) override;
};

// 封装rtp收发、rtp帧打包和解包和track识别，对外提供Frame发送和接收接口
class WebRtcTransportImp : public WebRtcTransport, public FrameWriterInterface {
public:
    using Ptr = std::shared_ptr<WebRtcTransportImp>;
    WebRtcTransportImp(const IceConfig* options, IceAgent* agent = nullptr) : WebRtcTransport(options, agent) {}

    bool canSendRtp() const;
    bool canRecvRtp() const;
    bool canSendRtp(const RtcMedia& media) const;
    bool canRecvRtp(const RtcMedia& media) const;
    // 发送帧数据
    bool inputFrame(const Frame::Ptr &frame) override {return sendFrame(frame);}
    bool sendFrame(const Frame::Ptr &frame);
    // 帧接收回调
    using FrameCallback = std::function<void(Frame::Ptr)>;
    FrameCallback onFrame;
    virtual void onRecvFrame(MediaTrack &track, const std::string &rid, Frame::Ptr rtp) {
        if (onFrame) {
            onFrame(rtp);
        }
    }
    // 关键帧请求回调
    using KeyFrameReqCallback = std::function<void(uint32_t)>;
    KeyFrameReqCallback onKeyFrame;
    virtual void onKeyFrameReq(MediaTrack &track, uint32_t ssrc) {
        if (onKeyFrame) {
            onKeyFrame(ssrc);
        }
    }

    friend class WrappedRtpTrack;
    MediaTrack::Ptr getTrack(TrackType type) const;

    void start() override;
protected:
    void onClose() override;

    void onStartWebRTC() override;
    void onCheckSdp(SdpType type, RtcSession &sdp) override;

    void onRtp(const char *buf, size_t len, uint64_t stamp_ms) override;
    void onRtcp(const char *buf, size_t len) override;
    void onBeforeEncryptRtp(const char *buf, int &len, void *ctx) override;

    void onSendNack(MediaTrack &track, const FCI_NACK &nack, uint32_t ssrc);
    void onSendTwcc(uint32_t ssrc, const std::string &twcc_fci);
    void onSortedRtp(MediaTrack &track, const std::string &rid, RtpPacket::Ptr rtp);
    void onSendRtp(const RtpPacket::Ptr &rtp, bool rtx);

    void createRtpChannel(const std::string &rid, uint32_t ssrc, MediaTrack &track);

    void sendRtcpRemb(uint32_t ssrc, size_t bit_rate);
    void sendRtcpPli(uint32_t ssrc);

    uint64_t _bytes_usage = 0;
    uint32_t _remb_bitrate = 0;

    Ticker _rtcp_sr_send_ticker;
    Ticker _rtcp_rr_send_ticker;

    // twcc rtcp发送上下文对象
    std::shared_ptr<TwccContext> _twcc_ctx;
    // 根据发送rtp的track类型获取相关信息
    MediaTrack::Ptr _type_to_track[2];
    // 根据rtcp的ssrc获取相关信息，收发rtp和rtx的ssrc都会记录
    std::unordered_map<uint32_t/*ssrc*/, MediaTrack::Ptr> _ssrc_to_track;
    // 根据接收rtp的pt获取相关信息
    std::unordered_map<uint8_t/*pt*/, std::unique_ptr<WrappedMediaTrack>> _pt_to_track;
};
}