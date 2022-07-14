﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "WebRtcPlayer.h"
#include "Util/NoticeCenter.h"
using namespace mediakit;
using namespace toolkit;
WebRtcPlayer::Ptr WebRtcPlayer::create(const hv::EventLoopPtr &poller,
                                       const RtspMediaSource::Ptr &src,
                                       const MediaInfo &info) {
    WebRtcPlayer::Ptr ret(new WebRtcPlayer(poller, src, info), [](WebRtcPlayer *ptr) {
        ptr->onDestory();
        delete ptr;
    });
    ret->onCreate();
    return ret;
}

WebRtcPlayer::WebRtcPlayer(const hv::EventLoopPtr &poller,
                           const RtspMediaSource::Ptr &src,
                           const MediaInfo &info) : WebRtcTransportImp(poller) {
    _media_info = info;
    _play_src = src;
    CHECK(_play_src);
}

void WebRtcPlayer::onStartWebRTC() {
    CHECK(_play_src);
    WebRtcTransportImp::onStartWebRTC();
    if (canSendRtp()) {
        _play_src->pause(false);
        _reader = _play_src->getRing()->attach(getPoller(), true);
        std::weak_ptr<WebRtcPlayer> weak_self = std::static_pointer_cast<WebRtcPlayer>(shared_from_this());
        _reader->setReadCB([weak_self](const RtspMediaSource::RingDataType &pkt) {
            auto strongSelf = weak_self.lock();
            if (!strongSelf) {
                return;
            }
            size_t i = 0;
            for(auto rtp : *pkt) {
                // TraceL << getIdentifier() << " send " << rtp->dump() << " i:"<<i;
                strongSelf->onSendRtp(rtp, ++i == pkt->size());
            }
        });
        _reader->setDetachCB([weak_self]() {
            if (auto strongSelf = weak_self.lock()) {
                strongSelf->onShutdown(SockException(Err_shutdown, "rtsp ring buffer detached"));
            }
        });
    }
    //使用完毕后，释放强引用，这样确保推流器断开后能及时注销媒体
    _play_src = nullptr;
}

void WebRtcPlayer::onDestory() {
    WebRtcTransportImp::onDestory();

    auto duration = getDuration();
    auto bytes_usage = getBytesUsage();
    //流量统计事件广播
    GET_CONFIG(uint32_t, iFlowThreshold, General::kFlowThreshold);
    if (_reader && getSession()) {
        WarnL << "RTC播放器(" << _media_info.shortUrl() << ")结束播放,耗时(s):" << duration;
        if (bytes_usage >= iFlowThreshold * 1024) {
            NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastFlowReport, _media_info, bytes_usage, duration,
                                               true, *getSession());
        }
    }
}

void WebRtcPlayer::onRtcConfigure(RtcConfigure &configure) const {
    CHECK(_play_src);
    WebRtcTransportImp::onRtcConfigure(configure);
    //这是播放
    configure.audio.direction = configure.video.direction = RtpDirection::sendonly;
    configure.setPlayRtspInfo(_play_src->getSdp());
}