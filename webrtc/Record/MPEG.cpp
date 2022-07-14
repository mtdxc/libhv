/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <assert.h>
#include "MPEG.h"

#if defined(ENABLE_HLS) || defined(ENABLE_RTPPROXY)
#include "Ap4.h"
#include "Extension/Factory.h"
namespace mediakit{

MpegMuxer::MpegMuxer(bool is_ps) {
    _is_ps = is_ps;
    createContext();
}

MpegMuxer::~MpegMuxer() {
    releaseContext();
}

#define XX(name, type, value, str, mpeg_id)                                     \
    case name : {                                                               \
        if (mpeg_id == 0) break;                                                \
        stream_type = mpeg_id;                                                  \
        break;                                                                  \
    }

bool MpegMuxer::addTrack(const Track::Ptr &track) {
    unsigned int stream_id   = 0;
    unsigned int stream_type = 0;
    switch (track->getCodecId()) {
        CODEC_MAP(XX)
        default: break;
    }
    std::shared_ptr<AP4_SampleDescription> desc;
    AP4_Mpeg2TsWriter::SampleStream* stream = nullptr;
    int timeScale = 90000;
    if (track->getTrackType() == TrackVideo) {
        stream_id   = AP4_MPEG2_TS_DEFAULT_STREAM_ID_VIDEO;
        _writer.SetVideoStream(timeScale, stream_type, stream_id, stream);
        auto video = std::dynamic_pointer_cast<VideoTrack>(track);
        desc = Factory::getAP4Descripion(track);
        _have_video = true;
    } else {
        stream_id   = AP4_MPEG2_TS_DEFAULT_STREAM_ID_AUDIO;
        _writer.SetAudioStream(timeScale, stream_type, stream_id, stream);
        auto audio = std::dynamic_pointer_cast<AudioTrack>(track);
        desc = Factory::getAP4Descripion(track);
    }
    if (stream) {
        //@todo fill desc
        TsStream stm = { stream, desc };
        _streams[track->getCodecId()] = stm;
        return true;
    }
    WarnL << "忽略不支持该编码格式:" << track->getCodecName();
    return false;
}
#undef XX

void MpegMuxer::writeHeader() {
    if (_write_header) return;
    _writer.WritePAT(*_buffer);
    _writer.WritePMT(*_buffer);
    flushCache();
    _write_header = true;
}

bool MpegMuxer::inputFrame(const Frame::Ptr &frame) {
    auto it = _streams.find(frame->getCodecId());
    if (it == _streams.end()) {
        return false;
    }
    writeHeader();
    auto stream = it->second;
    _key_pos = !_have_video;
    switch (frame->getCodecId()) {
        case CodecH264:
        case CodecH265: {
            //这里的代码逻辑是让SPS、PPS、IDR这些时间戳相同的帧打包到一起当做一个帧处理，
            return _frame_merger.inputFrame(frame, [&](uint32_t dts, uint32_t pts, const Buffer::Ptr &buffer, bool have_idr) {
                _key_pos = have_idr;
                //取视频时间戳为TS的时间戳
                _timestamp = (uint32_t) dts;
                //_max_cache_size = 512 + 1.2 * buffer->size();
                //mpeg_muxer_input((::mpeg_muxer_t *)_context, track_id, have_idr ? 0x0001 : 0, pts * 90LL, dts * 90LL, buffer->data(), buffer->size());
                AP4_Sample sample;
                sample.SetDts(dts * 90LL);
                sample.SetCts(pts * 90LL);
                sample.SetSync(have_idr);
                AP4_DataBuffer buf((AP4_UI08*)buffer->data(), buffer->size());
                stream.stream->WriteSample(sample, buf, stream.desc.get(), false, *_buffer);
                flushCache();
            });
        }

        case CodecAAC: {
            if (frame->prefixSize() == 0) {
                WarnL << "必须提供adts头才能mpeg-ts打包";
                return false;
            }
        }

        default: {
            if (!_have_video) {
                //没有视频时，才以音频时间戳为TS的时间戳
                _timestamp = (uint32_t) frame->dts();
            }
            //_max_cache_size = 512 + 1.2 * frame->size();
            //mpeg_muxer_input((::mpeg_muxer_t *)_context, track_id, frame->keyFrame() ? 0x0001 : 0, frame->pts() * 90LL, frame->dts() * 90LL, frame->data(), frame->size());
            AP4_Sample sample;
            sample.SetDts(frame->dts() * 90LL);
            sample.SetCts(frame->pts() * 90LL);
            sample.SetSync(frame->keyFrame());
            AP4_DataBuffer buf((AP4_UI08*)frame->data(), frame->size());
            stream.stream->WriteSample(sample, buf, stream.desc.get(), false, *_buffer);
            flushCache();
            return true;
        }
    }
}

void MpegMuxer::resetTracks() {
    _have_video = false;
    //通知片段中断
    onWrite(nullptr, _timestamp, false);
    releaseContext();
    createContext();
}

void MpegMuxer::createContext()
{
    _buffer = new AP4_MemoryByteStream();
}

void MpegMuxer::releaseContext() {
    _streams.clear();
    _frame_merger.clear();
    _buffer->Release();
    _buffer = nullptr;
    _write_header = false;
}

void MpegMuxer::flushCache()
{
    AP4_Position pos;
    _buffer->Tell(pos);
    if (pos) {
        auto buf = BufferRaw::create();
        buf->assign((const char*)_buffer->GetData(), pos);
        this->onWrite(buf, _timestamp, _key_pos);
        _key_pos = false;
        _buffer->Seek(0);
    }
}

}//mediakit

#endif