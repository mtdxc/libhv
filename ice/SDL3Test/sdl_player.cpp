/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <signal.h>
#include "hlog.h"
#include "config.h"
#include "rtc/RtcClient.h"
#include "Transcode.h"
#include "SDL3/SDL_Main.h"
#include "libavcodec/avcodec.h"
#include "SDLCapture.h"

using namespace std;
using namespace ice;
using namespace mediakit;

class YuvDisplayer {
public:
    using Ptr = std::shared_ptr<YuvDisplayer>;

    YuvDisplayer(void *hwnd = nullptr, const char *title = "untitled") {
        _title = title;
        _hwnd = hwnd;
    }

    virtual ~YuvDisplayer() {
        if (_texture) {
            SDL_DestroyTexture(_texture);
            _texture = nullptr;
        }
        if (_render) {
            SDL_DestroyRenderer(_render);
            _render = nullptr;
        }
        if (_win) {
            SDL_DestroyWindow(_win);
            _win = nullptr;
        }
    }

    bool displayYUV(AVFrame *pFrame) {
        if (!_win) {
            if (_hwnd) {
                SDL_PropertiesID props = SDL_CreateProperties();
                // 根据平台设置正确的属性
                const char *platform = SDL_GetPlatform();

                if (strcmp(platform, "Windows") == 0) {
                    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, _hwnd);
                } else if (strcmp(platform, "Linux") == 0) {
                    // 尝试 X11
                    // SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_X11_WINDOW_POINTER, _hwnd);
                } else if (strcmp(platform, "macOS") == 0) {
                    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_COCOA_WINDOW_POINTER, _hwnd);
                }
                _win = SDL_CreateWindowWithProperties(props);
                SDL_DestroyProperties(props);
            } else {
                _win = SDL_CreateWindow(_title.data(), pFrame->width, pFrame->height, SDL_WINDOW_OPENGL);
            }
        }
        if (_win && !_render) {
            _render = SDL_CreateRenderer(_win, "direct3d,opengl,software"); // "direct3d", "metal", "software", "opengl"
        }
        if (_render && (!_texture|| _texture->w != pFrame->width || _texture->h != pFrame->height)) {
            if (_texture) {
                SDL_DestroyTexture(_texture);
                _texture = nullptr;
            }
            if (pFrame->format == AV_PIX_FMT_NV12) {
                _texture = SDL_CreateTexture(_render, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STREAMING, pFrame->width, pFrame->height);
            } else {
                _texture = SDL_CreateTexture(_render, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, pFrame->width, pFrame->height);
            }
        }
        if (_texture) {
            if (pFrame->format == AV_PIX_FMT_NV12) {
                SDL_UpdateNVTexture(_texture, nullptr, 
                    pFrame->data[0], pFrame->linesize[0], 
                    pFrame->data[1], pFrame->linesize[1]);
            } else {
                SDL_UpdateYUVTexture(_texture, nullptr, 
                    pFrame->data[0], pFrame->linesize[0], 
                    pFrame->data[1], pFrame->linesize[1], 
                    pFrame->data[2], pFrame->linesize[2]);
            }

            // SDL_UpdateTexture(_texture, nullptr, pFrame->data[0], pFrame->linesize[0]);
            SDL_RenderClear(_render);
            SDL_RenderTexture(_render, _texture, nullptr, nullptr);
            SDL_RenderPresent(_render);
            return true;
        }
        return false;
    }

private:
    std::string _title;
    void *_hwnd = nullptr;
    SDL_Window *_win = nullptr;
    SDL_Renderer *_render = nullptr;
    SDL_Texture *_texture = nullptr;
};

class SDLPlayer : public WhepClient {
    FFmpegDecoder::Ptr _decoder[CodecMax];
    FFmpegSwr::Ptr swr;
    SDLCapture loop_;
public:
    SDLPlayer(const IceConfig* options) : WhepClient(options) {
    }

    void onStartWebRTC() override {
        WhepClient::onStartWebRTC();
        auto track = getTrack(TrackAudio);
        if (track) {
            TrackInfo info;
            info.audio.codecId = track->getCodec();
            info.audio.channel = track->plan_rtp->channel;
            info.audio.sampleRate = track->plan_rtp->sample_rate;
            loop_.startAudioPlay(info.audio.sampleRate, info.audio.channel);
            auto pcms = std::make_shared<PcmBuffer<short>>();
            loop_.setPcmFillCallback([pcms](short* pcm, int samples, int channel) { 
                pcms->Read(pcm, samples * channel);
            });
            auto dec = std::make_shared<FFmpegDecoder>(info);
            dec->setOnDecode([pcms, this](const FFmpegFrame::Ptr &frame) {
                if (!swr) {
#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
                    swr = std::make_shared<FFmpegSwr>(AV_SAMPLE_FMT_S16, &(frame->ch_layout), frame->sample_rate);
#else
                    swr = std::make_shared<FFmpegSwr>(AV_SAMPLE_FMT_S16, frame->channels, frame->channel_layout, frame->sample_rate);
#endif
                }
                auto pcm = swr->inputFrame(frame);
                auto len = pcm->nb_samples * FFmpegFrame::getChannels(pcm);// * av_get_bytes_per_sample((enum AVSampleFormat)pcm->format);
                pcms->Write((short*)pcm->data[0], len);
            });
            _decoder[track->getCodec()] = dec;
        }
        track = getTrack(TrackVideo);
        if (track) {
            auto displayer = std::make_shared<YuvDisplayer>(nullptr, url());
            TrackInfo info;
            info.video.codecId = track->getCodec();
            info.video.width = 640;
            info.video.height = 480;
            info.video.frameRate = 30;
            auto dec = std::make_shared<FFmpegDecoder>(info);
            dec->setOnDecode([this, displayer](const FFmpegFrame::Ptr &frame) {
                loop_.doTask([frame, displayer]() {
                    // sdl要求在main线程渲染
                    displayer->displayYUV(frame.get());
                    return true;
                });
            });
            _decoder[track->getCodec()] = dec;
        }
    }
    void onRecvFrame(MediaTrack &track, const std::string &rid, Frame::Ptr frame) override {
        if (auto dec = _decoder[frame->codec]) {
            dec->inputFrame(frame, true, false, false);
        }
    }
    void onClose() override {
        WhepClient::onClose();
        for (auto &dec : _decoder) {
            if (dec) {
                dec = nullptr;
            }
        }
        loop_.stopAudioPlay();
        loop_.shutdown();
    }
    void runLoop() {
        loop_.runLoop();
    }
};

std::shared_ptr<SDLPlayer> player;
int main(int argc, char *argv[]) {
#ifdef _WIN32
    // 1. 首先调用AllocConsole创建一个控制台窗口
    AllocConsole();

    // 2. 但此时调用cout或者printf都不能正常输出文字到窗口（包括输入流cin和scanf）, 所以需要如下重定向输入输出流：
    FILE *stream;
    freopen_s(&stream, "CON", "r", stdin); //重定向输入流
    freopen_s(&stream, "CON", "w", stdout); //重定向输入流

    // 清除流缓冲区, 在win11上还是无法输出文字，需要在加入如下代码
    std::cin.clear();
    std::cout.clear();

    // 3. 如果我们需要用到控制台窗口句柄，可以调用FindWindow取得：
    SetConsoleTitleA(argv[0]); //设置窗口名
#endif 
    //logger_enable_color(hlog, true);
    hlog_set_handler(stdout_logger);

    // 设置日志
    const char* url = "http://127.0.0.1:8080/index/api/whep?app=live&stream=test";
    if (argc > 1) {
        url = argv[1];
    }
    
    IceConfig config;
    player = std::make_shared<SDLPlayer>(&config);
    player->open(url);

    signal(SIGINT, [](int) { if(player) player->safeClose(); });
    player->runLoop();
    return 0;
}

