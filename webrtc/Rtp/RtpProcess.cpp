﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#if defined(ENABLE_RTPPROXY)
#include "GB28181Process.h"
#include "RtpProcess.h"
#include "Util/File.h"
using namespace std;
using namespace toolkit;

static constexpr char kRtpAppName[] = "rtp";
//在创建_muxer对象前(也就是推流鉴权成功前)，需要先缓存frame，这样可以防止丢包，提高体验
//但是同时需要控制缓冲长度，防止内存溢出。200帧数据，大概有10秒数据，应该足矣等待鉴权hook返回
static constexpr size_t kMaxCachedFrame = 200;

namespace mediakit {

RtpProcess::RtpProcess(const string &stream_id) {
    _media_info._schema = kRtpAppName;
    _media_info._vhost = DEFAULT_VHOST;
    _media_info._app = kRtpAppName;
    _media_info._streamid = stream_id;

    GET_CONFIG(string, dump_dir, RtpProxy::kDumpDir);
    if(!dump_dir.empty())
    {
        FILE *fp = File::create_file(File::absolutePath(_media_info._streamid + ".rtp", dump_dir).c_str(), "wb");
        if (fp) {
            _save_file_rtp.reset(fp, fclose);
        }

        fp = File::create_file(File::absolutePath(_media_info._streamid + ".video", dump_dir).c_str(), "wb");
        if (fp) {
            _save_file_video.reset(fp, fclose);
        }
    }
}

RtpProcess::~RtpProcess() {
    uint64_t duration = (_last_frame_time.createdTime() - _last_frame_time.elapsedTime()) / 1000;
    WarnL << "RTP推流器(" << _media_info.shortUrl() << ")断开,耗时(s):" << duration;

    //流量统计事件广播
    GET_CONFIG(uint32_t, iFlowThreshold, General::kFlowThreshold);
    if (_total_bytes >= iFlowThreshold * 1024) {
        NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastFlowReport, _media_info, _total_bytes, duration, false, *this);
    }
}

bool RtpProcess::inputRtp(bool is_udp, const Session::Ptr &sock, const char *data, size_t len, const struct sockaddr *addr, uint32_t *dts_out) {
    if (_busy_flag.test_and_set()) {
        WarnL << "其他线程正在执行本函数";
        return false;
    }
    onceToken token(nullptr, [&]() {
        _busy_flag.clear();
    });

    if (!_sock) {
        //第一次运行本函数
        _sock = sock;
        _addr.reset(new sockaddr_storage(*((sockaddr_storage *)addr)));
        emitOnPublish();
    }

    _total_bytes += len;
    if (_save_file_rtp) {
        // rtp dump格式
        uint16_t size = (uint16_t)len;
        size = htons(size);
        fwrite((uint8_t *) &size, 2, 1, _save_file_rtp.get());
        fwrite((uint8_t *) data, len, 1, _save_file_rtp.get());
    }
    if (!_process) {
        _process = std::make_shared<GB28181Process>(_media_info, this);
    }

    if (_muxer && !_muxer->isEnabled() && !dts_out && !_save_file_video) {
        //无人访问、且不取时间戳、不导出调试文件时，我们可以直接丢弃数据
        _last_frame_time.resetTime();
        return false;
    }

    bool ret = _process ? _process->inputRtp(is_udp, data, len) : false;
    if (dts_out) {
        *dts_out = _dts;
    }
    return ret;
}

bool RtpProcess::inputFrame(const Frame::Ptr &frame) {
    _dts = frame->dts();
    if (_save_file_video && frame->getTrackType() == TrackVideo) {
        fwrite((uint8_t *) frame->data(), frame->size(), 1, _save_file_video.get());
    }
    if (_muxer) {
        _last_frame_time.resetTime();
        return _muxer->inputFrame(frame);
    }
    // else cache
    if (_cached_func.size() > kMaxCachedFrame) {
        WarnL << "cached frame of track(" << frame->getCodecName() << ") is too much, now dropped, please check your on_publish hook url in config.ini file";
        return false;
    }
    auto frame_cached = Frame::getCacheAbleFrame(frame);
    lock_guard<recursive_mutex> lck(_func_mtx);
    _cached_func.emplace_back([this, frame_cached]() {
        _last_frame_time.resetTime();
        _muxer->inputFrame(frame_cached);
    });
    return true;
}

bool RtpProcess::addTrack(const Track::Ptr &track) {
    if (_muxer) {
        return _muxer->addTrack(track);
    }

    lock_guard<recursive_mutex> lck(_func_mtx);
    _cached_func.emplace_back([this, track]() {
        _muxer->addTrack(track);
    });
    return true;
}

void RtpProcess::addTrackCompleted() {
    if (_muxer) {
        _muxer->addTrackCompleted();
    } else {
        lock_guard<recursive_mutex> lck(_func_mtx);
        _cached_func.emplace_back([this]() {
            _muxer->addTrackCompleted();
        });
    }
}

void RtpProcess::doCachedFunc() {
    lock_guard<recursive_mutex> lck(_func_mtx);
    for (auto &func : _cached_func) {
        func();
    }
    _cached_func.clear();
}

bool RtpProcess::alive() {
    if (_stop_rtp_check.load()) {
        if(_last_check_alive.elapsedTime() > 5 * 60 * 1000){
            //最多暂停5分钟的rtp超时检测，因为NAT映射有效期一般不会太长
            _stop_rtp_check = false;
        } else {
            return true;
        }
    }

    _last_check_alive.resetTime();
    GET_CONFIG(uint64_t, timeoutSec, RtpProxy::kTimeoutSec)
    if (_last_frame_time.elapsedTime() / 1000 < timeoutSec) {
        return true;
    }
    return false;
}

void RtpProcess::setStopCheckRtp(bool is_check){
    _stop_rtp_check = is_check;
    if (!is_check) {
        _last_frame_time.resetTime();
    }
}

void RtpProcess::onDetach() {
    if (_on_detach) {
        _on_detach();
    }
}

void RtpProcess::setOnDetach(const function<void()> &cb) {
    _on_detach = cb;
}

string RtpProcess::getIdentifier() const {
    return _media_info._streamid;
}

int RtpProcess::getTotalReaderCount() {
    return _muxer ? _muxer->totalReaderCount() : 0;
}

void RtpProcess::setListener(const std::weak_ptr<MediaSourceEvent> &listener) {
    setDelegate(listener);
}

void RtpProcess::emitOnPublish() {
    weak_ptr<RtpProcess> weak_self = shared_from_this();
    Broadcast::PublishAuthInvoker invoker = [weak_self](const string &err, const ProtocolOption &option) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        if (err.empty()) {
            strong_self->_muxer = std::make_shared<MultiMediaSourceMuxer>(strong_self->_media_info._vhost,
                                                                          strong_self->_media_info._app,
                                                                          strong_self->_media_info._streamid, 0.0f,
                                                                          option);
            strong_self->_muxer->setMediaListener(strong_self);
            strong_self->doCachedFunc();
            InfoL << "允许RTP推流";
        } else {
            WarnL << "禁止RTP推流:" << err;
        }
    };

    //触发推流鉴权事件
    auto flag = NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastMediaPublish, MediaOriginType::rtp_push, _media_info, invoker, _sock);
    if (!flag) {
        //该事件无人监听,默认不鉴权
        invoker("", ProtocolOption());
    }
}

MediaOriginType RtpProcess::getOriginType(MediaSource &sender) const{
    return MediaOriginType::rtp_push;
}

string RtpProcess::getOriginUrl(MediaSource &sender) const {
    return _media_info.fullUrl();
}

std::shared_ptr<SockInfo> RtpProcess::getOriginSock(MediaSource &sender) const {
    return _sock;
}

toolkit::EventPoller::Ptr RtpProcess::getOwnerPoller(MediaSource &sender) {
    return _sock ? _sock->getPoller() : nullptr;
}

}//namespace mediakit
#endif//defined(ENABLE_RTPPROXY)