/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "WebRtcSession.h"
#include "WebRtcServer.h"
#include "StunPacket.hpp"
#include "Common/config.h"
#include "WebRtcTransport.h"

using namespace toolkit;
using namespace mediakit;

std::string WebRtcSession::getUserName(void* buf, int len) {
    if (!RTC::StunPacket::IsStun((const uint8_t *) buf, len)) {
        return "";
    }
    std::unique_ptr<RTC::StunPacket> packet(RTC::StunPacket::Parse((const uint8_t *) buf, len));
    if (!packet) {
        return "";
    }
    if (packet->GetClass() != RTC::StunPacket::Class::REQUEST ||
        packet->GetMethod() != RTC::StunPacket::Method::BINDING) {
        return "";
    }
    //收到binding request请求
    auto vec = hv::split(packet->GetUsername(), ':');
    return vec[0];
}

WebRtcSession::WebRtcSession(hio_t* io) : toolkit::Session(io)
{
    _peer_addr = hio_peeraddr(io);
}

void WebRtcSession::onRecv(hv::Buffer* buf) {
    if (_find_transport) {
        //只允许寻找一次transport
        _find_transport = false;
        auto user_name = getUserName(buf->data(), buf->size());
        _identifier = std::to_string(fd()) + '-' + user_name;
        auto transport = WebRtcServer::Instance().getItem(user_name);
        CHECK(transport && transport->getPoller()->isInLoopThread());
        transport->setSession(shared_from_this());
        _transport = std::move(transport);
        //InfoP(this);
    }
    _ticker.resetTime();
    CHECK(_transport);
    try {
        _transport->inputSockData((char*)buf->data(), buf->size(), _peer_addr);
    }
    catch (...) {
    }
}

void WebRtcSession::onManager() {
    GET_CONFIG(float, timeoutSec, RTC::kTimeOutSec);
    if (!_transport && _ticker.createdTime() > timeoutSec * 1000) {
        shutdown(SockException(Err_timeout, "illegal webrtc connection"));
        return;
    }
    if (_ticker.elapsedTime() > timeoutSec * 1000) {
        shutdown(SockException(Err_timeout, "webrtc connection timeout"));
        return;
    }
}

void WebRtcSession::onError(const toolkit::SockException &err)
{
    //udp链接超时，但是rtc链接不一定超时，因为可能存在udp链接迁移的情况
    //在udp链接迁移时，新的WebRtcSession对象将接管WebRtcTransport对象的生命周期
    //本WebRtcSession对象将在超时后自动销毁
    WarnP(this) << err.what();

    if (!_transport) {
        return;
    }
    auto transport = std::move(_transport);
    getPoller()->async([transport] {
        //延时减引用，防止使用transport对象时，销毁对象
    }, false);
}


