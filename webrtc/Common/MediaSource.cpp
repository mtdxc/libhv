﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "MediaSource.h"
#include "Util/util.h"
//#include "Network/sockutil.h"
//#include "Network/TcpSession.h"
#include "Util/NoticeCenter.h"
#ifdef ENABLE_MP4
#include "Record/MP4Reader.h"
#endif // ENABLE_MP4


using namespace std;

StatisticImp(mediakit::MediaSource);

namespace mediakit {

static recursive_mutex s_media_source_mtx;
using StreamMap = unordered_map<string/*strema_id*/, weak_ptr<MediaSource> >;
using AppStreamMap = unordered_map<string/*app*/, StreamMap>;
using VhostAppStreamMap = unordered_map<string/*vhost*/, AppStreamMap>;
using SchemaVhostAppStreamMap = unordered_map<string/*schema*/, VhostAppStreamMap>;
static SchemaVhostAppStreamMap s_media_source_map;

string getOriginTypeString(MediaOriginType type){
#define SWITCH_CASE(type) case MediaOriginType::type : return #type
    switch (type) {
        SWITCH_CASE(unknown);
        SWITCH_CASE(rtmp_push);
        SWITCH_CASE(rtsp_push);
        SWITCH_CASE(rtp_push);
        SWITCH_CASE(pull);
        SWITCH_CASE(ffmpeg_pull);
        SWITCH_CASE(mp4_vod);
        SWITCH_CASE(device_chn);
        SWITCH_CASE(rtc_push);
        SWITCH_CASE(srt_push);
        default : return "unknown";
    }
}

string MediaSource::getUrl() const {
    if(this == NullMediaSource) return "";
    return _schema + "://" + _vhost + "/" + _app + "/" + _stream_id;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////

MediaSource* const MediaSource::NullMediaSource = nullptr;

MediaSource::MediaSource(const string &schema, const string &vhost, const string &app, const string &stream_id){
    GET_CONFIG(bool, enableVhost, General::kEnableVhost);
    if (!enableVhost) {
        _vhost = DEFAULT_VHOST;
    } else {
        _vhost = vhost.empty() ? DEFAULT_VHOST : vhost;
    }
    _schema = schema;
    _app = app;
    _stream_id = stream_id;
    _create_stamp = time(NULL);
    _default_poller = hv::EventLoopThreadPool::Instance()->loop();
}

MediaSource::~MediaSource() {
    unregist();
}

const string& MediaSource::getSchema() const {
    return _schema;
}

const string& MediaSource::getVhost() const {
    return _vhost;
}

const string& MediaSource::getApp() const {
    //获取该源的id
    return _app;
}

const string& MediaSource::getId() const {
    return _stream_id;
}

std::shared_ptr<void> MediaSource::getOwnership() {
    if (_owned.test_and_set()) {
        //已经被所有
        return nullptr;
    }
    weak_ptr<MediaSource> weak_self = shared_from_this();
    //确保返回的Ownership智能指针不为空，0x01无实际意义
    return std::shared_ptr<void>((void *) 0x01, [weak_self](void *ptr) {
        auto strong_self = weak_self.lock();
        if (strong_self) {
            strong_self->_owned.clear();
        }
    });
}

int MediaSource::getBytesSpeed(TrackType type){
    if(type == TrackInvalid || type == TrackMax){
        return _speed[TrackVideo].getSpeed() + _speed[TrackAudio].getSpeed();
    }
    return _speed[type].getSpeed();
}

uint64_t MediaSource::getAliveSecond() const {
    //使用Ticker对象获取存活时间的目的是防止修改系统时间导致回退
    return _ticker.createdTime() / 1000;
}

vector<Track::Ptr> MediaSource::getTracks(bool ready) const {
    auto listener = _listener.lock();
    if(!listener){
        return vector<Track::Ptr>();
    }
    return listener->getMediaTracks(const_cast<MediaSource &>(*this), ready);
}

void MediaSource::setListener(const std::weak_ptr<MediaSourceEvent> &listener){
    _listener = listener;
}

std::weak_ptr<MediaSourceEvent> MediaSource::getListener(bool next) const{
    if (!next) {
        return _listener;
    }

    auto listener = dynamic_pointer_cast<MediaSourceEventInterceptor>(_listener.lock());
    if (!listener) {
        return _listener;
    }
    else{
        //获取被拦截前的对象
        auto next_obj = listener->getDelegate();
        return next_obj ? next_obj : _listener;
    }
}

int MediaSource::totalReaderCount(){
    auto listener = _listener.lock();
    if(!listener){
        return readerCount();
    }
    return listener->totalReaderCount(*this);
}

MediaOriginType MediaSource::getOriginType() const {
    auto listener = _listener.lock();
    if (!listener) {
        return MediaOriginType::unknown;
    }
    return listener->getOriginType(const_cast<MediaSource &>(*this));
}

string MediaSource::getOriginUrl() const {
    std::string ret;
    if (auto listener = _listener.lock())
        ret = listener->getOriginUrl(const_cast<MediaSource &>(*this));
    if (ret.empty())
        ret = getUrl();
    return ret;
}

std::shared_ptr<SockInfo> MediaSource::getOriginSock() const {
    auto listener = _listener.lock();
    if (!listener) {
        return nullptr;
    }
    return listener->getOriginSock(const_cast<MediaSource &>(*this));
}

bool MediaSource::seekTo(uint32_t stamp) {
    auto listener = _listener.lock();
    if(!listener){
        return false;
    }
    return listener->seekTo(*this, stamp);
}

bool MediaSource::pause(bool pause) {
    auto listener = _listener.lock();
    if (!listener) {
        return false;
    }
    return listener->pause(*this, pause);
}

bool MediaSource::speed(float speed) {
    auto listener = _listener.lock();
    if (!listener) {
        return false;
    }
    return listener->speed(*this, speed);
}

bool MediaSource::close(bool force) {
    auto listener = _listener.lock();
    if(!listener){
        return false;
    }
    return listener->close(*this,force);
}

int MediaSource::getLossRate(mediakit::TrackType type) {
    auto listener = _listener.lock();
    if (!listener) {
        return -1;
    }
    return listener->getLossRate(*this, type);
}

EventPoller::Ptr MediaSource::getOwnerPoller() {
    EventPoller::Ptr ret;
    auto listener = _listener.lock();
    if (listener) {
        ret = listener->getOwnerPoller(*this);
    }
    return ret ? ret : _default_poller;
}

void MediaSource::onReaderChanged(int size) {
    std::weak_ptr<MediaSource> weak_self = shared_from_this();
    getOwnerPoller()->runInLoop([weak_self, size]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        auto listener = strong_self->_listener.lock();
        if (listener) {
            listener->onReaderChanged(*strong_self, size);
        }
    });
}

bool MediaSource::setupRecord(Recorder::type type, bool start, const string &custom_path, size_t max_second){
    auto listener = _listener.lock();
    if (!listener) {
        WarnL << "未设置MediaSource的事件监听者，setupRecord失败:" << getUrl();
        return false;
    }
    return listener->setupRecord(*this, type, start, custom_path, max_second);
}

bool MediaSource::isRecording(Recorder::type type){
    auto listener = _listener.lock();
    if(!listener){
        return false;
    }
    return listener->isRecording(*this, type);
}

void MediaSource::startSendRtp(const MediaSourceEvent::SendRtpArgs &args, const std::function<void(uint16_t, const SockException &)> cb) {
    auto listener = _listener.lock();
    if (!listener) {
        cb(0, SockException(Err_other, "尚未设置事件监听器"));
        return;
    }
    return listener->startSendRtp(*this, args, cb);
}

bool MediaSource::stopSendRtp(const string &ssrc) {
    auto listener = _listener.lock();
    if (!listener) {
        return false;
    }
    return listener->stopSendRtp(*this, ssrc);
}

template<typename MAP, typename LIST, typename First, typename ...KeyTypes>
static void for_each_media_l(const MAP &map, LIST &list, const First &first, const KeyTypes &...keys) {
    if (first.empty()) {
        for (auto &pr : map) {
            for_each_media_l(pr.second, list, keys...);
        }
    }
    else {
        auto it = map.find(first);
        if (it != map.end()) {
            for_each_media_l(it->second, list, keys...);
        }
    }
}

template<typename MAP, typename LIST, typename First>
static void for_each_media_l(const MAP &map, LIST &list, const First &first) {
    if (first.empty()) {
        for (auto &pr : map) {
            if(auto ptr = pr.second.lock())
                list.emplace_back(std::move(ptr));
        }
    }
    else{
        auto it = map.find(first);
        if (it != map.end()) {
            if(auto ptr = it->second.lock())
                list.emplace_back(std::move(ptr));
        }
    }
}

void MediaSource::for_each_media(const function<void(const Ptr &src)> &cb,
                                 const string &schema,
                                 const string &vhost,
                                 const string &app,
                                 const string &stream) {
    deque<Ptr> src_list;
    {
        lock_guard<recursive_mutex> lock(s_media_source_mtx);
        for_each_media_l(s_media_source_map, src_list, schema, vhost, app, stream);
    }
    for (auto &src : src_list) {
        cb(src);
    }
}

static MediaSource::Ptr find_l(const string &schema, const string &vhost_in, const string &app, const string &id, bool from_mp4) {
    string vhost = vhost_in;
    GET_CONFIG(bool, enableVhost, General::kEnableVhost);
    if(vhost.empty() || !enableVhost){
        vhost = DEFAULT_VHOST;
    }

    if (app.empty() || id.empty()) {
        //如果未指定app与stream id，那么就是遍历而非查找，所以应该返回查找失败
        return nullptr;
    }

    MediaSource::Ptr ret;
    MediaSource::for_each_media([&](const MediaSource::Ptr &src) { ret = std::move(const_cast<MediaSource::Ptr &>(src)); }, schema, vhost, app, id);

    if(!ret && from_mp4 && schema != HLS_SCHEMA){
        //未找到媒体源，则读取mp4创建一个
        //播放hls不触发mp4点播(因为HLS也可以用于录像，不是纯粹的直播)
        ret = MediaSource::createFromMP4(schema, vhost, app, id);
    }
    return ret;
}

static void findAsync_l(const MediaInfo &info, const std::shared_ptr<Session> &session, bool retry,
                        const function<void(const MediaSource::Ptr &src)> &cb){
    auto src = find_l(info._schema, info._vhost, info._app, info._streamid, true);
    if (src || !retry) {
        cb(src);
        return;
    }

    GET_CONFIG(int, maxWaitMS, General::kMaxStreamWaitTimeMS);
    void *listener_tag = session.get();
    auto poller = session->getPoller();
    // 保证只会回调一次
    std::shared_ptr<atomic_flag> invoked(new atomic_flag{false});
    auto cb_once = [cb, invoked](const MediaSource::Ptr &src) {
        if (invoked->test_and_set()) {
            return;
        }
        cb(src);
    };

    auto on_timeout = poller->setTimeout(maxWaitMS, [cb_once, listener_tag](hv::TimerID tid) {
        // 最多等待一定时间，如在这个时间内，流还未注册上，则返回空
        NoticeCenter::Instance().delListener(listener_tag, Broadcast::kBroadcastMediaChanged);
        cb_once(nullptr);
        return 0;
    });

    auto cancel_all = [on_timeout, poller, listener_tag]() {
        //取消延时任务，防止多次回调
        poller->killTimer(on_timeout);
        // on_timeout->cancel();
        //取消媒体注册事件监听
        NoticeCenter::Instance().delListener(listener_tag, Broadcast::kBroadcastMediaChanged);
    };

    weak_ptr<Session> weak_session = session;
    auto on_register = [weak_session, info, cb_once, cancel_all, poller](BroadcastMediaChangedArgs) {
        if (!bRegist ||
            sender.getSchema() != info._schema ||
            sender.getVhost() != info._vhost ||
            sender.getApp() != info._app ||
            sender.getId() != info._streamid) {
            //不是自己感兴趣的事件，忽略之
            return;
        }

        poller->async([weak_session, cancel_all, info, cb_once]() {
            cancel_all();
            if (auto strong_session = weak_session.lock()) {
                //播发器请求的流终于注册上了，切换到自己的线程再回复
                DebugL << "收到媒体注册事件,回复播放器:" << info._schema << "/" << info._vhost << "/" << info._app << "/" << info._streamid;
                //再找一遍媒体源，一般能找到
                findAsync_l(info, strong_session, false, cb_once);
            }
        }, false);
    };

    //监听媒体注册事件
    NoticeCenter::Instance().addListener(listener_tag, Broadcast::kBroadcastMediaChanged, on_register);

    function<void()> close_player = [cb_once, cancel_all, poller]() {
        poller->async([cancel_all, cb_once]() {
            cancel_all();
            //告诉播放器，流不存在，这样会立即断开播放器
            cb_once(nullptr);
        });
    };
    //广播未找到流,此时可以立即去拉流，这样还来得及
    NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastNotFoundStream, info, *session, close_player);
}

void MediaSource::findAsync(const MediaInfo &info, const std::shared_ptr<Session> &session, const function<void (const Ptr &)> &cb) {
    return findAsync_l(info, session, true, cb);
}

MediaSource::Ptr MediaSource::find(const string &schema, const string &vhost, const string &app, const string &id, bool from_mp4) {
    return find_l(schema, vhost, app, id, from_mp4);
}

MediaSource::Ptr MediaSource::find(const string &vhost, const string &app, const string &stream_id, bool from_mp4) {
    MediaSource::Ptr ret = MediaSource::find(RTMP_SCHEMA, vhost, app, stream_id, from_mp4);
    if (!ret) {
        ret = MediaSource::find(RTSP_SCHEMA, vhost, app, stream_id, from_mp4);
        if (!ret) ret = MediaSource::find(HLS_SCHEMA, vhost, app, stream_id, from_mp4);
    }
    return ret;
}

void MediaSource::emitEvent(bool regist){
    if (auto listener = _listener.lock()) {
        listener->onRegist(*this, regist);
    }
    //触发广播
    NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastMediaChanged, regist, *this);
    InfoL << (regist ? "媒体注册:" : "媒体注销:") << getUrl();
}

void MediaSource::regist() {
    {
        //减小互斥锁临界区
        lock_guard<recursive_mutex> lock(s_media_source_mtx);
        auto &ref = s_media_source_map[_schema][_vhost][_app][_stream_id];
        auto src = ref.lock();
        if (src) {
            if (src.get() == this) {
                return;
            }
            //增加判断, 防止当前流已注册时再次注册
            throw std::invalid_argument("media source already existed:" + getUrl());
        }
        ref = shared_from_this();
    }
    emitEvent(true);
}

template<typename MAP, typename First, typename ...KeyTypes>
static bool erase_media_source(bool &hit, const MediaSource *thiz, MAP &map, const First &first, const KeyTypes &...keys) {
    auto it = map.find(first);
    if (it != map.end() && erase_media_source(hit, thiz, it->second, keys...)) {
        map.erase(it);
    }
    return map.empty();
}

template<typename MAP, typename First>
static bool erase_media_source(bool &hit, const MediaSource *thiz, MAP &map, const First &first) {
    auto it = map.find(first);
    if (it != map.end()) {
        auto src = it->second.lock();
        if (!src || src.get() == thiz) {
            //对象已经销毁或者对象就是自己，那么移除之
            map.erase(it);
            hit = true;
        }
    }
    return map.empty();
}

//反注册该源
bool MediaSource::unregist() {
    bool ret = false;
    {
        //减小互斥锁临界区
        lock_guard<recursive_mutex> lock(s_media_source_mtx);
        erase_media_source(ret, this, s_media_source_map, _schema, _vhost, _app, _stream_id);
    }

    if (ret) {
        emitEvent(false);
    }
    return ret;
}

/////////////////////////////////////MediaInfo//////////////////////////////////////

void MediaInfo::parse(const string &url_in){
    _full_url = url_in;
    string url = url_in;
    auto pos = url.find("?");
    if (pos != string::npos) {
        _param_strs = url.substr(pos + 1);
        url.erase(pos);
    }

    auto schema_pos = url.find("://");
    if (schema_pos != string::npos) {
        _schema = url.substr(0, schema_pos);
    } else {
        schema_pos = -3;
    }
    auto split_vec = split(url.substr(schema_pos + 3), "/");
    if (split_vec.size() > 0) {
        splitUrl(split_vec[0], _host, _port);
        _vhost = _host;
         if (_vhost == "localhost" || -1 == _vhost.find_first_not_of("[1234567890:.]")) {
            //如果访问的是localhost或ip，那么则为默认虚拟主机
            _vhost = DEFAULT_VHOST;
        }
    }
    if (split_vec.size() > 1) {
        _app = split_vec[1];
    }
    if (split_vec.size() > 2) {
        string stream_id;
        for (size_t i = 2; i < split_vec.size(); ++i) {
            stream_id.append(split_vec[i] + "/");
        }
        if (stream_id.back() == '/') {
            stream_id.pop_back();
        }
        _streamid = stream_id;
    }

    auto params = Parser::parseArgs(_param_strs);
    if (params.find(VHOST_KEY) != params.end()) {
        _vhost = params[VHOST_KEY];
    }

    GET_CONFIG(bool, enableVhost, General::kEnableVhost);
    if (!enableVhost || _vhost.empty()) {
        //如果关闭虚拟主机或者虚拟主机为空，则设置虚拟主机为默认
        _vhost = DEFAULT_VHOST;
    }
}

MediaSource::Ptr MediaSource::createFromMP4(const string &schema, const string &vhost, const string &app, const string &stream, const string &file_path , bool check_app){
    GET_CONFIG(string, appName, Record::kAppName);
    if (check_app && app != appName) {
        return nullptr;
    }
#ifdef ENABLE_MP4
    try {
        MP4Reader::Ptr pReader(new MP4Reader(vhost, app, stream, file_path));
        pReader->startReadMP4();
        return MediaSource::find(schema, vhost, app, stream);
    } catch (std::exception &ex) {
        WarnL << ex.what();
        return nullptr;
    }
#else
    WarnL << "创建MP4点播失败，请编译时打开\"ENABLE_MP4\"选项";
    return nullptr;
#endif //ENABLE_MP4
}

/////////////////////////////////////MediaSourceEvent//////////////////////////////////////

void MediaSourceEvent::onReaderChanged(MediaSource &sender, int size){
    if (size || totalReaderCount(sender)) {
        //还有人观看该视频，不触发关闭事件
        hv::killTimer(_async_close_timer);
        _async_close_timer = 0;
        return;
    }

    // 没人观看该视频源，表明该源可以关闭了
    GET_CONFIG(string, record_app, Record::kAppName);
    GET_CONFIG(int, stream_none_reader_delay, General::kStreamNoneReaderDelayMS);
    //如果mp4点播, 无人观看时我们强制关闭点播
    bool is_mp4_vod = sender.getApp() == record_app;

    std::weak_ptr<MediaSource> weak_sender = sender.shared_from_this();
    _async_close_timer = hv::setInterval(stream_none_reader_delay, [weak_sender, is_mp4_vod](hv::TimerID tID) {
        auto strong_sender = weak_sender.lock();
        if (!strong_sender) {
            //对象已经销毁
            hv::killTimer(tID);
            return false;
        }

        if (strong_sender->totalReaderCount()) {
            //还有人观看该视频，不触发关闭事件
            hv::killTimer(tID);
            return false;
        }

        if (!is_mp4_vod) {
            //直播时触发无人观看事件，让开发者自行选择是否关闭
            NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastStreamNoneReader, *strong_sender);
        } else {
            //这个是mp4点播，我们自动关闭
            WarnL << "MP4点播无人观看,自动关闭:" << strong_sender->getUrl();
            strong_sender->close(false);
        }
        hv::killTimer(tID);
        return false;
    });
}

string MediaSourceEvent::getOriginUrl(MediaSource &sender) const {
    return sender.getUrl();
}

MediaOriginType MediaSourceEventInterceptor::getOriginType(MediaSource &sender) const {
    auto listener = _listener.lock();
    if (!listener) {
        return MediaOriginType::unknown;
    }
    return listener->getOriginType(sender);
}

string MediaSourceEventInterceptor::getOriginUrl(MediaSource &sender) const {
    std::string ret;
    if (auto listener = _listener.lock())
        ret = listener->getOriginUrl(sender);
    if (ret.empty())
        ret = MediaSourceEvent::getOriginUrl(sender);
    return ret;
}

std::shared_ptr<SockInfo> MediaSourceEventInterceptor::getOriginSock(MediaSource &sender) const {
    auto listener = _listener.lock();
    if (!listener) {
        return nullptr;
    }
    return listener->getOriginSock(sender);
}

bool MediaSourceEventInterceptor::seekTo(MediaSource &sender, uint32_t stamp) {
    auto listener = _listener.lock();
    if (!listener) {
        return false;
    }
    return listener->seekTo(sender, stamp);
}

bool MediaSourceEventInterceptor::pause(MediaSource &sender, bool pause) {
    auto listener = _listener.lock();
    if (!listener) {
        return false;
    }
    return listener->pause(sender, pause);
}

bool MediaSourceEventInterceptor::speed(MediaSource &sender, float speed) {
    auto listener = _listener.lock();
    if (!listener) {
        return false;
    }
    return listener->speed(sender, speed);
}

bool MediaSourceEventInterceptor::close(MediaSource &sender, bool force) {
    auto listener = _listener.lock();
    if (!listener) {
        return false;
    }
    return listener->close(sender, force);
}

int MediaSourceEventInterceptor::totalReaderCount(MediaSource &sender) {
    auto listener = _listener.lock();
    if (!listener) {
        return sender.readerCount();
    }
    return listener->totalReaderCount(sender);
}

void MediaSourceEventInterceptor::onReaderChanged(MediaSource &sender, int size) {
    auto listener = _listener.lock();
    if (!listener) {
        MediaSourceEvent::onReaderChanged(sender, size);
    } else {
        listener->onReaderChanged(sender, size);
    }
}

void MediaSourceEventInterceptor::onRegist(MediaSource &sender, bool regist) {
    auto listener = _listener.lock();
    if (listener) {
        listener->onRegist(sender, regist);
    }
}

int MediaSourceEventInterceptor::getLossRate(MediaSource &sender, TrackType type){
    auto listener = _listener.lock();
    if (listener) {
        return listener->getLossRate(sender, type);
    }
    return -1; //异常返回-1
}

EventPoller::Ptr MediaSourceEventInterceptor::getOwnerPoller(MediaSource &sender) {
    auto listener = _listener.lock();
    if (listener) {
        return listener->getOwnerPoller(sender);
    }
    return nullptr;
}


bool MediaSourceEventInterceptor::setupRecord(MediaSource &sender, Recorder::type type, bool start, const string &custom_path, size_t max_second) {
    auto listener = _listener.lock();
    if (!listener) {
        return false;
    }
    return listener->setupRecord(sender, type, start, custom_path, max_second);
}

bool MediaSourceEventInterceptor::isRecording(MediaSource &sender, Recorder::type type) {
    auto listener = _listener.lock();
    if (!listener) {
        return false;
    }
    return listener->isRecording(sender, type);
}

vector<Track::Ptr> MediaSourceEventInterceptor::getMediaTracks(MediaSource &sender, bool trackReady) const {
    auto listener = _listener.lock();
    if (!listener) {
        return vector<Track::Ptr>();
    }
    return listener->getMediaTracks(sender, trackReady);
}

void MediaSourceEventInterceptor::startSendRtp(MediaSource &sender, const MediaSourceEvent::SendRtpArgs &args, const std::function<void(uint16_t, const SockException &)> cb) {
    if (auto listener = _listener.lock()) {
        listener->startSendRtp(sender, args, cb);
    } else {
        MediaSourceEvent::startSendRtp(sender, args, cb);
    }
}

bool MediaSourceEventInterceptor::stopSendRtp(MediaSource &sender, const string &ssrc){
    auto listener = _listener.lock();
    if (!listener) {
        return false;
    }
    return listener->stopSendRtp(sender, ssrc);
}

void MediaSourceEventInterceptor::setDelegate(const std::weak_ptr<MediaSourceEvent> &listener) {
    if (listener.lock().get() == this) {
        throw std::invalid_argument("can not set self as a delegate");
    }
    _listener = listener;
}

std::shared_ptr<MediaSourceEvent> MediaSourceEventInterceptor::getDelegate() const{
    return _listener.lock();
}

/////////////////////////////////////FlushPolicy//////////////////////////////////////

static bool isFlushAble_default(bool is_video, uint64_t last_stamp, uint64_t new_stamp, size_t cache_size) {
    if (new_stamp + 500 < last_stamp) {
        //时间戳回退比较大(可能seek中)，由于rtp中时间戳是pts，是可能存在一定程度的回退的
        return true;
    }

    //时间戳发送变化或者缓存超过1024个,sendmsg接口一般最多只能发送1024个数据包
    return last_stamp != new_stamp || cache_size >= 1024;
}

static bool isFlushAble_merge(bool is_video, uint64_t last_stamp, uint64_t new_stamp, size_t cache_size, int merge_ms) {
    if (new_stamp + 500 < last_stamp) {
        //时间戳回退比较大(可能seek中)，由于rtp中时间戳是pts，是可能存在一定程度的回退的
        return true;
    }

    if (new_stamp > last_stamp + merge_ms) {
        //时间戳增量超过合并写阈值
        return true;
    }

    //缓存数超过1024个,这个逻辑用于避免时间戳异常的流导致的内存暴增问题
    //而且sendmsg接口一般最多只能发送1024个数据包
    return cache_size >= 1024;
}

bool FlushPolicy::isFlushAble(bool is_video, bool is_key, uint64_t new_stamp, size_t cache_size) {
    bool flush_flag = false;
    if (is_key && is_video) {
        //遇到关键帧flush掉前面的数据，确保关键帧为该组数据的第一帧，确保GOP缓存有效
        flush_flag = true;
    } else {
        GET_CONFIG(int, mergeWriteMS, General::kMergeWriteMS);
        if (mergeWriteMS <= 0) {
            //关闭了合并写或者合并写阈值小于等于0
            flush_flag = isFlushAble_default(is_video, _last_stamp[is_video], new_stamp, cache_size);
        } else {
            flush_flag = isFlushAble_merge(is_video, _last_stamp[is_video], new_stamp, cache_size, mergeWriteMS);
        }
    }

    if (flush_flag) {
        _last_stamp[is_video] = new_stamp;
    }
    return flush_flag;
}

} /* namespace mediakit */