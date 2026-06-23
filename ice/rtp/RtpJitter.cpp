#include "RtpJitter.h"
#include "hlog.h"
#include "htime.h"
#include "EventLoop.h"
using namespace ice;
#define MIN_RTT 20
#define MAX_RTT 500
///////////////////////////////////////////////////////////////
// RtpJitter
// 日志等级约定
// Trace 进出包
// Debug Nack调试
// Info，Warn 可能丢包
RtpJitter::~RtpJitter() {
    clear();
}

void RtpJitter::clear() {
    start_ = false;
    seq_map_.clear();
    key_seqs_.clear();
    clearFrame();
    if (timer_) {
        hv::killTimer(timer_);
        timer_ = 0;
    }
}

void RtpJitter::setParams(size_t max_size, size_t max_ms, int max_retry, float ratio, unsigned int delayMs) {
    max_size_ = max_size;
    max_ms_ = max_ms;
    max_retry_ = max_retry;
    nack_ratio_ = ratio;
    delay_ms_ = delayMs;
    if (max_distance_ > max_size_)
        max_distance_ = max_size_;
    hlogi("max_size=%d, max_ms=%d, max_dist=%d, max_retry=%d, ratio=%.2f, delay=%d", 
        max_size_, max_ms_, max_distance_, max_retry_, nack_ratio_, delayMs);
}

RtpPacket::Ptr RtpJitter::getPacket(uint16_t seq) const {
    auto it = seq_map_.find(seq);
    if (it != seq_map_.end())
        return it->second.rtp;
    return nullptr;
}

int RtpJitter::getJitterMs() const {
    int ret = 0;
    if (seq_map_.size() > 1) {
        return seq_map_.rbegin()->second.create_time - seq_map_.begin()->second.create_time;
    }
    return 0;
}

std::string RtpJitter::sizeStr() const {
    char line[32];
    snprintf(line, sizeof(line), "size=%zu, ms=%d", seq_map_.size(), getJitterMs());
    return line;
}

bool RtpJitter::needDrop(uint64_t now) const {
    if (seq_map_.empty())
        return false;
    return seq_map_.size() > max_size_ || (now - seq_map_.begin()->second.create_time) > max_ms_;
}

void RtpJitter::UpdateRTT(int rtt) {
    if (rtt < MIN_RTT)
        rtt = MIN_RTT;
    else if (rtt > MAX_RTT)
        rtt = MAX_RTT;
    if (abs(rtt - rtt_) > 10) {
        hlogd("rtt change: %d -> %d", rtt_, rtt);
    }
    rtt_ = rtt;
}

void RtpJitter::output(RtpPacket::Ptr rtp) {
    uint16_t seq = rtp->getSeq();
    if (seq != min_seq_) {
        hlogw("drop %d->%d, jitter %s", min_seq_, seq, sizeStr().c_str());
    }
    min_seq_ = static_cast<uint16_t>(seq + 1);

    if (frame_cb_) {
        // 视频包进入组帧逻辑，完整帧由onFrame回调输出
        assembleFrame(rtp);
    } else if(rtp_cb_) {
        rtp_cb_(rtp);
    }
}

bool RtpJitter::pop(uint64_t now) {
    if (seq_map_.empty())
        return false;
    auto iter = seq_map_.begin();
    uint16_t seq = iter->first;
    auto ns = iter->second;
    seq_map_.erase(iter);
    if (ns.rtp) {
        hlogi("rtp %d, jitter %s used %dms", seq, sizeStr().c_str(), now - ns.create_time);
        output(ns.rtp);
    } else {
        hlogd("nack %d, jitter %s used %dms", seq, sizeStr().c_str(), now - ns.create_time);
    }
    return true;
}

void RtpJitter::put(uint16_t seq, RtpPacket::Ptr rtp, uint64_t now) {
    auto &ns = seq_map_[seq];
    if (rtp) {
        if (!ns.create_time) {
            ns.create_time = now;
            hlogd("rtp %d, jitter %s", seq, sizeStr().c_str());
        } else if (!ns.rtp) {
            int rtt = now - ns.nack_time;
            hlogd("recover seq %d with retry %d got rtt %d, delay=%d, min_seq=%d", seq, ns.retry_cnt, rtt, now - ns.create_time, min_seq_);
            if (ns.retry_cnt==1 && delay_ms_) UpdateRTT(rtt);
        }
        ns.rtp = rtp;
    } else {
        ns.create_time = ns.nack_time = now;
        hlogd("detect lost %d, jitter %s", seq, sizeStr().c_str());
    }
}

void RtpJitter::checkTimer() {
    if (this->seq_map_.empty()) {
        if (timer_) {
            hv::killTimer(timer_);
            timer_ = 0;
        }
    } else if (!this->timer_) {
        this->timer_ = hv::setTimeout(this->rtt_, [this](hv::TimerID id) {
            LostList lost_seqs;
            if (getLostSeq(lost_seqs)) {
                onLostPacket(lost_seqs);
            }
            if (seq_map_.empty()) {
                hv::killTimer(timer_);
                timer_ = 0;
            }
        });
    }
}

int RtpJitter::getLostSeq(LostList &seq) {
    int ret = 0;
    auto now = gettick_ms();
    compact(now);
    int delay = nack_ratio_ * rtt_;
    if (delay > MAX_RTT) delay = MAX_RTT;
    for (auto it = seq_map_.begin(); it != seq_map_.end(); it++) {
        if (it->second.rtp)
            continue;
        auto &ns = it->second;
        if (delay_ms_ > 0 && !ns.retry_cnt && (now - ns.create_time) > delay_ms_) { // 第一次重传
            ns.retry_cnt = 1;
            ns.nack_time = now;
            seq.push_back(it->first);
            ret++;
            hlogd("send nack %d,1 with delay %d", it->first, now - ns.create_time);
        } else if ((now - ns.nack_time) > ((ns.retry_cnt < max_retry_) ? delay : MAX_RTT)) {
            ns.retry_cnt++;
            hlogd("resend nack %d,%d with delay %d, %d", it->first, ns.retry_cnt, now - ns.nack_time, now - ns.create_time);
            ns.nack_time = now;
            seq.push_back(it->first);
            ret++;
        }
    }
    return ret;
}

void RtpJitter::input(RtpPacket::Ptr rtp, bool rtx) {
    uint16_t seq = rtp->getSeq();
    ssrc_ = rtp->getSSRC();
    if (!start_) {
        max_seq_ = seq;
        min_seq_ = seq;
        start_ = true;
    }
    SeqLowerThan comp;
    if (comp(seq, min_seq_)) { // 旧包已回调则丢弃该包
        hlogd("skip %s packet %d, min_seq=%d", rtx ? "rtx" : "old", seq, min_seq_);
        return;
    }
    auto now = gettick_ms();
    if (seq == min_seq_) {
        put(seq, rtp, now);
        flush();
        if (comp(max_seq_, min_seq_))
            max_seq_ = min_seq_;
        return;
    }

    if (has_keyreq_ && rtp->IsKeyFrame()) {
        this->key_seqs_.insert(seq);
        auto it = this->key_seqs_.lower_bound(seq - max_size_);
        if (it != this->key_seqs_.begin()) {
            this->key_seqs_.erase(this->key_seqs_.begin(), it);
        }
        hlogd("got key %d, total %zu", seq, this->key_seqs_.size());
    }

    if (rtx || comp(seq, max_seq_)) {
        put(seq, rtp, now);
        return;
    }
    if (seq == max_seq_) {
        put(seq, rtp, now);
    } else {
        uint16_t cur = max_seq_;
        int distance = SeqLowerThan::Distance(seq, cur);
        if (distance < max_distance_) {
            LostList lost;
            for (; cur != seq; cur++) {
                put(cur, nullptr, now);
                if (delay_ms_ == 0) {
                    lost.push_back(cur);
                    seq_map_[cur].retry_cnt = 1;
                    seq_map_[cur].nack_time = now;
                }
            }
            if (lost.size()) {
                onLostPacket(lost);
            }
        } else {
            // seq long jump
            hlogw("skip big lost %d:%d->%d", distance, cur, seq);
        }
        put(seq, rtp, now);
    }
    max_seq_ = static_cast<uint16_t>(seq + 1);
    compact(now);
}

bool RtpJitter::RemoveNackItemsUntilKeyFrame() {
    while (!this->key_seqs_.empty()) {
        uint16_t kseq = *this->key_seqs_.begin();
        auto it = this->seq_map_.find(kseq);
        if (it != this->seq_map_.end()) {
            // We have found a keyframe that actually is newer than at least one
            // packet in the nack list.
            this->seq_map_.erase(this->seq_map_.begin(), it);
            hlogi("drop to key %d->%d, jitter %s", min_seq_, kseq, sizeStr().c_str());
            min_seq_ = kseq;
            return true;
        }

        // If this keyframe is so old it does not remove any packets from the list,
        // remove it from the list of keyframes and try the next keyframe.
        this->key_seqs_.erase(this->key_seqs_.begin());
    }
    return false;
}

void RtpJitter::compact(uint64_t now) {
    // 清理Jitter
    while (needDrop(now)) {
        if (has_keyreq_) {
            // 丢到下一关键帧
            if (RemoveNackItemsUntilKeyFrame()) {
                flush();
            } else {
                // 丢无可丢，则清空缓存请求关键帧
                hlogw("clear all %zu items without keyframe", seq_map_.size());
                seq_map_.clear();
                min_seq_ = max_seq_;
                onRequestKeyframe();
                break;
            }
        } else {
            pop(now);
        }
    }
    // 刷新剩余连续帧
    flush();
}

int RtpJitter::flush() {
    int ret = 0;
    uint64_t first = 0, last = 0;
    auto it = seq_map_.begin();
    while (it != seq_map_.end()) {
        if (it->first == min_seq_ && it->second.rtp) {
            if (!first)
                first = it->second.create_time;
            else
                last = it->second.create_time;
            output(it->second.rtp);
            it = seq_map_.erase(it);
            ret++;
        } else {
            break;
        }
    }

    if (ret > 1) {
        int64_t delta = last - first;
        hlogi("%d pkts %d ms, jitter %s, min_seq=%d", ret, delta, sizeStr().c_str(), min_seq_);
    }
    checkTimer();
    return ret;
}

// ========================= 视频组帧 =========================

void RtpJitter::clearFrame() {
    frame_pkts_.clear();
    frame_timestamp_ = 0;
    frame_has_loss_ = false;
}

void RtpJitter::assembleFrame(RtpPacket::Ptr rtp) {
    uint32_t ts = rtp->getTimestamp();

    // timestamp变化 → 新帧开始，先输出旧帧
    if (!frame_pkts_.empty() && ts != frame_timestamp_) {
        emitFrame();
    }

    // 新帧开始
    if (frame_pkts_.empty()) {
        frame_timestamp_ = ts;
        frame_has_loss_ = false;
    }

    frame_pkts_.push_back(rtp);

    // Marker bit = 1 → 帧的最后一个包，输出完整帧
    if (rtp->getMarker()) {
        emitFrame();
    }
}

void RtpJitter::emitFrame() {
    if (frame_pkts_.empty()) return;

    Frame::Ptr frame = std::make_shared<Frame>();
    frame->timestamp = frame_timestamp_;
    frame->timestamp = frame_pkts_[0]->ntp_stamp;
    frame->codec = frame_pkts_[0]->codec;

    bool ok = false;
    for (auto &pkt : frame_pkts_) {
        if (!frame->appendRtp(pkt)) {
            hlogw("Rtp extract failed, seq=%d", pkt->getSeq());
            ok = false;
            break;
        }
        ok = true;
    }

    if (ok && frame->size()) {
        hlogd("video frame: %s, ts=%u, %zu bytes, %zu pkts",
              frame->is_key ? "key" : "delta",
              frame->timestamp, 
              frame->size(), frame_pkts_.size());
        if (frame_cb_) frame_cb_(frame);
    }

    clearFrame();
}
