﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_RTPSENDER_H
#define ZLMEDIAKIT_RTPSENDER_H

#if defined(ENABLE_RTPPROXY)
#include "Extension/CommonRtp.h"
#include "Common/MediaSource.h"
#include "Common/MediaSink.h"
#include "Network/Socket.h"
namespace mediakit{

//rtp发送客户端，支持发送GB28181协议
class RtpSender : public MediaSinkInterface, public std::enable_shared_from_this<RtpSender>{
public:
    typedef std::shared_ptr<RtpSender> Ptr;

    RtpSender();
    ~RtpSender() override = default;

    /**
     * 开始发送ps-rtp包
     * @param args 发送参数
     * @param cb 连接目标端口是否成功的回调
     */
    void startSend(const MediaSourceEvent::SendRtpArgs &args, const std::function<void(uint16_t local_port, const SockException &ex)> &cb);

    /**
     * 输入帧数据
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * 添加track，内部会调用Track的clone方法
     * 只会克隆sps pps这些信息 ，而不会克隆Delegate相关关系
     * @param track
     */
    virtual bool addTrack(const Track::Ptr & track) override;

    /**
     * 添加所有Track完毕
     */
    virtual void addTrackCompleted() override;

    /**
     * 重置track
     */
    virtual void resetTracks() override;

private:
    //合并写输出
    void onFlushRtpList(std::shared_ptr<std::list<Buffer::Ptr> > rtp_list);
    //udp/tcp连接成功回调
    void onConnect();
    //异常断开socket事件
    void onErr(const SockException &ex, bool is_connect = false);

private:
    bool _is_connect = false;
    MediaSourceEvent::SendRtpArgs _args;
    toolkit::Socket::Ptr _socket;
    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr _connect_timer;
    MediaSinkInterface::Ptr _interface;
};

}//namespace mediakit
#endif// defined(ENABLE_RTPPROXY)
#endif //ZLMEDIAKIT_RTPSENDER_H
