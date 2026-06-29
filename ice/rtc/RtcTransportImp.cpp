#include "config.h"
#include "onceToken.h"
#include "Stamp.h"
#include "rtp/Rtcp.h"
#include "rtp/RtpJitter.h"
#include "RtcTransportImp.hpp"
#include "rtp/RtcpContext.h"
#include "rtp/TwccContext.h"
#include "rtp/Nack.h"

using namespace std;
namespace ice {
// RTC配置项
namespace Rtc {
#define RTC_FIELD "rtc."
// rtp and rtcp receive timeout
const string kTimeOutSec = RTC_FIELD "timeoutSec";
// server’s public ip
const string kExternIP = RTC_FIELD "externIP";
// 设置remb比特率bps，非0时关闭twcc并开启remb。
// 该设置在rtc推流时有效，可以控制推流画质
const string kRembBitRate = RTC_FIELD "rembBitRate";
const string kRembFreq = RTC_FIELD "rembFreq";
// 带关键帧请求时视频Nack优化
const string kUseNackGenerator = RTC_FIELD "use_nack_generator";
const string kUseRtpJitter = RTC_FIELD "use_rtp_jitter";
// webrtc udp服务端口
const string kPort = RTC_FIELD "port";
const string kTcpPort = RTC_FIELD "tcpPort";

// Bitrate setting
const string kStartBitrate = RTC_FIELD "start_bitrate";
const string kMaxBitrate = RTC_FIELD "max_bitrate";
const string kMinBitrate = RTC_FIELD "min_bitrate";

// Data channel setting
const string kDataChannelEcho = RTC_FIELD "datachannel_echo";
// 发送nack的延迟时间，单位ms；发现丢包后延迟ms发送nack包
const string kNackDelayMS = RTC_FIELD "nackDelayMS";
// 关键帧距，单位ms，之前是写死的2000，现改成可配置的
const string kMaxKeyFrameMS = RTC_FIELD "max_keyframe_ms";
const string kMinKeyFrameMS = RTC_FIELD "min_keyframe_ms";

static onceToken token([]() {
    mINI::Instance()[kMaxKeyFrameMS] = 2000;
    mINI::Instance()[kMinKeyFrameMS] = 1000;
    mINI::Instance()[kRembFreq] = 2000;
    mINI::Instance()[kNackDelayMS] = 0u;
    mINI::Instance()[kUseNackGenerator] = 1;
    mINI::Instance()[kUseRtpJitter] = 1;

    mINI::Instance()[kTimeOutSec] = 15;
    mINI::Instance()[kExternIP] = "";

    mINI::Instance()[kPort] = 8000;
    mINI::Instance()[kTcpPort] = 8000;

    mINI::Instance()[kRembBitRate] = 0;
    mINI::Instance()[kStartBitrate] = 0;
    mINI::Instance()[kMaxBitrate] = 0;
    mINI::Instance()[kMinBitrate] = 0;

    mINI::Instance()[kDataChannelEcho] = true;
});

} // namespace Rtc

//////////////////////////////////////////////////////////////////
// WebRtcTransportImp
void WebRtcTransportImp::start() {
    timeout_sec_ = mINI::Instance()[Rtc::kTimeOutSec];
    WebRtcTransport::start();
    _twcc_ctx = std::make_shared<TwccContext>();
    _twcc_ctx->setOnSendTwccCB([this](uint32_t ssrc, std::string fci) {onSendTwcc(ssrc, fci);});
}

void WebRtcTransportImp::onClose() {
    _ssrc_to_track.clear();
    _pt_to_track.clear();
    for (auto &it : _type_to_track) {
        it.reset();     
    }
}

bool WebRtcTransportImp::canSendRtp(const RtcMedia &m) const {
    return (getRole() == Role::PEER && m.direction == RtpDirection::sendonly) ||
           (getRole() == Role::CLIENT && m.direction == RtpDirection::recvonly) || (m.direction == RtpDirection::sendrecv);
}

bool WebRtcTransportImp::canRecvRtp(const RtcMedia &m) const {
    return (getRole() == Role::PEER && m.direction == RtpDirection::recvonly) ||
           (getRole() == Role::CLIENT && m.direction == RtpDirection::sendonly) || (m.direction == RtpDirection::sendrecv);
}

bool WebRtcTransportImp::canSendRtp() const {
    if (!_answer_sdp) {
        return false;
    }
    for (auto &m : _answer_sdp->media) {
        if (canSendRtp(m)) {
            return true;
        }
    }
    return false;
}

bool WebRtcTransportImp::canRecvRtp() const {
    if (!_answer_sdp) {
        return false;
    }
    for (auto &m : _answer_sdp->media) {
        if (canRecvRtp(m)) {
            return true;
        }
    }
    return false;
}


class RtpChannel : public std::enable_shared_from_this<RtpChannel>, public RtpJitter {
public:
    RtpChannel(hv::EventLoopPtr poller, uint32_t ssrc) {
        ssrc_ = ssrc;
        _poller = std::move(poller);

        // Set jitter buffer parameters
        GET_CONFIG(uint32_t, nack_maxms, Rtc::kNackMaxMS);
        GET_CONFIG(uint32_t, nack_max_rtp, Rtc::kNackMaxSize);
        GET_CONFIG(float, nack_ratio, Rtc::kNackIntervalRatio);
        GET_CONFIG(unsigned int, nackDelay, Rtc::kNackDelayMS);
        GET_CONFIG(uint32_t, nack_retry, Rtc::kNackMaxCount);
        setParams(nack_max_rtp, nack_maxms, nack_retry, nack_ratio, nackDelay);
    }
    
    void inputRtp(RtpPacket::Ptr rtp, bool is_rtx) {
        updateStamp(rtp.get());
        // input rtp and sort
        input(rtp, is_rtx);

        if (_on_req_key && rtp->getType() == TrackVideo) {
            RequestKeyFrame(true);
        }
        if (!is_rtx) {
            // 统计rtp接收情况，便于生成rtcp sr
            _rtcp_context.onRtp(rtp->getSeq(), rtp->getTimestamp(), rtp->ntp_stamp, rtp->sample_rate, rtp->size());
        }
    }

    void setOnReqKeyFrame(function<void()> cb) {
        _on_req_key = std::move(cb);
        enableKeyframeReq(_on_req_key!=nullptr);
    }

    void RequestKeyFrame(bool max) {
        GET_CONFIG(int, keyframeMin, Rtc::kMinKeyFrameMS);
        GET_CONFIG(int, keyframeMax, Rtc::kMaxKeyFrameMS);
        if (_pli_ticker.elapsedTime() > (max ? keyframeMax : keyframeMin)) {
            _pli_ticker.resetTime();
            if (_on_req_key) _on_req_key();
        }
    }

    void onRtcp(RtcpHeader *rtcp) {
        _rtcp_context.onRtcp(rtcp);
    }
    BufferPtr createRtcpRR(uint32_t ssrc) {
        return _rtcp_context.createRtcpRR(ssrc, getSSRC());
    }

    float getLossRate() {
        auto expected = _rtcp_context.getExpectedPacketsInterval();
        if (!expected) {
            return -1;
        }
        return _rtcp_context.getLostInterval() * 100 / expected;
    }

    const RtcpContextForRecv &rtcpContext() { return _rtcp_context; }

    int setNtpStamp(uint32_t rtp_stamp, uint64_t ntp_stamp_ms) {
        int ret = 0;
        _disable_ntp = rtp_stamp == 0 && ntp_stamp_ms == 0;
        if (!_disable_ntp) {
            ret = _ntp_stamp.setNtpStamp(rtp_stamp, ntp_stamp_ms);
        }
        return ret;
    }

    // a=rtcp-xr:rrtr
    BufferPtr createRrtr() {
        if (!enable_rrtr) return nullptr;
        while (_dlrr_map.size() > 16) _dlrr_map.erase(_dlrr_map.begin());
        auto now = gettick_ms();
        auto rrtr = RtcpXRRRTR::create(getSSRC(), now);
        uint32_t key = ((ntohl(rrtr->ntpmsw) & 0xFFFF) << 16) | ((ntohl(rrtr->ntplsw) >> 16) & 0xFFFF);
        _dlrr_map[key] = now;
        return RtcpHeader::toBuffer(rrtr);
    }

    void onGotDlrrItem(RtcpXRDLRRReportItem *item) {
        auto it = _dlrr_map.find(item->lrr);
        if (it != _dlrr_map.end()) {
            auto rtt = gettick_ms() - it->second - item->dlrr * 1000 / 65535;
            UpdateRTT(rtt);
            _dlrr_map.erase(it);
        }
    }

    void enableRTTR(bool v) { enable_rrtr = v; }

    void setOnNack(function<void(const FCI_NACK &nack)> on_nack) { _on_nack = std::move(on_nack); }

protected:
    bool enable_rrtr = false;
    std::map<uint32_t, uint64_t> _dlrr_map;

    void updateStamp(RtpPacket *rtp) {
        if (_disable_ntp) {
            // Does not support NTP timestamp, such as national standard streaming, so directly use RTP timestamp
            rtp->ntp_stamp = rtp->getTimestamp() * uint64_t(1000) / rtp->sample_rate;
        }
        else {
            // Set NTP timestamp
            rtp->ntp_stamp = _ntp_stamp.getNtpStamp(rtp->getTimestamp(), rtp->sample_rate, rtp->getSSRC());
        }
    }
    bool _disable_ntp = false;
    NtpStamp _ntp_stamp;

    RtcpContextForRecv _rtcp_context;
    // keyframe req
    std::function<void()> _on_req_key;
    // pli rtcp timer
    Ticker _pli_ticker;

private:
    void onRequestKeyframe() override { RequestKeyFrame(false); }
    void onLostPacket(const LostList &nack_rtp) override {
        NackContext::sendNack(nack_rtp, _on_nack);
    }
private:
    hv::EventLoopPtr _poller;
    std::function<void(const FCI_NACK &nack)> _on_nack;
};

std::shared_ptr<RtpChannel> MediaTrack::getRtpChannel(uint32_t ssrc) const {
    auto it_chn = rtp_channel.find(rtp_ext_ctx->getRid(ssrc));
    if (it_chn == rtp_channel.end()) {
        return nullptr;
    }
    return it_chn->second;
}

void WrappedRtpTrack::inputRtp(RtpPacket::Ptr rtp, uint64_t stamp_ms) {
#if 0
    auto seq = rtc->getSeq();
    if (track->media->type == TrackVideo && seq % 100 == 0) {
        // Simulate packet lost
        return;
    }
#endif

    auto ssrc = rtp->getSSRC();

    // 修改和统一ext id
    auto it = rtp->extMap.find((int)RtpExtType::transport_cc);
    if (it != rtp->extMap.end()) {
        _twcc_ctx.onRtp(ssrc, it->second.getTransportCCSeq(), stamp_ms);
    }

    auto &ref = track->rtp_channel[rtp->rid];
    if (!ref) {
        _transport.createRtpChannel(rtp->rid, ssrc, *track);
    }

    // Parse and sort rtp
    ref->inputRtp(rtp, false);
}

void WrappedRtxTrack::inputRtp(RtpPacket::Ptr rtp, uint64_t stamp_ms) {
    // 修改和统一ext id
    auto &ref = track->rtp_channel[rtp->rid];
    if (!ref) {
        // Discard rtx packets before receiving the corresponding rtp
        hlogw("unknown rtx rtp, rid: %s, codec: %s", rtp->rid.c_str(), track->plan_rtp->codec.c_str());
        return;
    }

    // This is the rtx retransmission packet
    //  https://datatracker.ietf.org/doc/html/rfc4588#section-4
    auto payload = rtp->getPayload();
    auto size = rtp->getPayloadSize();
    if (size < 2) {
        return;
    }
    // track->rtx++;
    // The first two bytes are the original rtp seq
#if 1    
    rtp->RtxDecode(track->plan_rtp->pt, ref->getSSRC());
#else
    auto origin_seq = payload[0] << 8 | payload[1];
    auto rtphdr = rtp->getHeader();
    // converted rtx to rtp
    rtphdr->pt = track->plan_rtp->pt;
    rtphdr->seq = htons(origin_seq);
    rtphdr->ssrc = htonl(ref->getSSRC());

    memmove(payload, payload + 2, size - 2);
    rtp->setSize(rtp->size() - 2);
#endif
    ref->inputRtp(rtp, true);
}

MediaTrack::Ptr WebRtcTransportImp::getTrack(TrackType type) const {
    for (auto it : _ssrc_to_track) {
        if (it.second->getTrackType() == type) {
            return it.second;
        }
    }
    return nullptr;
}

void WebRtcTransportImp::onStartWebRTC() {
    // 获取ssrc和pt相关信息,届时收到rtp和rtcp时分别可以根据pt和ssrc找到相关的信息
    for (auto &m_answer : _answer_sdp->media) {
        if (m_answer.type == TrackApplication) {
            continue;
        }
        auto m_offer = _offer_sdp->getMedia(m_answer.type);
        auto track = std::make_shared<MediaTrack>();

        track->media = &m_answer;
        track->answer_ssrc_rtp = m_answer.getRtpSSRC();
        track->answer_ssrc_rtx = m_answer.getRtxSSRC();
        track->offer_ssrc_rtp = m_offer->getRtpSSRC();
        track->offer_ssrc_rtx = m_offer->getRtxSSRC();
        track->plan_rtp = &m_answer.plan[0];
        track->plan_rtx = m_answer.getRelatedRtxPlan(track->plan_rtp->pt);
        track->rtcp_context_send = std::make_shared<RtcpContextForSend>();
        track->nack_list = std::make_shared<NackList>();

        // rtp track type --> MediaTrack
        if (canSendRtp(m_answer)) {
            // 该类型的track 才支持发送
            _type_to_track[m_answer.type] = track;
        }
        // send ssrc --> MediaTrack
        _ssrc_to_track[track->answer_ssrc_rtp] = track;
        _ssrc_to_track[track->answer_ssrc_rtx] = track;

        // recv ssrc --> MediaTrack
        _ssrc_to_track[track->offer_ssrc_rtp] = track;
        _ssrc_to_track[track->offer_ssrc_rtx] = track;

        // rtp pt --> MediaTrack
        _pt_to_track.emplace(track->plan_rtp->pt, std::unique_ptr<WrappedMediaTrack>(new WrappedRtpTrack(track, *_twcc_ctx, *this)));
        if (track->plan_rtx) {
            // rtx pt --> MediaTrack
            _pt_to_track.emplace(track->plan_rtx->pt, std::unique_ptr<WrappedMediaTrack>(new WrappedRtxTrack(track)));
        }
        // 记录rtp ext类型与id的关系，方便接收或发送rtp时修改rtp ext id
        track->rtp_ext_ctx = std::make_shared<RtpExtContext>(m_answer);
        weak_ptr<MediaTrack> weak_track = track;
        track->rtp_ext_ctx->setOnGetRtp([this, weak_track](uint8_t pt, uint32_t ssrc, const string &rid) {
            // ssrc --> MediaTrack
            auto track = weak_track.lock();
            assert(track);
            _ssrc_to_track[ssrc] = std::move(track);
            hlogi("get rtp, pt: %d, ssrc: %u, rid: %s", (int)pt, ssrc, rid.c_str());
        });

        size_t index = 0;
        for (auto &ssrc : m_offer->rtp_ssrc_sim) {
            // 记录ssrc对应的MediaTrack
            _ssrc_to_track[ssrc.ssrc] = track;
            if (m_offer->rtp_rids.size() > index) {
                // 支持firefox的simulcast, 提前映射好ssrc和rid的关系
                track->rtp_ext_ctx->setRid(ssrc.ssrc, m_offer->rtp_rids[index]);
            }
            else {
                // SDP munging没有rid, 它通过group-ssrc:SIM给出ssrc列表;
                // 系统又要有rid，这里手工生成rid，并为其绑定ssrc
                std::string rid = "r" + std::to_string(index);
                track->rtp_ext_ctx->setRid(ssrc.ssrc, rid);
                if (ssrc.rtx_ssrc) {
                    track->rtp_ext_ctx->setRid(ssrc.rtx_ssrc, rid);
                }
            }
            ++index;
        }
    }
}


bool WebRtcTransportImp::sendFrame(const Frame::Ptr &frame) {
    auto track = _type_to_track[frame->getTrackType()];
    if (!track) return false;
    auto pkts = frame->splitToRtp(track->plan_rtp->pt, track->answer_ssrc_rtp, track->seq);
    hlogd("%s sendFrame %d rtp frame, %s", getIdentifier(), pkts.size(), frame->toString().c_str());
    for (auto pkt : pkts) {
        onSendRtp(pkt, false);
    }
    return true;
}

void WebRtcTransportImp::createRtpChannel(const std::string &rid, uint32_t ssrc, MediaTrack &track) {
    // rid --> RtpReceiverImp
    auto &ref = track.rtp_channel[rid];
    weak_ptr<WebRtcTransportImp> weak_self = static_pointer_cast<WebRtcTransportImp>(shared_from_this());
    ref = std::make_shared<RtpChannel>(loop(), ssrc);
    ref->setOnFrame([&track, this, rid](Frame::Ptr rtp) mutable { onRecvFrame(track, rid, std::move(rtp)); });
    //ref->setOnSort([&track, this, rid](RtpPacket::Ptr rtp) mutable { onSortedRtp(track, rid, std::move(rtp)); });
    if (track.media->type == TrackVideo) {
        ref->setOnReqKeyFrame([this, ssrc]() { sendRtcpPli(ssrc); });
    }
    ref->setOnNack([&track, weak_self, ssrc](const FCI_NACK &nack) mutable {
        // nack发送可能由定时器异步触发
        if (auto strong_self = weak_self.lock()) {
            strong_self->onSendNack(track, nack, ssrc);
        }
    });
    hlogi("create rtp receiver of ssrc: %u, rid: %s, codec: %s", ssrc, rid.c_str(), track.plan_rtp->codec.c_str());
}

void WebRtcTransportImp::onRtcp(const char *buf, size_t len) {
    _bytes_usage += len;
    auto rtcps = RtcpHeader::loadFromBytes((char *)buf, len);
    for (auto rtcp : rtcps) {
        switch ((RtcpType)rtcp->pt) {
        case RtcpType::RTCP_SR: {
            // 对方汇报rtp发送情况
            RtcpSR *sr = (RtcpSR *)rtcp;
            auto it = _ssrc_to_track.find(sr->ssrc);
            if (it == _ssrc_to_track.end()) {
                hlogw("未识别的sr rtcp包: %s", rtcp->dumpString().c_str());
                break;
            }

            auto &track = it->second;
            auto rtp_chn = track->getRtpChannel(sr->ssrc);
            if (!rtp_chn) {
                hlogw("未识别的sr rtcp包: %s", rtcp->dumpString().c_str());
                break;
            }

            // 设置rtp时间戳与ntp时间戳的对应关系
            rtp_chn->setNtpStamp(sr->rtpts, sr->getNtpUnixStampMS());
            rtp_chn->onRtcp(sr);
            break;
        }
        case RtcpType::RTCP_RR: {
            // 对方汇报rtp接收情况
            RtcpRR *rr = (RtcpRR *)rtcp;
            for (auto item : rr->getItemList()) {
                auto it = _ssrc_to_track.find(item->ssrc);
                if (it != _ssrc_to_track.end()) {
                    auto &track = it->second;
                    track->rtcp_context_send->onRtcp(rtcp);
                }
                else {
                    hlogw("未识别的rr rtcp包: %s", rtcp->dumpString().c_str());
                }
            }
            break;
        }
        case RtcpType::RTCP_BYE: {
            // 对方汇报停止发送rtp
            RtcpBye *bye = (RtcpBye *)rtcp;
            for (auto ssrc : bye->getSSRC()) {
                auto it = _ssrc_to_track.find(*ssrc);
                if (it == _ssrc_to_track.end()) {
                    hlogw("未识别的bye rtcp包: %s", rtcp->dumpString().c_str());
                    continue;
                }
                _ssrc_to_track.erase(it);
            }
            onRtcpBye();
            // bye 会在 sender audio track mute 时出现, 因此不能作为 shutdown 的依据
            break;
        }
        case RtcpType::RTCP_PSFB:
            switch ((PSFBType)rtcp->report_count) { 
            case PSFBType::RTCP_PSFB_FIR:
            case PSFBType::RTCP_PSFB_PLI: {
                RtcpFB *fb = (RtcpFB *)rtcp;
                auto it = _ssrc_to_track.find(fb->ssrc_media);
                if (it == _ssrc_to_track.end()) {
                    hlogw("未识别的 rtcp包: %s", rtcp->dumpString().c_str());
                    break;
                }
                hlogi("onKeyFrameReq: %s, ssrc: %u", getIdentifier(), fb->ssrc_media);
                onKeyFrameReq(*it->second, fb->ssrc_media);
                break;
            }
            default: break;
            }
            break;
        case RtcpType::RTCP_RTPFB: {
            // RTPFB
            switch ((RTPFBType)rtcp->report_count) {
            case RTPFBType::RTCP_RTPFB_NACK: {
                RtcpFB *fb = (RtcpFB *)rtcp;
                auto it = _ssrc_to_track.find(fb->ssrc_media);
                if (it == _ssrc_to_track.end()) {
                    hlogw("未识别的 rtcp包: %s", rtcp->dumpString().c_str());
                    break;
                }
                auto &track = it->second;
                auto &fci = fb->getFci<FCI_NACK>();
                track->nack_list->forEach(fci, [&](const RtpPacket::Ptr &rtp) {
                    // rtp retransmission
                    onSendRtp(rtp, true);
                });
                break;
            }
            default: break;
            }
            break;
        }
        case RtcpType::RTCP_XR: {
            RtcpXRRRTR *xr = (RtcpXRRRTR *)rtcp;
            if (xr->bt != 4) {
                break;
            }
            auto it = _ssrc_to_track.find(xr->ssrc);
            if (it == _ssrc_to_track.end()) {
                hlogw("未识别的 rtcp包: %s", rtcp->dumpString().c_str());
                break;
            }
            auto &track = it->second;
            track->rtcp_context_send->onRtcp(rtcp);
            auto xrdlrr = track->rtcp_context_send->createRtcpXRDLRR(track->answer_ssrc_rtp, track->answer_ssrc_rtp);
            sendRtcp(xrdlrr->data(), xrdlrr->size());
            break;
        }
        default: break;
        }
    }
}

void WebRtcTransportImp::onRtp(const char *buf, size_t len, uint64_t stamp_ms) {
    _bytes_usage += len;

    uint8_t pt = ((RtpHeader *)buf)->pt;
    // 根据接收到的rtp的pt信息，找到该流的信息
    auto it = _pt_to_track.find(pt);
    if (it == _pt_to_track.end()) {
        hlogw("unknown rtp pt: %d", pt);
        return;
    }

    std::shared_ptr<RtpPacket> rtp = std::make_shared<RtpPacket>();
    if (rtp->parse((const uint8_t *)buf, len)) {
        auto track = it->second->track;
        rtp->sample_rate = track->plan_rtp->sample_rate;
        rtp->codec = getCodecId(track->plan_rtp->codec);
        track->rtp_ext_ctx->parseRtpExtId(rtp.get());
        it->second->inputRtp(rtp, stamp_ms);
    }

    // send rr
    if (_rtcp_rr_send_ticker.elapsedTime() > 5000) {
        _rtcp_rr_send_ticker.resetTime();
        for (auto &it : _ssrc_to_track) {
            auto ssrc = it.first;
            auto &track = it.second;
            auto rtp_chn = track->getRtpChannel(ssrc);
            if (rtp_chn) {
                auto rr = rtp_chn->createRtcpRR(track->answer_ssrc_rtp);
                if (rr && rr->size() > 0) {
                    sendRtcp(rr->data(), rr->size());
                }
            }
        }
        // 开启remb，则发送remb包调节比特率
        if (_remb_bitrate && _answer_sdp->supportRtcpFb(SdpConst::kRembRtcpFb)) {
            sendRtcpRemb(rtp->getSSRC(), _remb_bitrate);
        }
    }
}

void WebRtcTransportImp::onSendNack(MediaTrack &track, const FCI_NACK &nack, uint32_t ssrc) {
    auto rtcp = RtcpFB::create(RTPFBType::RTCP_RTPFB_NACK, &nack, FCI_NACK::kSize);
    rtcp->ssrc = htonl(track.answer_ssrc_rtp);
    rtcp->ssrc_media = htonl(ssrc);
    sendRtcp(rtcp.get(), rtcp->getSize());
}

void WebRtcTransportImp::onSendTwcc(uint32_t ssrc, const string &twcc_fci) {
    auto rtcp = RtcpFB::create(RTPFBType::RTCP_RTPFB_TWCC, twcc_fci.data(), twcc_fci.size());
    rtcp->ssrc = htonl(0);
    rtcp->ssrc_media = htonl(ssrc);
    sendRtcp(rtcp.get(), rtcp->getSize());
}

void WebRtcTransportImp::sendRtcpRemb(uint32_t ssrc, size_t bit_rate) {
    auto remb = FCI_REMB::create({ssrc}, (uint32_t)bit_rate);
    auto fb = RtcpFB::create(PSFBType::RTCP_PSFB_REMB, remb.data(), remb.size());
    fb->ssrc = htonl(0);
    fb->ssrc_media = htonl(ssrc);
    sendRtcp(fb.get(), fb->getSize());
}

void WebRtcTransportImp::sendRtcpPli(uint32_t ssrc) {
    auto pli = RtcpFB::create(PSFBType::RTCP_PSFB_PLI);
    pli->ssrc = htonl(0);
    pli->ssrc_media = htonl(ssrc);
    sendRtcp(pli.get(), pli->getSize());
}
///////////////////////////////////////////////////////////////////

void WebRtcTransportImp::onSortedRtp(MediaTrack &track, const string &rid, RtpPacket::Ptr rtp) {
    //onRecvRtp(track, rid, std::move(rtp));
}

///////////////////////////////////////////////////////////////////

void WebRtcTransportImp::onSendRtp(const RtpPacket::Ptr &rtp, bool rtx) {
    auto &track = _type_to_track[rtp->getType()];
    if (!track) {
        // 忽略，对方不支持该编码类型
        return;
    }
    if (!rtx) {
        // 统计rtp发送情况，好做sr汇报
        track->rtcp_context_send->onRtp(rtp->getSeq(), rtp->getTimestamp(), rtp->ntp_stamp, rtp->sample_rate, rtp->size());
        track->nack_list->pushBack(rtp);
#if 0
        // 此处模拟发送丢包
        if (rtp->type == TrackVideo && rtp->getSeq() % 100 == 0) {
            return;
        }
#endif
    }
    else {
        // 发送rtx重传包
        // TraceL << "send rtx rtp:" << rtp->getSeq();
    }
    pair<bool /*rtx*/, MediaTrack *> ctx{rtx, track.get()};
    sendRtp(rtp->data(), rtp->size(), &ctx);
    _bytes_usage += rtp->size();

    // send sr
    if (track->rtcp_context_send && _rtcp_sr_send_ticker.elapsedTime() > 5000) {
        _rtcp_sr_send_ticker.resetTime();
        auto sr = track->rtcp_context_send->createRtcpSR(track->answer_ssrc_rtp);
        if (sr && sr->size() > 0) {
            sendRtcp(sr->data(), sr->size());
        }
    }
}

void WebRtcTransportImp::onBeforeEncryptRtp(const char *buf, int &len, void *ctx) {
    if (!ctx) return ;
    auto pr = (pair<bool /*rtx*/, MediaTrack *> *)ctx;
    auto header = (RtpHeader *)buf;

    if (!pr->first || !pr->second->plan_rtx) {
        // 普通的rtp,或者不支持rtx, 修改目标pt和ssrc
        pr->second->rtp_ext_ctx->changeRtpExtId(header);
        header->pt = pr->second->plan_rtp->pt;
        header->ssrc = htonl(pr->second->answer_ssrc_rtp);
    }
    else {
        // 重传的rtp, rtx
        pr->second->rtp_ext_ctx->changeRtpExtId(header);
        header->pt = pr->second->plan_rtx->pt;
        if (pr->second->answer_ssrc_rtx) {
            // 有rtx单独的ssrc,有些情况下，浏览器支持rtx，但是未指定rtx单独的ssrc
            header->ssrc = htonl(pr->second->answer_ssrc_rtx);
        }
        else {
            // 未单独指定rtx的ssrc，那么使用rtp的ssrc
            header->ssrc = htonl(pr->second->answer_ssrc_rtp);
        }

        auto origin_seq = ntohs(header->seq);
        // seq跟原来的不一样
        header->seq = htons(pr->second->rtx_seq++);

        auto payload = header->getPayload();
        auto payload_size = header->getPayloadSize(len);
        if (payload_size) {
            // rtp负载后移两个字节，这两个字节用于存放osn
            // https://datatracker.ietf.org/doc/html/rfc4588#section-4
            memmove(payload + 2, payload, payload_size);
        }
        payload[0] = origin_seq >> 8;
        payload[1] = origin_seq & 0xFF;
        len += 2;
    }
}

#define RTP_SSRC_OFFSET 1
#define RTX_SSRC_OFFSET 2
#define RTP_CNAME "zlmediakit-rtp"
#define RTP_LABEL "zlmediakit-label"
#define RTP_MSLABEL "zlmediakit-mslabel"

 void WebRtcTransportImp::onCheckSdp(SdpType type, RtcSession &sdp) {
    if (type != SdpType::answer) return;
    /*
    auto extern_ips = GetExternIPS();
    // 修改answer sdp的ip、端口信息
    for (auto &m : sdp.media) {
        m.addr.reset();
        m.addr.address = extern_ips.empty() ? _local_ip.empty() ? SockUtil::get_local_ip() : _local_ip : extern_ips[0];
        m.rtcp_addr.reset();
        m.rtcp_addr.address = m.addr.address;

        GET_CONFIG(uint16_t, udp_port, Rtc::kPort);
        GET_CONFIG(uint16_t, tcp_port, Rtc::kTcpPort);
        m.port = m.port ? (udp_port ? udp_port : tcp_port) : 0;
        if (m.type != TrackApplication) {
            m.rtcp_addr.port = m.port;
        }
        sdp.origin.address = m.addr.address;
    }
    */
    if (!canSendRtp()) {
        return;
    }

    for (auto &m : sdp.media) {
        if (m.type == TrackApplication) {
            continue;
        }
        if (!m.rtp_rtx_ssrc.empty()) {
            // The ssrc has been generated
            continue;
        }
        // Add the ssrc information to answer sdp
        m.rtp_rtx_ssrc.emplace_back();
        auto &ssrc = m.rtp_rtx_ssrc.back();
        // 作为客户端库，使用offer sdp中的ssrc
        if (_role == Role::CLIENT && _offer_sdp) {
            auto offer = _offer_sdp->getMedia(m.type);
            if (offer->rtp_rtx_ssrc.size()) {
                ssrc = offer->rtp_rtx_ssrc[0];
            }
        }
        else {
            // 发送的ssrc我们随便定义，因为发送rtp时会修改为此值
            // We can define the ssrc we send at will, because it will be modified to this value when sending rtp
            ssrc.ssrc = m.type + RTP_SSRC_OFFSET;
            ssrc.cname = RTP_CNAME;
            ssrc.label = std::string(RTP_LABEL) + '-' + m.mid;
            ssrc.mslabel = RTP_MSLABEL;
            ssrc.msid = ssrc.mslabel + ' ' + ssrc.label;

            if (m.getRelatedRtxPlan(m.plan[0].pt)) {
                // rtx ssrc
                ssrc.rtx_ssrc = ssrc.ssrc + RTX_SSRC_OFFSET;
            }
        }
    }
}

} // namespace ice