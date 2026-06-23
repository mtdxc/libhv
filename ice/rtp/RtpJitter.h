#ifndef _RTP_JITTER_H_
#define _RTP_JITTER_H_
#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Frame.h"
//#include "rtc/Stamp.h"
#include "RtpPacket.h"

namespace ice {
struct NackStatus {
    uint64_t create_time = 0;
    uint64_t nack_time = 0;
    int retry_cnt = 0;
    RtpPacket::Ptr rtp;
};

struct SeqLowerThan {
    static constexpr uint16_t MaxValue = 0xFFFF;
    bool operator()(uint16_t lhs, uint16_t rhs) const {
		return ((rhs > lhs) && (rhs - lhs <= MaxValue / 2)) ||
		       ((lhs > rhs) && (lhs - rhs > MaxValue / 2));
    }
    static int Distance(uint16_t v1, uint16_t v2) {
        int ret = v1 - v2;
        if (ret > MaxValue / 2)
            ret -= MaxValue;
        else if (ret < -MaxValue / 2)
            ret += MaxValue;
        return ret;
    }
};
using RtpMap = std::map<uint16_t, NackStatus, SeqLowerThan>;
using LostList = std::list<uint16_t>;
using onFrame = std::function<void(Frame::Ptr)>;
using onSort = std::function<void(RtpPacket::Ptr)>;

/*
* 将读取和写入放一起，能做到缓存最小化，当没丢包时没缓存
*/
class RtpJitter {
    // rtp和nack队列
    RtpMap seq_map_;
    // 队列最大ms
    int max_ms_ = 1000;
    // 队列最大个数
    int max_size_ = 1024;
    // 最大重试次数
    int max_retry_ = 5;
    // 当收到seq - max_seq > max_distance_时，不请求nack，用于处理Seq跳跃
    int max_distance_ = 80;
    // nack发送延迟，delay = nack_ratio_ * rtt_
    float nack_ratio_ = 1.2f;
    // 检测到丢包后，延迟该ms再请求nack，
    // 该延迟内，如有乱序包的到达，则不请求nack
    unsigned int delay_ms_ = 20;
    int rtt_ = 60;
    // 输入缓冲区指针，指向收到的最大seq，用于判断丢包和生成nack
    uint16_t max_seq_ = 0;
    // 输出缓冲区指针，用于连续输出Rtp包
    uint16_t min_seq_ = 0;
    // start_ tag
    bool start_ = false;

    // 定时器，用于重发nack
    uint64_t timer_;
    /*
      是否启用关键帧队列开关，为true则
      - 在input时更新key_seqs_，
      - 并在compact时，启用关键帧丢包RemoveNackItemsUntilKeyFrame
    */ 
    bool has_keyreq_ = false;
    // 关键帧序列列表
    std::set<uint16_t, SeqLowerThan> key_seqs_;
protected:
    // 丢到下一个关键帧
    bool RemoveNackItemsUntilKeyFrame();

    // 入rtp/nack(rtp==null)包
    void put(uint16_t seq, RtpPacket::Ptr rtp, uint64_t now);
    // pop首个rtp/nack包
    bool pop(uint64_t now);
    // 输出包，并更新min_seq_ = rtp->getSeq() + 1
    void output(RtpPacket::Ptr rtp);

    // 判断是否需要丢帧(缓冲区满，或延迟超过)
    bool needDrop(uint64_t now) const;
    /* 该函数先检测needDrop，当返回true时，
     * 采取强制收缩缓冲区(必要时丢包)，并依据has_keyreq_值支持两种丢包方式
     * - 1 视频丢帧: 丢到下一关键帧的seq，当没关键帧时，则全丢并请求关键帧
     * - 0 音频丢帧: 每次pop一个包，直到needDrop返回false，此时会输出已缓存的rtp包
     */
    void compact(uint64_t now);
    // 刷新头部连续的rtp包
    int flush();

    void checkTimer();
    // 返回nack的系列号列表
    int getLostSeq(LostList &seq);

protected:
    // 请求nack回调
    virtual void onLostPacket(const LostList &seq) {}
    // 请求关键帧回调
    virtual void onRequestKeyframe() {}
    onFrame frame_cb_;
    onSort rtp_cb_;
    uint32_t ssrc_ = 0;

    // 视频组帧相关
    std::vector<RtpPacket::Ptr> frame_pkts_;
    uint32_t frame_timestamp_ = 0;
    bool frame_has_loss_ = false;

    // 组帧核心逻辑
    void assembleFrame(RtpPacket::Ptr rtp);
    void emitFrame();
    void clearFrame();
public:
    RtpJitter() = default;
    virtual ~RtpJitter();

    uint32_t getIdentifier() const { return ssrc_; }
    uint32_t getSSRC() const { return ssrc_; }
    void enableKeyframeReq(bool v) { has_keyreq_ = v; }
    void setOnFrame(onFrame cb) { frame_cb_ = std::move(cb); }
    void setOnSort(onSort cb) { rtp_cb_ = std::move(cb); }
    void setParams(size_t max_size, size_t max_ms, int max_retry, float ratio = 1.2f, unsigned int delay_ms = 0);
    int getJitterMs() const;
    std::string sizeStr() const;

    RtpPacket::Ptr getPacket(uint16_t seq) const;
    void input(RtpPacket::Ptr rtp, bool rtx);
    void UpdateRTT(int rtt);
    void clear();
};
}
#endif
