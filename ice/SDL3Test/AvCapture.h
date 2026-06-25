#ifndef ICE_SDL3TEST_AVCAPTURE_H
#define ICE_SDL3TEST_AVCAPTURE_H

#include "SDLCapture.h"
#include "Transcode.h"
using mediakit::FFmpegEncoder;
using mediakit::FFmpegFrame;

class AvCapture : public SDLCapture, public FrameDispatcher {
    bool start_ = false;
    volatile bool req_key_ = true;
    Ticker ticker_;

    FFmpegEncoder::Ptr audio_enc, video_enc;
    AudioInfo ainfo;
    VideoInfo vinfo;
    uint32_t audio_device_ = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    uint32_t video_device_ = 0;

    void onPcm(short* pcm, int samples, int channel) override {
        if (audio_enc) {
            auto frame = FFmpegFrame::alloc();
            FFmpegFrame::initAudio(frame.get(), channel, getRecordSpec().freq, AV_SAMPLE_FMT_S16);
            frame->data[0] = (uint8_t *)pcm;
            frame->linesize[0] = samples * 2 * channel;
            frame->nb_samples = samples;
            frame->pts = ticker_.createdTime();
            audio_enc->inputFrame(frame, false);      
        }
    }
    void onYuv(uint8_t* yuv, int width, int height) override {
        if (video_enc) {
            auto frame = FFmpegFrame::allocPicture(AV_PIX_FMT_YUV420P, width, height);
            memcpy(frame->data[0], yuv, width * height * 3 / 2);
            frame->pts = ticker_.createdTime();
            if (req_key_) {
                req_key_ = false;
                frame->pict_type = AV_PICTURE_TYPE_I;
            }
            video_enc->inputFrame(frame, true);
        }
    }
    void onSizeChange(size_t size) override {
        if (size == 0) {
            stopAll();
        } else if(!start_) {
            start();
        }
    }
public:
    bool RequestKeyFrame() {
        req_key_ = true;
        return true;
    }
    void PrintDevices() {
        std::vector<Device> devs, adevs;
        getAudioCaputreDevice(adevs);
        getVideoCaptureDevice(devs);
        if (devs.empty() && adevs.empty()) {
            printf("no device found!\n");
            return;
        }
        printf("%zu microphone devices:\n", adevs.size());
        for (auto& dev: adevs) {
            printf("%u> %s\n", dev.id, dev.name.c_str());
        }
        printf("%zu camera devices:\n", devs.size());
        for (auto& dev: devs) {
            printf("%u> %s\n", dev.id, dev.name.c_str());
        }
    }
    
    bool setupAudio(CodecId id, int sampleRate, int channels, uint32_t deviceId = SDL_AUDIO_DEVICE_DEFAULT_RECORDING) {
        ainfo.sampleBit = 16;
        ainfo.sampleRate = sampleRate;
        ainfo.channel = channels;
        ainfo.codecId = id;
        ainfo.bitrate = 64000;
        audio_device_ = deviceId;
        return true;
    }
    bool setupVideo(CodecId id, int width, int height, int fps) {
        vinfo.width = width;
        vinfo.height = height;
        vinfo.frameRate = fps;
        vinfo.codecId = id;
        vinfo.bitRate = 512000;
        std::vector<Device> devs;
        if (getVideoCaptureDevice(devs)) {
            video_device_ = devs[0].id;
        }
        return true;
    }
    bool start() {
        if (ainfo.codecId != CodecInvalid && startAudioRecord(ainfo.sampleRate, ainfo.channel, audio_device_)) {
            ainfo.sampleBit = 16;
            auto& aspec = getRecordSpec();
            ainfo.sampleRate = aspec.freq;
            ainfo.channel = aspec.channels;
            TrackInfo info;
            info.audio = ainfo;
            audio_enc = std::make_shared<FFmpegEncoder>(info);
            audio_enc->setOnEncode([this](const Frame::Ptr &frame) {
                inputFrame(frame);
            });
            start_ = true;
        }

        if (vinfo.codecId != CodecInvalid && startCamera(vinfo.width, vinfo.height, vinfo.frameRate, video_device_)) {
            auto& vspec = getVideoSpec();
            vinfo.width = vspec.width;
            vinfo.height = vspec.height;
            vinfo.frameRate = vspec.framerate_numerator / vspec.framerate_denominator;
            TrackInfo info;
            info.video = vinfo;
            video_enc = std::make_shared<FFmpegEncoder>(info);
            video_enc->setOnEncode([this](const Frame::Ptr &frame) {
                inputFrame(frame);
            });
            start_ = true;
        }
        return start_;
    }

    void stopAll() {
        stopCamera();
        stopAudioRecord();
        video_enc = audio_enc = nullptr;
        start_ = false;
    }
};

#endif //ICE_SDL3TEST_AVCAPTURE_H