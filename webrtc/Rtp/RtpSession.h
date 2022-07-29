﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_RTPSESSION_H
#define ZLMEDIAKIT_RTPSESSION_H

#if defined(ENABLE_RTPPROXY)

#include "Socket.h"
#include "RtpSplitter.h"
#include "RtpProcess.h"
#include "Util/TimeTicker.h"

namespace mediakit{

class RtpSession : public toolkit::Session, public RtpSplitter, public MediaSourceEvent,
    public std::enable_shared_from_this<RtpSession> {
public:
    static const std::string kStreamID;
    static const std::string kIsUDP;
    static const std::string kSSRC;

    RtpSession(hio_t* io);
    ~RtpSession() override;

    void onRecv(const toolkit::Buffer::Ptr &);
    void onError(const toolkit::SockException &err) override;
    void onManager() override;
    //void attachServer(const toolkit::Server &server) override;
protected:
    // 通知其停止推流
    bool close(MediaSource &sender,bool force) override;
    // 观看总人数
    int totalReaderCount(MediaSource &sender) override;
    // 收到rtp回调
    void onRtpPacket(const char *data, size_t len) override;

    const char *onSearchPacketTail(const char *data, size_t len) override;

private:
    bool _is_udp = false;
    bool _search_rtp = false;
    bool _search_rtp_finished = false;
    uint32_t _ssrc = 0;
    toolkit::Ticker _ticker;
    std::string _stream_id;
    struct sockaddr* _addr;
    RtpProcess::Ptr _process;
    // 负责自动增加和减少对象统计数
    std::shared_ptr<void> _statistic_counter;
};

}//namespace mediakit
#endif//defined(ENABLE_RTPPROXY)
#endif //ZLMEDIAKIT_RTPSESSION_H
