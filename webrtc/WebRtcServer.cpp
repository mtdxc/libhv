/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "IceServer.hpp"
#include "WebRtcServer.h"
#include "WebRtcTransport.h"
#include "hstring.h"
#include "hbase.h"
#include "Common/config.h"
using toolkit::mINI;
 //RTC配置项目
namespace RTC {
#define RTC_FIELD "rtc."
//rtp和rtcp接受超时时间
const std::string kDocRoot = RTC_FIELD"docRoot";
//服务器外网ip
const std::string kHttpPort = RTC_FIELD"httpPort";
//设置remb比特率，非0时关闭twcc并开启remb。该设置在rtc推流时有效，可以控制推流画质
const std::string kHttpsPort = RTC_FIELD"httpsPort";
const std::string kCertFile = RTC_FIELD"certFile";
const std::string kKeyFile = RTC_FIELD"keyFile";

static onceToken token([]() {
    mINI::Instance()[kDocRoot] = "html";
    mINI::Instance()[kCertFile] = "cert/server.crt";
    mINI::Instance()[kKeyFile] = "cert/server.key";
    mINI::Instance()[kHttpPort] = 80;
    mINI::Instance()[kHttpsPort] = 443;
});
}//namespace RTC

using namespace mediakit;
using toolkit::mINI;
static std::string getUserName(void* buf, int len) {
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

//////////////////////////////////////////////////////////////////////////////////
WebRtcServer::WebRtcServer() : _udp(NULL, false){
}

WebRtcServer::~WebRtcServer() {
    stop();
}

WebRtcServer& WebRtcServer::Instance()
{
    static WebRtcServer instance;
    return instance;
}

void WebRtcServer::start()
{
    startUdp();
    startHttp();
}

void WebRtcServer::stop()
{
    websocket_server_stop(&_http);
    _udp.stop();
}

void WebRtcServer::detect_local_ip()
{
    sockaddr_u localAddr, remoteAddr;
    sockaddr_set_ipport(&remoteAddr, "8.8.8.8", 1234);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    connect(fd, (sockaddr*)&remoteAddr, sockaddr_len(&remoteAddr));
    int len = sizeof(localAddr);
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

void SockInfoToJson(hv::Json& val, toolkit::SockInfo* info) {
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

void WebRtcServer::startHttp()
{
    GET_CONFIG(std::string, api_secret, "api.secret");
    int port = 80;
    static HttpService http;
    http.document_root = mINI::Instance()[RTC::kDocRoot];
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


    static WebSocketService ws;
#if 0
    ws.onopen = [](const WebSocketChannelPtr& channel, const std::string& url) {
        printf("onopen: GET %s\n", url.c_str());
        MyContext* ctx = channel->newContext<MyContext>();
        // send(time) every 1s
        ctx->timerID = setInterval(1000, [channel](TimerID id) {
            if (channel->isConnected() && channel->isWriteComplete()) {
                char str[DATETIME_FMT_BUFLEN] = { 0 };
                datetime_t dt = datetime_now();
                datetime_fmt(&dt, str);
                channel->send(str);
            }
        });
    };
    ws.onmessage = [](const WebSocketChannelPtr& channel, const std::string& msg) {
        MyContext* ctx = channel->getContext<MyContext>();
        ctx->handleMessage(msg);
    };
    ws.onclose = [](const WebSocketChannelPtr& channel) {
        printf("onclose\n");
        MyContext* ctx = channel->getContext<MyContext>();
        if (ctx->timerID != INVALID_TIMER_ID) {
            killTimer(ctx->timerID);
        }
        channel->deleteContext<MyContext>();
    };
    _http.set
#endif

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
    _http.share_pools = _udp.loops();
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
    CHECK(_transport);
    try {
        _transport->inputSockData((char*)buf->data(), buf->size(), _peer_addr);
    }
    catch (...) {
    }
}

WebRtcSession::WebRtcSession(hio_t* io) : hv::SocketChannel(io)
{
    _peer_addr = hio_peeraddr(io);
    this->onconnect = []() {
    };

    this->onclose = [this]() {
    };

    this->onread = [this](hv::Buffer* buf) {
        onRecv(buf);
    };
}

void WebRtcServer::startUdp()
{
    GET_CONFIG(uint16_t, local_port, RTC::kPort);
    int listenfd = _udp.createsocket(local_port);
    if (listenfd < 0) {
        return;
    }
    LOGI("udp listen on %d, listenfd=%d ...\n", local_port, listenfd);

    _udp.onNewClient = [this](hio_t* io, hv::Buffer* data) {
        // @todo select loop with data or peerAddr
        auto user_name = getUserName(data->data(), data->size());
        if (user_name.empty()) {
            return;
        }
        if(auto ret = getItem(user_name))
            _udp.accept(io, ret->getPoller());
    };
    _udp.onMessage = [this](const std::shared_ptr<WebRtcSession> & session, hv::Buffer* buf) {
        session->onRecv(buf);
    };
    _udp.setThreadNum(4);
    _udp.setLoadBalance(LB_LeastConnections);

    _udp.start();
}

//////////////////////////////////////////////////////////////////////////
int main(int argc, char** argv) {
    char path[200];
    char* cfgPath = get_executable_path(path, sizeof(path));
    if (argc > 1) {
        cfgPath = argv[1];
    }
    else {
        char* dot = strrchr(path, '.');
        strcpy(dot, ".ini");
    }
    mediakit::loadIniConfig(cfgPath);

    hlog_set_level(LOG_LEVEL_DEBUG);
    hlog_set_handler(stdout_logger);

    WebRtcServer::Instance().start();

    printf("press any key to exit\n");
    getchar();
    
    WebRtcServer::Instance().stop();
}