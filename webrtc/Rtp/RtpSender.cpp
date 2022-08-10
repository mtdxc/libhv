/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#if defined(ENABLE_RTPPROXY)
#include "RtpSender.h"
#include "Rtsp/RtspSession.h"
#include "WebRtcServer.h"
#include "RtpCache.h"
#include "hsocket.h"
using namespace std;
using namespace toolkit;

namespace mediakit{

RtpSender::RtpSender() {
    _poller = WebRtcServer::Instance().getPoller();
    //_socket = Socket::createSocket(_poller, false);
}

void RtpSender::startSend(const MediaSourceEvent::SendRtpArgs &args, const function<void(uint16_t local_port, const SockException &ex)> &cb){
    _args = args;
    if (!_interface) {
        //重连时不重新创建对象
        auto lam = [this](std::shared_ptr<std::list<Buffer::Ptr>> list) { onFlushRtpList(std::move(list)); };
        if (args.use_ps) {
            _interface = std::make_shared<RtpCachePS>(lam, atoi(args.ssrc.data()), args.pt);
        } else {
            _interface = std::make_shared<RtpCacheRaw>(lam, atoi(args.ssrc.data()), args.pt, args.only_audio);
        }
    }

    std::weak_ptr<RtpSender> weak_self = shared_from_this();
    if (args.passive) {
        // tcp被动发流模式
        _args.is_udp = false;
        try {
            toolkit::Session::Ptr tcp_listener = nullptr;
            if (args.src_port) {
                hio_t* io = hio_create_socket(_poller->loop(), "::", args.src_port);
                //指定端口
                if (!io) {
                    throw std::invalid_argument(StrPrinter << "open tcp passive server failed on port:" << args.src_port);
                }
                tcp_listener = std::make_shared<toolkit::Session>(io);
            } else {
                toolkit::Session::Ptr pr[2];
                //从端口池获取随机端口
                makeSockPair(pr, "::", false, false);
                tcp_listener = pr[0];
            }
            // tcp服务器默认开启5秒
            auto delay_task = _poller->doDelayTask(5 * 1000, [tcp_listener, cb]() mutable {
                cb(0, SockException(Err_timeout, "wait tcp connection timeout"));
                tcp_listener = nullptr;
                return 0;
            });
            typedef std::function<void(hio_t*)> AcceptCB;
            AcceptCB* acceptCb = new AcceptCB([weak_self, cb, delay_task](hio_t* io) {
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return;
                }
                //立即关闭tcp服务器
                strong_self->_poller->killTimer(delay_task);
                strong_self->_socket = std::make_shared<toolkit::Session>(io);
                strong_self->onConnect();
                cb(strong_self->_socket->get_local_port(), SockException());
                InfoL << "accept connection from:" << strong_self->_socket->peeraddr();
            });

            hio_t* io = tcp_listener->io();
            hevent_set_userdata(io, acceptCb);
            hio_setcb_accept(tcp_listener->io(), [](hio_t* io) {
                if (AcceptCB* pCB = (AcceptCB*)hevent_userdata(io)) {
                    (*pCB)(io);
                    delete pCB;
                }
            });
            hio_accept(tcp_listener->io());
            InfoL << "start tcp passive server on:" << tcp_listener->get_local_port();
        } catch (std::exception &ex) {
            cb(0, SockException(Err_other, ex.what()));
            return;
        }
        return;
    }
    if (args.is_udp) {
        auto poller = _poller;
        WebRtcServer::Instance().getPoller()->async([cb, args, weak_self, poller]() {
            sockaddr_u addr;
            // 切换线程目的是为了dns解析放在后台线程执行
            if (!ResolveAddr(args.dst_url.data(), &addr)) {
                poller->async([args, cb]() {
                    //切回自己的线程
                    cb(0, SockException(Err_dns, StrPrinter << "dns解析域名失败:" << args.dst_url));
                });
                return;
            }
            sockaddr_set_port(&addr, args.dst_port);
            //dns解析成功
            poller->async([args, addr, weak_self, cb]() {
                //切回自己的线程
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return;
                }
                string ifr_ip = addr.sa.sa_family == AF_INET ? "0.0.0.0" : "::";
                try {
                    if (args.src_port) {
                        //指定端口
                        hio_t* io = hloop_create_udp_server(hv::tlsEventLoop()->loop(), ifr_ip.data(), args.src_port);
                        if (!io) {
                            throw std::invalid_argument(StrPrinter << "bindUdpSock failed on port:" << args.src_port);
                        }
                        strong_self->_socket = std::make_shared<toolkit::Session>(io);
                    } else {
                        toolkit::Session::Ptr pr[2];
                        //从端口池获取随机端口
                        makeSockPair(pr, ifr_ip, true);
                        strong_self->_socket = pr[0];
                    }
                } catch (std::exception &ex) {
                    cb(0, SockException(Err_other, ex.what()));
                    return;
                }
                hio_set_peeraddr(strong_self->_socket->io(), (struct sockaddr *)&addr, sockaddr_len(&addr));
                strong_self->onConnect();
                cb(strong_self->_socket->get_local_port(), SockException());
            });
        });
    } else {
        hio_t* io = hio_create_socket(_poller->loop(), "::", args.src_port);
        _socket = std::make_shared<toolkit::Session>(io);
        _socket->onconnect = [cb, weak_self]() {
            SockException err;
            auto strong_self = weak_self.lock();
            if (strong_self) {
                if (!err) {
                    //tcp连接成功
                    strong_self->onConnect();
                }
                cb(strong_self->_socket->get_local_port(), err);
            } else {
                cb(0, err);
            }
        };//, 5.0F, "::", args.src_port);
        _socket->setConnectTimeout(5000);
        _socket->onclose = [cb, weak_self]() {
            auto err = SockException(Err_eof, "socket closed");
            auto strong_self = weak_self.lock();
            if (strong_self) {
                cb(strong_self->_socket->get_local_port(), err);
            }
        };
        // connect(args.dst_url, args.dst_port , 5.0F, "::", args.src_port);
        _socket->startConnect(args.dst_port, args.dst_url.c_str());
    }
}

void RtpSender::onConnect(){
    _is_connect = true;
    /*
    //加大发送缓存,防止udp丢包之类的问题
    SockUtil::setSendBuf(_socket->rawFD(), 4 * 1024 * 1024);
    if (!_args.is_udp) {
        //关闭tcp no_delay并开启MSG_MORE, 提高发送性能
        SockUtil::setNoDelay(_socket->rawFD(), false);
        _socket->setSendFlags(SOCKET_DEFAULE_FLAGS | FLAG_MORE);
    }
    */
    //连接建立成功事件
    std::weak_ptr<RtpSender> weak_self = shared_from_this();
    _socket->onclose = [weak_self]() {
        auto strong_self = weak_self.lock();
        if (strong_self) {
            strong_self->onErr(SockException(Err_eof, "socket closed"));
        }
    };
    //获取本地端口，断开重连后确保端口不变
    _args.src_port = _socket->get_local_port();
    InfoL << "开始发送 rtp:" << _socket->get_peer_ip() << ":" << _socket->get_peer_port() << ", 是否为udp方式:" << _args.is_udp;
}

bool RtpSender::addTrack(const Track::Ptr &track){
    return _interface->addTrack(track);
}

void RtpSender::addTrackCompleted(){
    _interface->addTrackCompleted();
}

void RtpSender::resetTracks(){
    _interface->resetTracks();
}

//此函数在其他线程执行
bool RtpSender::inputFrame(const Frame::Ptr &frame) {
    //连接成功后才做实质操作(节省cpu资源)
    return _is_connect ? _interface->inputFrame(frame) : false;
}

//此函数在其他线程执行
void RtpSender::onFlushRtpList(shared_ptr<std::list<Buffer::Ptr> > rtp_list) {
    if(!_is_connect){
        //连接成功后才能发送数据
        return;
    }

    auto is_udp = _args.is_udp;
    auto socket = _socket;
    _poller->async([rtp_list, is_udp, socket]() {
        size_t i = 0;
        auto size = rtp_list->size();
        for (auto packet : *rtp_list) {
            if (is_udp) {
                //udp模式，rtp over tcp前4个字节可以忽略
                socket->write(packet->data() + 4, packet->size() - 4);
                //socket->send(std::make_shared<BufferRtp>(std::move(packet), 4), nullptr, 0, ++i == size);
            } else {
                // tcp模式, rtp over tcp前2个字节可以忽略,只保留后续rtp长度的2个字节
                // socket->send(std::make_shared<BufferRtp>(std::move(packet), 2), nullptr, 0, ++i == size);
                socket->write(packet->data() + 2, packet->size() - 2);
            }
        }
    });
}

void RtpSender::onErr(const SockException &ex, bool is_connect) {
    _is_connect = false;

    if (_args.passive) {
        WarnL << "tcp passive connection lost: " << ex.what();
    } else {
        //监听socket断开事件，方便重连
        if (is_connect) {
            WarnL << "重连" << _args.dst_url << ":" << _args.dst_port << "失败, 原因为:" << ex.what();
        } else {
            WarnL << "停止发送 rtp:" << _args.dst_url << ":" << _args.dst_port << ", 原因为:" << ex.what();
        }
    }

    weak_ptr<RtpSender> weak_self = shared_from_this();
    _connect_timer = std::make_shared<Timer>(10.0f, [weak_self]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return false;
        }
        strong_self->startSend(strong_self->_args, [weak_self](uint16_t local_port, const SockException &ex){
            auto strong_self = weak_self.lock();
            if (strong_self && ex) {
                //连接失败且本对象未销毁，那么重试连接
                strong_self->onErr(ex, true);
            }
        });
        return false;
    }, _poller);
}

}//namespace mediakit
#endif// defined(ENABLE_RTPPROXY)