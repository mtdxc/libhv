/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "Nack.h"
#include "onceToken.h"
#include "config.h"
#include "htime.h"
#include "hlog.h"
#include <algorithm>

using namespace std;

namespace ice {

// RTC配置项
namespace Rtc {
#define RTC_FIELD "rtc."
// ~ nack接收端, rtp发送端
// rtp重发缓存列队最大长度，单位毫秒
const string kMaxRtpCacheMS = RTC_FIELD "maxRtpCacheMS";
// rtp重发缓存列队最大长度，单位个数
const string kMaxRtpCacheSize = RTC_FIELD "maxRtpCacheSize";

// ~ nack发送端，rtp接收端
// 最大保留的rtp丢包状态个数
const string kNackMaxSize = RTC_FIELD "nackMaxSize";
// rtp丢包状态最长保留时间
const string kNackMaxMS = RTC_FIELD "nackMaxMS";
// nack最多请求重传次数
const string kNackMaxCount = RTC_FIELD "nackMaxCount";
// nack重传频率，rtt的倍数
const string kNackIntervalRatio = RTC_FIELD "nackIntervalRatio";
// nack包中rtp个数，减小此值可以让nack包响应更灵敏
const string kNackRtpSize = RTC_FIELD "nackRtpSize";

static onceToken token([]() {
    mINI::Instance()[kMaxRtpCacheMS] = 2000;
    mINI::Instance()[kMaxRtpCacheSize] = 1024;
    mINI::Instance()[kNackMaxSize] = 1024;
    mINI::Instance()[kNackMaxMS] = 1000;
    mINI::Instance()[kNackMaxCount] = 5;
    mINI::Instance()[kNackIntervalRatio] = 1.0f;
    mINI::Instance()[kNackRtpSize] = 3;

});

} // namespace Rtc

void NackList::pushBack(RtpPacket::Ptr rtp) {
    GET_CONFIG(uint32_t, max_rtp_cache_ms, Rtc::kMaxRtpCacheMS);
    GET_CONFIG(uint32_t, max_rtp_cache_size, Rtc::kMaxRtpCacheSize);

    // Record rtp
    auto seq = rtp->getSeq();
    _nack_cache_seq.emplace_back(seq);
    _nack_cache_pkt.emplace(seq, std::move(rtp));

    // Limit the maximum number of rtp cache
    if (_nack_cache_seq.size() > max_rtp_cache_size) {
        popFront();
    }

    if (++_cache_ms_check < 100) {
        // 每100个rtp包检测下缓存长度，节省cpu资源
        return;
    }
    _cache_ms_check = 0;
    // 限制rtp缓存最大时长
    while (getCacheMS() >= max_rtp_cache_ms) {
        popFront();
    }
}

void NackList::forEach(const FCI_NACK &nack, const function<void(const RtpPacket::Ptr &rtp)> &func) {
    auto seq = nack.getPid();
    for (auto bit : nack.getBitArray()) {
        if (bit) {
            // Packet loss
            RtpPacket::Ptr *ptr = getRtp(seq);
            if (ptr) {
                func(*ptr);
            }
        }
        ++seq;
    }
}

void NackList::popFront() {
    if (_nack_cache_seq.empty()) {
        return;
    }
    _nack_cache_pkt.erase(_nack_cache_seq.front());
    _nack_cache_seq.pop_front();
}

RtpPacket::Ptr *NackList::getRtp(uint16_t seq) {
    auto it = _nack_cache_pkt.find(seq);
    if (it == _nack_cache_pkt.end()) {
        return nullptr;
    }
    return &it->second;
}

uint32_t NackList::getCacheMS() {
    while (_nack_cache_seq.size() > 2) {
        auto back_stamp = getNtpStamp(_nack_cache_seq.back());
        if (back_stamp == -1) {
            _nack_cache_seq.pop_back();
            continue;
        }

        auto front_stamp = getNtpStamp(_nack_cache_seq.front());
        if (front_stamp == -1) {
            _nack_cache_seq.pop_front();
            continue;
        }

        if (back_stamp >= front_stamp) {
            return back_stamp - front_stamp;
        }
        // ntp时间戳回退了，非法数据，丢掉
        _nack_cache_seq.pop_front();
    }
    return 0;
}

int64_t NackList::getNtpStamp(uint16_t seq) {
    auto it = _nack_cache_pkt.find(seq);
    if (it == _nack_cache_pkt.end()) {
        return -1;
    }
    // 使用ntp时间戳，不会回退
    return it->second->ntp_stamp;
}

////////////////////////////////////////////////////////////////////////////////////////////////

NackContext::NackContext(uint32_t ssrc) : _ssrc(ssrc) {
    setOnNack(nullptr);
}

void NackContext::received(uint16_t seq, bool is_rtx) {
    if (!_started) {
        // Record the first seq
        _started = true;
        _nack_seq = seq - 1;
    }
    // InfoT << "received " << seq << " rtx=" << is_rtx;
    if (seq < _nack_seq && _nack_seq != UINT16_MAX && seq < 1024 && _nack_seq > UINT16_MAX - 1024) {
        // seq回环,清空回环前状态
        makeNack(UINT16_MAX, true);
        _seq.emplace(seq);
        return;
    }

    if (is_rtx || (seq < _nack_seq && _nack_seq != UINT16_MAX)) {
        // seq非回环回退包，猜测其为重传包，清空其nack状态
        clearNackStatus(seq);
        return;
    }

    auto pr = _seq.emplace(seq);
    if (!pr.second) {
        // Seq duplicate, ignore
        return;
    }

    auto max_seq = *_seq.rbegin();
    auto min_seq = *_seq.begin();
    auto diff = max_seq - min_seq;
    if (diff > (UINT16_MAX >> 1)) {
        // 回环后，忽略掉回环前的大值seq
        _seq.erase(max_seq);
        return;
    }
    if (min_seq == (uint16_t)(_nack_seq + 1) && _seq.size() == (size_t)diff + 1) {
        // all continuous seq, no packet loss
        _seq.clear();
        _nack_seq = max_seq;
    } else {
        // Seq is not continuous, there is packet loss
        makeNack(max_seq, false);
    }
}

void NackContext::makeNack(uint16_t max_seq, bool flush) {
    // 尝试移除前面部分连续的seq
    eraseFrontSeq();
    GET_CONFIG(uint32_t, nack_rtpsize, Rtc::kNackRtpSize);
    if(nack_rtpsize > FCI_NACK::kBitSize) nack_rtpsize = FCI_NACK::kBitSize;
    // 最多生成5个nack包，防止seq大幅跳跃导致一直循环
    auto max_nack = 5u;
    while (_nack_seq != max_seq && max_nack--) {
        // 一次不能发送超过16+1个rtp的状态
        uint16_t nack_rtp_count = std::min<uint16_t>(FCI_NACK::kBitSize, max_seq - (uint16_t)(_nack_seq + 1));
        if (!flush && nack_rtp_count < nack_rtpsize) {
            // 非flush状态下，seq个数不足以发送一个nack包
            break;
        }

        // make lost map
        std::vector<bool> vec;
        vec.resize(nack_rtp_count, false);
        for (size_t i = 0; i < nack_rtp_count; ++i) {
            vec[i] = !_seq.count((uint16_t)(_nack_seq + i + 2));
        }
        doNack(FCI_NACK(_nack_seq + 1, vec), true);

        // advance _nack_seq and clear _seq cache
        _nack_seq += nack_rtp_count + 1;
        auto it = _seq.upper_bound(_nack_seq);
        // Remove seq <= _nack_seq
        _seq.erase(_seq.begin(), it);
    }
}

void NackContext::setOnNack(onNack cb) {
    if (cb) {
        _cb = std::move(cb);
    } else {
        _cb = [](const FCI_NACK &nack) {};
    }
}

void NackContext::doNack(const FCI_NACK &nack, bool record_nack) {
    if (record_nack) {
        recordNack(nack);
    }
    _cb(nack);
}

void NackContext::eraseFrontSeq() {
    // 移除队首连续的seq.
    for (auto it = _seq.begin(); it != _seq.end();) {
        if (*it != (uint16_t)(_nack_seq + 1)) {
            break;
        }
        _nack_seq = *it;
        it = _seq.erase(it);
    }
}

void NackContext::clearNackStatus(uint16_t seq) {
    auto it = _nack_send_status.find(seq);
    if (it == _nack_send_status.end()) {
        return;
    }
    // 收到重传包与第一个nack包间的时间约等于rtt时间.
    auto rtt = gettick_ms() - it->second.first_stamp;
    hlogd("recovered seq %d and got rtt %d", seq, rtt);
    _nack_send_status.erase(it);
    UpdateRtt(rtt);
}

void NackContext::UpdateRtt(uint32_t rtt) {
    // 限定rtt在合理有效范围内
    GET_CONFIG(uint32_t, nack_maxms, Rtc::kNackMaxMS);
    GET_CONFIG(uint32_t, nack_maxcount, Rtc::kNackMaxCount);
    int val = max<int>(10, min<int>(rtt, nack_maxms / nack_maxcount));
    if (abs(_rtt - val) > 10) {
        hlogd("%d->%d,%d", _rtt, val, rtt);
    }
    _rtt = val;
}

void NackContext::recordNack(const FCI_NACK &nack) {
    auto now = gettick_ms();
    auto seq = nack.getPid();
    for (auto flag : nack.getBitArray()) {
        if (flag) {
            auto &ref = _nack_send_status[seq];
            ref.first_stamp = now;
            ref.update_stamp = now;
            ref.nack_count = 1;
        }
        ++seq;
    }
    // There are too many records, remove some of the earlier records.
    GET_CONFIG(uint32_t, nack_maxsize, Rtc::kNackMaxSize);
    while (_nack_send_status.size() > nack_maxsize) {
        _nack_send_status.erase(_nack_send_status.begin());
    }
}

uint64_t NackContext::reSendNack() {
    // 保存需要发送nack的seq列表
    std::list<uint16_t> nack_rtp;
    auto now = gettick_ms();
    GET_CONFIG(uint32_t, nack_maxms, Rtc::kNackMaxMS);
    GET_CONFIG(uint32_t, nack_maxcount, Rtc::kNackMaxCount);
    GET_CONFIG(float, nack_intervalratio, Rtc::kNackIntervalRatio);
    for (auto it = _nack_send_status.begin(); it != _nack_send_status.end();) {
        if (now - it->second.first_stamp > nack_maxms) {
            // 删除过期的nack项
            it = _nack_send_status.erase(it);
            continue;
        }
        if (now - it->second.update_stamp < nack_intervalratio * _rtt) {
            // 防止频繁发送nack请求
            ++it;
            continue;
        }
        // This rtp needs to request retransmission.
        nack_rtp.push_back(it->first);
        // Update the nack sending timestamp.
        it->second.update_stamp = now;
        if (++(it->second.nack_count) >= nack_maxcount) {
            // Too many nack times, remove it.
            it = _nack_send_status.erase(it);
            continue;
        }
        ++it;
    }

    sendNack(nack_rtp, _cb);
    // 没有任何包需要重传时返回0，否则返回下次重传间隔(不得低于5ms)
    return _nack_send_status.empty() ? 0 : _rtt;
}

void NackContext::sendNack(const std::list<uint16_t>& nack_rtp, onNack cb) {
    int pid = -1;
    std::vector<bool> vec;
    for (auto it = nack_rtp.begin(); it != nack_rtp.end();) {
        if (pid == -1) {
            pid = *it;
            vec.assign(FCI_NACK::kBitSize, false);
            ++it;
            continue;
        }
        auto inc = *it - pid;
        if (inc < -32768)
            inc += 65536;
        if (inc > FCI_NACK::kBitSize) {
            // New nack packet.
            cb(FCI_NACK(pid, vec));
            pid = -1;
            continue;
        }
        // This packet is lost.
        if (inc > 0)
            vec[inc - 1] = true;
        ++it;
    }
    if (pid != -1) {
        cb(FCI_NACK(pid, vec));
    }
}

} // namespace mediakit
