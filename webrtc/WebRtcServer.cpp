/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */
#include "WebRtcServer.h"
#include "webrtc/IceServer.hpp"
#include "webrtc/WebRtcTransport.h"
#include "srt/SrtTransport.hpp"
#include "FMP4/FMP4MediaSource.h"
#include "Rtmp/RtmpMediaSource.h"
#include "TS/TSMediaSource.h"
#include "hstring.h"
#include "hbase.h"
#include <string>
#include <iostream>
#include "Common/config.h"

using namespace std;
using namespace mediakit;
using namespace toolkit;
 //RTC配置项目
namespace RTC {
#define RTC_FIELD "rtc."
//服务器外网ip
const string kHttpPort = RTC_FIELD"httpPort";
//设置remb比特率，非0时关闭twcc并开启remb。该设置在rtc推流时有效，可以控制推流画质
const string kHttpsPort = RTC_FIELD"httpsPort";
const string kCertFile = RTC_FIELD"certFile";
const string kKeyFile = RTC_FIELD"keyFile";

static onceToken token([]() {
    mINI::Instance()[kCertFile] = "cert/server.crt";
    mINI::Instance()[kKeyFile] = "cert/server.key";
    mINI::Instance()[kHttpPort] = 80;
    mINI::Instance()[kHttpsPort] = 443;
});
}//namespace RTC
////////////RTSP服务器配置///////////
namespace Rtsp {
#define RTSP_FIELD "rtsp."
const string kPort = RTSP_FIELD"port";
const string kSSLPort = RTSP_FIELD"sslport";
onceToken token1([]() {
    mINI::Instance()[kPort] = 554;
    mINI::Instance()[kSSLPort] = 332;
}, nullptr);

} //namespace Rtsp

////////////RTMP服务器配置///////////
namespace Rtmp {
#define RTMP_FIELD "rtmp."
const string kPort = RTMP_FIELD"port";
const string kSSLPort = RTMP_FIELD"sslport";
onceToken token1([]() {
    mINI::Instance()[kPort] = 1935;
    mINI::Instance()[kSSLPort] = 19350;
}, nullptr);
} //namespace RTMP

//////////////////////////////////////////////////////////////////////////////////
WebRtcServer::WebRtcServer() {
}

WebRtcServer::~WebRtcServer() {
    stop();
}

WebRtcServer& WebRtcServer::Instance()
{
    static WebRtcServer instance;
    return instance;
}

hv::EventLoopPtr WebRtcServer::getPoller()
{
    return EventLoopThreadPool::Instance()->nextLoop();
}

void WebRtcServer::start()
{
    EventLoopThreadPool::Instance()->start();
    startRtc();
    startSrt();
    startHttp();
    startRtmp();
    startRtsp();
}

void WebRtcServer::stop()
{
    _rtsp = nullptr;
    _rtmp = nullptr;
    websocket_server_stop(&_http);
    _udpRtc = nullptr;
    _udpSrt = nullptr;
    EventLoopThreadPool::Instance()->stop();
}

void WebRtcServer::detect_local_ip()
{
    sockaddr_u localAddr, remoteAddr;
    sockaddr_set_ipport(&remoteAddr, "8.8.8.8", 1234);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    connect(fd, (sockaddr*)&remoteAddr, sockaddr_len(&remoteAddr));
    socklen_t len = sizeof(localAddr);
    getsockname(fd, (sockaddr*)&localAddr, &len);
    char ip[32];
    this->_local_ip = sockaddr_ip(&localAddr, ip, 32);
    LOGI("detect localAddr %s", ip);
}


void WebRtcServer::registerPlugin(const std::string &type, Plugin cb) {
    std::lock_guard<std::mutex> lck(_mtx_creator);
    _map_creator[type] = std::move(cb);
}

WebRtcTransportPtr WebRtcServer::getItem(const std::string &key) {
    WebRtcTransportImp::Ptr ret;
    if (key.empty()) {
        return ret;
    }
    std::lock_guard<std::mutex> lck(_mtx);
    auto it = _map.find(key);
    if (it != _map.end()) {
        ret = it->second.lock();
    }
    return ret;
}

using namespace mediakit;
#ifdef ENABLE_MEM_DEBUG
extern uint64_t getTotalMemUsage();
extern uint64_t getTotalMemBlock();
extern uint64_t getThisThreadMemUsage();
extern uint64_t getThisThreadMemBlock();
extern std::vector<size_t> getBlockTypeSize();
extern uint64_t getTotalMemBlockByType(int type);
extern uint64_t getThisThreadMemBlockByType(int type);
#endif

#include "Rtmp/Rtmp.h"
#include "Common/MultiMediaSourceMuxer.h"
void getStatisticJson(const std::function<void(hv::Json &val)> &cb) {
    hv::Json val;
    val["MediaSource"] = ObjectStatistic<MediaSource>::count();
    val["MultiMediaSourceMuxer"] = ObjectStatistic<MultiMediaSourceMuxer>::count();
    /*
    val["TcpServer"] = ObjectStatistic<TcpServer>::count();
    val["TcpSession"] = ObjectStatistic<TcpSession>::count();
    val["UdpServer"] = ObjectStatistic<UdpServer>::count();
    val["UdpSession"] = ObjectStatistic<UdpSession>::count();
    val["TcpClient"] = ObjectStatistic<TcpClient>::count();
    val["Socket"] = ObjectStatistic<Socket>::count());
    */
    val["FrameImp"] = ObjectStatistic<FrameImp>::count();
    val["Frame"] = ObjectStatistic<Frame>::count();

    val["Buffer"] = ObjectStatistic<Buffer>::count();
    val["BufferRaw"] = ObjectStatistic<BufferRaw>::count();
    val["BufferLikeString"] = ObjectStatistic<BufferLikeString>::count();
    //val["BufferList"] = ObjectStatistic<BufferList>::count();

    val["RtpPacket"] = ObjectStatistic<RtpPacket>::count();
    val["RtmpPacket"] = ObjectStatistic<RtmpPacket>::count();
#ifdef ENABLE_MEM_DEBUG
    auto bytes = getTotalMemUsage();
    val["totalMemUsage"] = bytes;
    val["totalMemUsageMB"] = (int)(bytes / 1024 / 1024);
    val["totalMemBlock"] = getTotalMemBlock();
    static auto block_type_size = getBlockTypeSize();
    {
        int i = 0;
        string str;
        size_t last = 0;
        for (auto sz : block_type_size) {
            str.append(std::to_string(last) + "~" + std::to_string(sz) + ":" + std::to_string(getTotalMemBlockByType(i++)) + ";");
            last = sz;
        }
        str.pop_back();
        val["totalMemBlockTypeCount"] = str;
    }

    auto thread_size = EventPollerPool::Instance().getExecutorSize() + WorkThreadPool::Instance().getExecutorSize();
    std::shared_ptr<std::vector<Value> > thread_mem_info = std::make_shared<std::vector<Value> >(thread_size);

    std::shared_ptr<void> finished(nullptr, [thread_mem_info, cb, obj](void *) {
        for (auto &val : *thread_mem_info) {
            (*obj)["threadMem"].append(val);
        }
        //触发回调
        cb(*obj);
    });

    auto pos = 0;
    auto lam0 = [&](TaskExecutor &executor) {
        auto &val = (*thread_mem_info)[pos++];
        executor.async([finished, &val]() {
            auto bytes = getThisThreadMemUsage();
            val["threadName"] = getThreadName();
            val["threadMemUsage"] = bytes;
            val["threadMemUsageMB"] = (bytes / 1024 / 1024);
            val["threadMemBlock"] = getThisThreadMemBlock();
            {
                int i = 0;
                string str;
                size_t last = 0;
                for (auto sz : block_type_size) {
                    str.append(std::to_string(last) + "~" + std::to_string(sz) + ":" + std::to_string(getThisThreadMemBlockByType(i++)) + ";");
                    last = sz;
                }
                str.pop_back();
                val["threadMemBlockTypeCount"] = str;
            }
            });
    };
    auto lam1 = [lam0](const TaskExecutor::Ptr &executor) {
        lam0(*executor);
    };
    EventPollerPool::Instance().for_each(lam1);
    WorkThreadPool::Instance().for_each(lam1);
#else
    cb(val);
#endif
}

template<typename Args, typename First>
bool checkArgs(Args &args, const First &first) {
    return !args->GetString(first).empty();
}

template<typename Args, typename First, typename ...KeyTypes>
bool checkArgs(Args &args, const First &first, const KeyTypes &...keys) {
    return checkArgs(args, first) && checkArgs(args, keys...);
}

//检查http url中或body中或http header参数是否为空的宏
#define CHECK_ARGS(...)  \
    if(!checkArgs(req, ##__VA_ARGS__)){ \
        throw SockException(Err_refused, "缺少必要参数:" #__VA_ARGS__); \
    }

//检查http参数中是否附带secret密钥的宏，127.0.0.1的ip不检查密钥
#define CHECK_SECRET() \
    if(req->client_addr.ip != "127.0.0.1"){ \
        if(api_secret != req->GetString("secret")){ \
            throw SockException(Err_refused, "secret错误"); \
        } \
    }

void SockInfoToJson(hv::Json& val, SockInfo* info) {
    val["local_addr"] = info->localaddr();
    val["peer_addr"] = info->peeraddr();
    //val["identifier"] = info->getIdentifier();
}

hv::Json makeMediaSourceJson(MediaSource &media) {
    hv::Json item;
    item["schema"] = media.getSchema();
    item[VHOST_KEY] = media.getVhost();
    item["app"] = media.getApp();
    item["stream"] = media.getId();
    item["createStamp"] = media.getCreateStamp();
    item["aliveSecond"] = media.getAliveSecond();
    item["bytesSpeed"] = media.getBytesSpeed();
    item["readerCount"] = media.readerCount();
    item["totalReaderCount"] = media.totalReaderCount();
    item["originType"] = (int)media.getOriginType();
    item["originTypeStr"] = getOriginTypeString(media.getOriginType());
    item["originUrl"] = media.getOriginUrl();
    item["isRecordingMP4"] = media.isRecording(Recorder::type_mp4);
    item["isRecordingHLS"] = media.isRecording(Recorder::type_hls);
    auto originSock = media.getOriginSock();
    if (originSock) {
        SockInfoToJson(item["originSock"], originSock.get());
    }
    else {
        item["originSock"] = nullptr;
    }

    //getLossRate有线程安全问题；使用getMediaInfo接口才能获取丢包率；getMediaList接口将忽略丢包率
    auto current_thread = media.getOwnerPoller()->isInLoopThread();
    for (auto &track : media.getTracks(false)) {
        hv::Json obj;
        auto codec_type = track->getTrackType();
        obj["codec_id"] = track->getCodecId();
        obj["codec_id_name"] = track->getCodecName();
        obj["ready"] = track->ready();
        obj["codec_type"] = codec_type;
        if (current_thread) {
            obj["loss"] = media.getLossRate(codec_type);
        }
        switch (codec_type) {
        case TrackAudio: {
            auto audio_track = std::dynamic_pointer_cast<AudioTrack>(track);
            obj["sample_rate"] = audio_track->getAudioSampleRate();
            obj["channels"] = audio_track->getAudioChannel();
            obj["sample_bit"] = audio_track->getAudioSampleBit();
            break;
        }
        case TrackVideo: {
            auto video_track = std::dynamic_pointer_cast<VideoTrack>(track);
            obj["width"] = video_track->getVideoWidth();
            obj["height"] = video_track->getVideoHeight();
            obj["fps"] = round(video_track->getVideoFps());
            break;
        }
        default:
            break;
        }
        item["tracks"].push_back(obj);
    }
    return item;
}

#include "Rtmp/FlvMuxer.h"
class HttpFlvMuxer : public FlvMuxer, public std::enable_shared_from_this<HttpFlvMuxer>{
public:
    HttpFlvMuxer(HttpResponseWriterPtr writer) : _writer(writer) {
    }
    HttpFlvMuxer(WebSocketChannelPtr channel) : _channel(channel) {
    }
    using FlvMuxer::start;
protected:
    WebSocketChannelPtr _channel;
    HttpResponseWriterPtr _writer;
    // 写入二进制数据
    virtual void onWrite(const toolkit::Buffer::Ptr &data, bool flush) {
        if(_writer)
            _writer->WriteBody(data->data(), data->size());
        if(_channel)
            _channel->send(data->data(), data->size());
    }
    // stop或RtmpMediaSource Detach时回调
    virtual void onDetach() {
        if(_writer)
            _writer->close();
        if(_channel)
            _channel->close();
    }
    // 为了跨线程投递, 为啥不 enable_shared_from_this?
    virtual std::shared_ptr<FlvMuxer> getSharedPtr() {
        return shared_from_this();
    }
};

static std::string schema_from_stream(std::string& stream) {
    std::string schema;
    if (hv::endswith(stream, ".live.flv")) {
        schema = RTMP_SCHEMA;
        stream = stream.substr(0, stream.length() - 9);
    }
    else if (hv::endswith(stream, ".live.mp4")) {
        schema = FMP4_SCHEMA;
        stream = stream.substr(0, stream.length() - 9);
    }
    else if (hv::endswith(stream, ".live.ts")) {
        schema = TS_SCHEMA;
        stream = stream.substr(0, stream.length() - 8);
    }
    return schema;
}

void WebRtcServer::startHttp()
{
    GET_CONFIG(std::string, api_secret, "api.secret");
    int port = 80;
    static HttpService http;
    http.document_root = mINI::Instance()[Http::kRootPath];
    InfoL << "www root:" << http.document_root;
    // http.error_page = mINI::Instance()[Http::kNotFound];
    http.POST("/echo", [](const HttpContextPtr& ctx) {
        return ctx->send(ctx->body(), ctx->type());
    });
    http.POST("/index/api/webrtc", [this](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
        onRtcOfferReq(req, writer);
    });
    http.GET("/index/api/getStatistic", [this](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
        getStatisticJson([writer](const hv::Json &data) mutable {
            HttpResponse resp;
            resp.Json(data);
            writer->WriteResponse(&resp);
        });
    });
    http.GET("/paths", [](HttpRequest* req, HttpResponse* resp) {
        return resp->Json(http.Paths());
    });
    http.Any("/index/api/getServerConfig", [](HttpRequest* req, HttpResponse* resp) -> int {
        CHECK_SECRET();
        hv::Json val;
        for (auto &pr : mINI::Instance()) {
            val[pr.first] = pr.second;
        }
        return resp->Json(val);
    });

    http.GET("/index/api/getMediaList", [](HttpRequest* req, HttpResponse* resp) -> int {
        CHECK_SECRET();
        hv::Json val;
        //获取所有MediaSource列表
        MediaSource::for_each_media([&](const MediaSource::Ptr &media) {
            val["data"].push_back(makeMediaSourceJson(*media));
        }, req->GetString("schema"), req->GetString("vhost"), req->GetString("app"), req->GetString("stream"));
        return resp->Json(val);
    });

    //测试url http://127.0.0.1/index/api/isMediaOnline?schema=rtsp&vhost=__defaultVhost__&app=live&stream=obs
    http.GET("/index/api/isMediaOnline", [](HttpRequest* req, HttpResponse* resp) -> int {
        CHECK_SECRET();
        CHECK_ARGS("schema", "vhost", "app", "stream");
        hv::Json val;
        val["online"] = (bool)(MediaSource::find(req->GetString("schema"), req->GetString("vhost"), req->GetString("app"), req->GetString("stream")));
        return resp->Json(val);
    });

    //测试url http://127.0.0.1/index/api/getMediaInfo?schema=rtsp&vhost=__defaultVhost__&app=live&stream=obs
    http.Any("/index/api/getMediaInfo", [this](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
        CHECK_SECRET();
        CHECK_ARGS("schema", "vhost", "app", "stream");
        auto src = MediaSource::find(req->GetString("schema"), req->GetString("vhost"), req->GetString("app"), req->GetString("stream"));
        if (!src) {
            throw SockException(Err_other, "can not find the stream");
        }
        src->getOwnerPoller()->async([=]() mutable {
            auto val = makeMediaSourceJson(*src);
            val["code"] = 0;
            writer->End(val.dump());
        });
    });

    //主动关断流，包括关断拉流、推流
    //测试url http://127.0.0.1/index/api/close_stream?schema=rtsp&vhost=__defaultVhost__&app=live&stream=obs&force=1
    http.Any("/index/api/close_stream", [this](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
        CHECK_SECRET();
        CHECK_ARGS("schema", "vhost", "app", "stream");
        //踢掉推流器
        auto src = MediaSource::find(req->GetString("schema"), req->GetString("vhost"), req->GetString("app"), req->GetString("stream"));
        if (!src) {
            throw SockException(Err_other, "can not find the stream");
        }

        bool force = req->Get<bool>("force");
        src->getOwnerPoller()->async([=]() mutable {
            bool flag = src->close(force);
            hv::Json val;
            val["result"] = flag ? 0 : -1;
            val["msg"] = flag ? "success" : "close failed";
            val["code"] = flag ? 0 : -1;
            writer->End(val.dump());
        });
    });

    // /:field/ => req->query_params["field"]
    http.GET("/:app/:stream", [this](const HttpContextPtr& context) -> int {
        auto req = context->request;
        auto writer = context->writer;
        InfoL << "stream: " << req->url;
        MediaInfo mi;
        //mi.parse(req->url);
        mi._host = req->host;
        mi._app = req->query_params["app"];
        mi._streamid = req->query_params["stream"];
        std::string stream = mi._streamid;

        auto schema = req->query_params["schema"];
        if (schema.empty()) {
            schema = schema_from_stream(mi._streamid);
        }
        mi._schema = schema;
        if (mi._app.empty() || mi._streamid.empty() || schema.empty()) {
            return -1;
            //url不合法
            writer->End("stream url error");
            return HTTP_STATUS_NOT_FOUND;
        }

        // 解析带上协议+参数完整的url
        // mi.parse(schema + "://" + req->host + req->path);
        MediaSource::findAsync(mi, writer, [schema, stream, writer](const MediaSource::Ptr &src){
            std::weak_ptr<hv::HttpResponseWriter> weak_self = writer;
            if (!src) {
                writer->WriteStatus(HTTP_STATUS_NOT_FOUND);
                writer->End("stream not found");
                return;
            }
            writer->response->SetContentTypeByFilename(stream.c_str());
            writer->EndHeaders();

            if (schema == RTMP_SCHEMA) {
                auto rtmp_src = std::dynamic_pointer_cast<RtmpMediaSource>(src);
                assert(rtmp_src);
                auto muxer = std::make_shared<HttpFlvMuxer>(writer);
                uint32_t start_pts = 0;//atoll(_parser.getUrlArgs()["starPts"].data());
                muxer->start(writer->getPoller(), rtmp_src, start_pts);
                writer->setContextPtr(muxer);
            }
            else if(schema == FMP4_SCHEMA) {
                auto fmp4_src = std::dynamic_pointer_cast<FMP4MediaSource>(src);
                assert(fmp4_src);
                //直播牺牲延时提升发送性能
                //setSocketFlags();

                // fmp4要先发initSegment
                writer->WriteBody(fmp4_src->getInitSegment());
                fmp4_src->pause(false);
                auto reader = fmp4_src->getRing()->attach(writer->getPoller());
                reader->setDetachCB([weak_self]() {
                    if (auto strong_self = weak_self.lock()) {
                        strong_self->close();//SockException(Err_shutdown, "fmp4 ring buffer detached"));
                    }
                });
                reader->setReadCB([weak_self](const FMP4MediaSource::RingDataType &fmp4_list) {
                    if (auto strong_self = weak_self.lock()) {
                        for (auto pkt : *fmp4_list) {
                            strong_self->WriteBody(pkt->data(), pkt->size());
                        }
                    }
                });
                writer->setContextPtr(reader);
            }
            else if(schema == TS_SCHEMA) {
                auto ts_src = std::dynamic_pointer_cast<TSMediaSource>(src);
                assert(ts_src);
                //直播牺牲延时提升发送性能
                //setSocketFlags();

                ts_src->pause(false);
                auto reader = ts_src->getRing()->attach(writer->getPoller());
                reader->setDetachCB([weak_self]() {
                    if (auto strong_self = weak_self.lock()) {
                        strong_self->close();//SockException(Err_shutdown, "ts ring buffer detached"));
                    }
                });
                reader->setReadCB([weak_self](const TSMediaSource::RingDataType &ts_list) {                    
                    if (auto strong_self = weak_self.lock()) {
                        for (auto pkt : *ts_list) {
                            strong_self->WriteBody(pkt->data(), pkt->size());
                        }
                    }
                });
                writer->setContextPtr(reader);
            }
        });
        return 0;
    });

    static WebSocketService ws;
    ws.onopen = [](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
        MediaInfo mi;
        mi.parse(req->url);
        std::string schema = mi._schema;
        if (schema.empty() || hv::startswith(schema, "ws")) {
            mi._schema = schema_from_stream(mi._streamid);
        }

        if (mi._app.empty() || mi._streamid.empty()) {
            //url不合法
            channel->close();
            return;
        }
        // 解析带上协议+参数完整的url
        // mi.parse(schema + "://" + req->host + req->path);
        MediaSource::findAsync(mi, channel, [schema, channel](const MediaSource::Ptr &src){
            std::weak_ptr<hv::WebSocketChannel> weak_self = channel;
            if (!src) {
                channel->close();
                return;
            }
            if (schema == RTMP_SCHEMA) {
                auto rtmp_src = std::dynamic_pointer_cast<RtmpMediaSource>(src);
                assert(rtmp_src);
                auto muxer = std::make_shared<HttpFlvMuxer>(channel);
                uint32_t start_pts = 0;//atoll(_parser.getUrlArgs()["starPts"].data());
                muxer->start(channel->getPoller(), rtmp_src, start_pts);
                channel->setContextPtr(muxer);
            }
            else if(schema == FMP4_SCHEMA) {
                auto fmp4_src = std::dynamic_pointer_cast<FMP4MediaSource>(src);
                assert(fmp4_src);
                //直播牺牲延时提升发送性能
                //setSocketFlags();

                // fmp4要先发initSegment
                channel->send(fmp4_src->getInitSegment(), WS_OPCODE_BINARY);
                fmp4_src->pause(false);
                auto reader = fmp4_src->getRing()->attach(channel->getPoller());
                reader->setDetachCB([weak_self]() {
                    if (auto strong_self = weak_self.lock()) {
                        strong_self->close();//SockException(Err_shutdown, "fmp4 ring buffer detached"));
                    }
                });
                reader->setReadCB([weak_self](const FMP4MediaSource::RingDataType &fmp4_list) {
                    if (auto strong_self = weak_self.lock()) {
                        for (auto pkt : *fmp4_list) {
                            strong_self->send(pkt->data(), pkt->size());
                        }
                    }
                });
                channel->setContextPtr(reader);
            }
            else if(schema == TS_SCHEMA) {
                auto ts_src = dynamic_pointer_cast<TSMediaSource>(src);
                assert(ts_src);
                //直播牺牲延时提升发送性能
                //setSocketFlags();

                ts_src->pause(false);
                auto reader = ts_src->getRing()->attach(channel->getPoller());
                reader->setDetachCB([weak_self]() {
                    if (auto strong_self = weak_self.lock()) {
                        strong_self->close();//SockException(Err_shutdown, "ts ring buffer detached"));
                    }
                });
                reader->setReadCB([weak_self](const TSMediaSource::RingDataType &ts_list) {
                    if (auto strong_self = weak_self.lock()) {
                        for (auto pkt : *ts_list) {
                            strong_self->send(pkt->data(), pkt->size());
                        }
                    }
                });
                channel->setContextPtr(reader);
            }
        });        
    };
    ws.onmessage = [](const WebSocketChannelPtr& channel, const std::string& msg) {
        // MyContext* ctx = channel->getContext<MyContext>();
        // ctx->handleMessage(msg);
    };
    ws.onclose = [](const WebSocketChannelPtr& channel) {
        channel->setContextPtr(nullptr);
    };

    _http.port = mINI::Instance()[RTC::kHttpPort];
    _http.https_port = mINI::Instance()[RTC::kHttpsPort];
    if (_http.https_port) {
        hssl_ctx_init_param_t param;
        memset(&param, 0, sizeof(param));
        auto key = mINI::Instance()[RTC::kKeyFile];
        auto cert = mINI::Instance()[RTC::kCertFile];
        param.crt_file = cert.c_str();
        param.key_file = key.c_str();
        param.endpoint = HSSL_SERVER;
        if (hssl_ctx_init(&param) == NULL) {
            fprintf(stderr, "hssl_ctx_init failed!\n");
        }
    }
    _http.service = &http;
    _http.ws = &ws;
    _http.share_pools = EventLoopThreadPool::Instance().get();
    websocket_server_run(&_http, 0);
}

void WebRtcServer::onRtcOfferReq(const HttpRequestPtr& req, const HttpResponseWriterPtr& writer)
{
    auto type = req->GetParam("type");
    auto offer = req->Body();
    CHECK(!offer.empty(), "http body(webrtc offer sdp) is empty");
    WebRtcArgs args = req->query_params;
    if (args.count("app") && args.count("stream")) {
        args["url"] = StrPrinter << RTC_SCHEMA << "://" << req->GetHeader("Host", DEFAULT_VHOST) << "/" << args["app"] << "/" << args["stream"];
            //<< "?" << args.getParser().Params() + "&session=" + _session_id;
    }
    Plugin plugin;
    {
        std::lock_guard<std::mutex> lck(_mtx_creator);
        auto it = _map_creator.find(type);
        if (it == _map_creator.end()) {
            HttpResponse resp;
            resp.Set("code", "exception");
            resp.Set("msg", "the type can not supported");
            WarnL << "onRtcOfferReq type unsupported: " << type;
            writer->WriteResponse(&resp);
            return;
        }
        plugin = it->second;
    }

    plugin(writer, offer, args,
        [writer, offer](const WebRtcInterface &exchanger) mutable {
            HttpResponse resp;
            //设置返回类型
            resp.SetHeader("Content-Type", "application/json");
            //设置跨域
            resp.SetHeader("Access-Control-Allow-Origin", "*");

            try {
                resp.Set("code", 0);
                resp.Set("sdp", const_cast<WebRtcInterface &>(exchanger).getAnswerSdp(offer));
                resp.Set("id", exchanger.getIdentifier());
                resp.Set("type", "answer");
            }
            catch (std::exception &ex) {
                WarnL << "onRtcOfferReq exception: " << ex.what();
                resp.Set("code", "exception");
                resp.Set("msg", ex.what());
            }
            writer->WriteResponse(&resp);
        });
}

void WebRtcServer::startRtc()
{
    GET_CONFIG(uint16_t, local_port, RTC::kPort);
    if (!local_port) return;

    _udpRtc = std::make_shared<hv::UdpServerEventLoopTmpl2<WebRtcSession>>();
    int listenfd = _udpRtc->createsocket(local_port);
    if (listenfd < 0) {
        _udpRtc = nullptr;
        return;
    }
    LOGI("listen rtc on %d, listenfd=%d ...\n", local_port, listenfd);

    _udpRtc->onNewClient = [this](hio_t* io, hv::Buffer* data) {
        // @todo select loop with data or peerAddr
        auto user_name = WebRtcSession::getUserName(data->data(), data->size());
        if (user_name.empty()) {
            return;
        }
        if(auto ret = getItem(user_name))
            _udpRtc->accept(io, ret->getPoller());
    };
    _udpRtc->onMessage = [this](const std::shared_ptr<WebRtcSession> & session, hv::Buffer* buf) {
        try {
            session->onRecv(buf);
        }
        catch (SockException &ex) {
            session->shutdown(ex);
        }
        catch (exception &ex) {
            session->shutdown(SockException(Err_shutdown, ex.what()));
        }
    };
    _udpRtc->setLoadBalance(LB_LeastConnections);

    _udpRtc->start();
}

void WebRtcServer::startSrt()
{
    GET_CONFIG(uint16_t, local_port, SRT::kPort);
    if (!local_port) return;

    _udpSrt = std::make_shared<hv::UdpServerEventLoopTmpl2<SRT::SrtSession>>();
    int listenfd = _udpSrt->createsocket(local_port);
    if (listenfd < 0) {
        _udpSrt = nullptr;
        return;
    }
    LOGI("listen srt on %d, listenfd=%d ...\n", local_port, listenfd);

    _udpSrt->onNewClient = [this](hio_t* io, hv::Buffer* data) {
        // @todo select loop with data or peerAddr
        auto new_poller = SRT::SrtSession::queryPoller((uint8_t*)data->data(), data->size());
        if (new_poller)
            _udpSrt->accept(io, new_poller);
        else 
            _udpSrt->accept(io, nullptr);
    };
    _udpSrt->onMessage = [this](const std::shared_ptr<SRT::SrtSession>& session, hv::Buffer* data) {
        try {
            session->onRecv((uint8_t*)data->data(), data->size());
        }
        catch (SockException &ex) {
            session->shutdown(ex);
        }
        catch (exception &ex) {
            session->shutdown(SockException(Err_shutdown, ex.what()));
        }
    };
    _udpSrt->setLoadBalance(LB_LeastConnections);
    _udpSrt->start();
}

void WebRtcServer::startRtmp()
{
    GET_CONFIG(uint16_t, port, ::Rtmp::kPort);
    if (port <= 0) return;
    _rtmp = std::make_shared<hv::TcpServerEventLoopTmpl<mediakit::RtmpSession>>();
    int listenfd = _rtmp->createsocket(port);
    if (listenfd < 0) {
        _rtmp = nullptr;
        return;
    }
    LOGI("listen rtmp on %d", port);
    _rtmp->onConnection = [](const std::shared_ptr<RtmpSession>& session) {
        if (session->isConnected()) {
        }
        else {
            session->onError(SockException(Err_eof, "socket closed"));
        }
    };
    _rtmp->onMessage = [](const std::shared_ptr<RtmpSession>& session, hv::Buffer* buf) {
        try {
            session->onRecv((char*)buf->data(), buf->size());
        }
        catch (SockException &ex) {
            session->shutdown(ex);
        }
        catch (exception &ex) {
            session->shutdown(SockException(Err_shutdown, ex.what()));
        }
    };
    _rtmp->setLoadBalance(LB_LeastConnections);
    _rtmp->start();
}

void WebRtcServer::startRtsp()
{
    GET_CONFIG(uint16_t, port, ::Rtsp::kPort);
    if (port <= 0) return;
    _rtsp = std::make_shared<hv::TcpServerEventLoopTmpl<mediakit::RtspSession>>();
    int listenfd = _rtsp->createsocket(port);
    if (listenfd < 0) {
        _rtsp = nullptr;
        return;
    }
    LOGI("listen rtsp on %d", port);
    _rtsp->onConnection = [](const std::shared_ptr<RtspSession>& session) {
        if (session->isConnected()) {
        }
        else {
            session->onError(SockException(Err_eof, "socket closed"));
        }
    };
    _rtsp->onMessage = [](const std::shared_ptr<RtspSession>& session, hv::Buffer* buf) {
        try {
            auto buffer = BufferRaw::create();
            buffer->assign((char*)buf->data(), buf->size());
            session->onRecv(buffer);
        }
        catch (SockException &ex) {
            session->shutdown(ex);
        }
        catch (exception &ex) {
            session->shutdown(SockException(Err_shutdown, ex.what()));
        }
    };
    _rtsp->setLoadBalance(LB_LeastConnections);
    _rtsp->start();
}

//////////////////////////////////////////////////////////////////////////
int main(int argc, char** argv) {
    const char* cfgPath = nullptr;
    if (argc > 1) {
        cfgPath = argv[1];
    }
    // AP4::Initialize();
    mediakit::loadIniConfig(cfgPath);

    hlog_set_level(LOG_LEVEL_DEBUG);
    hlog_set_handler(stdout_logger);

    WebRtcServer::Instance().start();

    std::string line, cmd;
    std::vector<std::string> cmds;
    printf("press quit key to exit\n");
    while (getline(cin, line))
    {
        cmds = hv::split(line);
        if (cmds.empty()) {
            continue;
        }
        cmd = cmds[0];
        if (cmd == "quit" || cmd == "q") {
            cout << "use quit app" << endl;
            break;
        }
        else if (cmd == "count" || cmd == "c") {
            getStatisticJson([](hv::Json& val) {
                cout << val.dump() << endl;
            });
        }
    }
    
    WebRtcServer::Instance().stop();
    // AP4::Terminate();
}