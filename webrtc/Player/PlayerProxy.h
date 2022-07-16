﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SRC_DEVICE_PLAYERPROXY_H_
#define SRC_DEVICE_PLAYERPROXY_H_

#include <memory>
#include "Common/MultiMediaSourceMuxer.h"
#include "Player/MediaPlayer.h"
#include "Util/TimeTicker.h"

namespace mediakit {
/// 将MediaPlay代理成 MediaSource, 并实现重试逻辑
class PlayerProxy : public MediaPlayer, public MediaSourceEvent, 
    public std::enable_shared_from_this<PlayerProxy> {
    std::function<void(const SockException &ex)> _on_close;
    std::function<void(const SockException &ex)> _on_play;
public:
    typedef std::shared_ptr<PlayerProxy> Ptr;

    PlayerProxy(const std::string &vhost, const std::string &app, const std::string &stream_id,
                const ProtocolOption &option,
                int retry_count = -1, ///< 重试次数， 如果 < 0 则一直重试播放；否则重试retry_count次数
                const EventPoller::Ptr &poller = nullptr);

    ~PlayerProxy() override;

    /**
     * 设置play结果回调
     * @note 只触发一次；在play后置空
     * @param cb 回调对象
     */
    void setPlayCallbackOnce(const std::function<void(const SockException &ex)> &cb)
    { _on_play = cb; }
    /**
     * 设置主动关闭回调
     * @param cb 回调对象
     */
    void setOnClose(const std::function<void(const SockException &ex)> &cb) {
        _on_close = cb ? cb : [](const SockException &) {};
    }

    /**
     * 开始拉流播放
     * @param strUrl
     */
    void play(const std::string &strUrl) override;

    /**
     * 获取观看总人数
     */
    int totalReaderCount() ;

private:
    // MediaSourceEvent override
    bool close(MediaSource &sender,bool force) override;
    int totalReaderCount(MediaSource &sender) override;
    MediaOriginType getOriginType(MediaSource &sender) const override;
    std::string getOriginUrl(MediaSource &sender) const override;
    std::shared_ptr<SockInfo> getOriginSock(MediaSource &sender) const override;
    EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;

    void rePlay(const std::string &strUrl,int iFailedCnt);
    void onPlaySuccess();
    void setDirectProxy();

private:
    ProtocolOption _option;
    int _retry_count;
    std::string _vhost;
    std::string _app;
    std::string _stream_id;
    std::string _pull_url;
    Timer::Ptr _timer;

    MultiMediaSourceMuxer::Ptr _muxer;
};

} /* namespace mediakit */
#endif /* SRC_DEVICE_PLAYERPROXY_H_ */