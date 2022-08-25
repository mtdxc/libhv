﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SRC_RTSP_RTPBROADCASTER_H_
#define SRC_RTSP_RTPBROADCASTER_H_

#include <mutex>
#include <memory>
#include <unordered_set>
#include <unordered_map>
#include "Common/config.h"
#include "RtspMediaSource.h"
#include "Util/mini.h"
#include "Network/Socket.h"

namespace mediakit{
// 多播地址分配器
class MultiCastAddressMaker {
public:
    ~MultiCastAddressMaker() {}

    static MultiCastAddressMaker& Instance();
    static bool isMultiCastAddress(uint32_t addr);
    static std::string toString(uint32_t addr);

    std::shared_ptr<uint32_t> obtain(uint32_t max_try = 10);

private:
    MultiCastAddressMaker() {};
    void release(uint32_t addr);

private:
    uint32_t _addr = 0;
    std::recursive_mutex _mtx;
    std::unordered_set<uint32_t> _used_addr;
};

// 多播广播器，负责将RtspMediaSource在多播地址上广播
class RtpMultiCaster {
public:
    typedef std::shared_ptr<RtpMultiCaster> Ptr;
    typedef std::function<void()> onDetach;
    ~RtpMultiCaster();

    static Ptr get(toolkit::SocketHelper &helper, const std::string &local_ip, const std::string &vhost, const std::string &app, const std::string &stream);
    // add or remove from _detach_map
    void setDetachCB(void *listener,const onDetach &cb);

    std::string getMultiCasterIP();
    uint16_t getMultiCasterPort(TrackType trackType);

private:
    RtpMultiCaster(toolkit::SocketHelper &helper, const std::string &local_ip, const std::string &vhost, const std::string &app, const std::string &stream);

private:
    std::recursive_mutex _mtx;
    // audio/video sock
    toolkit::Socket::Ptr _udp_sock[2];
    std::shared_ptr<uint32_t> _multicast_ip;
    std::unordered_map<void * , onDetach > _detach_map;
    RtspMediaSource::RingType::RingReader::Ptr _rtp_reader;
};

}//namespace mediakit
#endif /* SRC_RTSP_RTPBROADCASTER_H_ */
