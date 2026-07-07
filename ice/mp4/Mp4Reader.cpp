/*
 * MIT License
 *
 * Copyright (c) 2016 xiongziliang <771730766@qq.com>
 *
 * This file is part of ZLMediaKit(https://github.com/xiongziliang/ZLMediaKit).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "hlog.h"
#include "Mp4Reader.h"
#include "H264NalParse.h"
#include "mov-reader.h"
#include "mov-format.h"
#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"

#define MP4_INVALID_TRACK_ID 0xFFFFFFFF

#if defined(_WIN32) || defined(_WIN64)
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#elif defined(__ANDROID__)
#define fseek64 fseek
#define ftell64 ftell
#elif defined(OS_LINUX)
#define fseek64 fseeko64
#define ftell64 ftello64
#else
#define fseek64 fseek
#define ftell64 ftell
#endif

int mov_read(void* fp, void* data, uint64_t bytes) {
  if (bytes == fread(data, 1, bytes, (FILE*)fp))
    return 0;
  return 0 != ferror((FILE*)fp) ? ferror((FILE*)fp) : -1 /*EOF*/;
}
int mov_write(void* fp, const void* data, uint64_t bytes) {
  return bytes == fwrite(data, 1, bytes, (FILE*)fp) ? 0 : ferror((FILE*)fp);
}

int mov_seek(void* param, int64_t offset) {
  return fseek64((FILE*)param, offset, offset >= 0 ? SEEK_SET : SEEK_END);
}
int64_t mov_tell(void* param) {
  return ftell64((FILE*)param);
}

mov_buffer_t* mov_get_file_buffer() {
  static mov_buffer_t io = {&mov_read, &mov_write, &mov_seek, &mov_tell};
  return &io;
}

Mp4Reader::Mp4Reader(hv::EventLoop* loop) : loop_(loop) {
  mp4_handle_ = nullptr;
  aCodec = vCodec = CodecInvalid;
}

Mp4Reader::~Mp4Reader() { Close(); }

bool Mp4Reader::Close() {
  bool ret = false;
  track_map_.clear();
  aCodec = vCodec = CodecInvalid;
  StopRead();
  if (mp4_handle_) {
    mov_reader_destroy(mp4_handle_);
    mp4_handle_ = nullptr;
    ret = true;
  }
  if (fp_) {
    fclose(fp_);
    fp_ = nullptr;
  }
  return ret;
}

bool Mp4Reader::Open(const char *path) {
  fp_ = fopen(path, "rb");
  if (!fp_) {
    hlogi("unable to open file %s", path);
    return false;
  }
  return Open(mov_get_file_buffer(), fp_);
}

bool Mp4Reader::Open(mov_buffer_t *provider, void* data) {
  mov_reader_t* handle = mov_reader_create(provider, data);
  if (!handle) {
    hlogi("unable to Mp4");
    return false;
  }

  mp4_handle_ = handle;
  static mov_reader_trackinfo_t w_on_track = {
    [](void* param, uint32_t track, uint8_t object, int width, int height,
        const void* extra, size_t bytes) {
      // onvideo
      Mp4Reader* thiz = (Mp4Reader*)param;
      thiz->onVideoTrack(track, object, width, height, extra, bytes);
    },
    [](void* param, uint32_t track, uint8_t object, int channel_count,
        int bit_per_sample, int sample_rate, const void* extra, size_t bytes) {
      // onaudio
        Mp4Reader* thiz = (Mp4Reader*)param;
      thiz->onAudioTrack(track, object, channel_count, bit_per_sample,
                          sample_rate, extra, bytes);
    },
    [](void* param, uint32_t track, uint8_t object, const void* extra,
        size_t bytes) {
      // onsubtitle, do nothing
    }};
  mov_reader_getinfo(handle, &w_on_track, this);
  
  if (!hasVideo() && !hasAudio()) {
    Close();
    return false;
  }

  duration_ = mov_reader_getduration(handle);
  return true;
}

void Mp4Reader::onSizeChange(size_t size) {
  if (size == 0) {
    StopRead();
  } else if (!timer_) {
    StartRead();
  }
}

bool Mp4Reader::StopRead() {
  if (timer_) {
    loop_->killTimer(timer_);
    timer_ = 0;
    return true;
  }
  return false;
}

void Mp4Reader::StartRead() {
  if (timer_ || !loop_) {
    return ;
  }
  timer_ = loop_->setTimeout(10, [this](hv::TimerID id) {
    int ret = ReadFrame();
    loop_->resetTimer(id, ret > 0 ? ret : 10);
    if (eof()) {
      Seek(0);
    }
  });  
}

int Mp4Reader::ReadFrame() {
  uint64_t tsp = frame_.dts;
  int ret = mov_reader_read2(
      mp4_handle_,
      [](void* param, uint32_t track, size_t bytes, int64_t pts, int64_t dts,
         int flags) {
        Mp4Reader* thiz = (Mp4Reader*)param;
        auto& frame = thiz->frame_;
        frame.codec = thiz->track_map_[track];
        frame.pts = pts;
        frame.dts = dts;
        frame.is_key = flags & MOV_AV_FLAG_KEYFREAME;
        frame.setSize(bytes);
        return (void*)frame.data();
      },
      this);

  switch (ret) {
  case 0:
    eof_ = true;
    return 0;

  case 1:
    if (frame_.codec == CodecH264 || frame_.codec == CodecH265 || frame_.codec == CodecH266) {
      uint32_t iOffset = 0;
      int bytes_ = frame_.size();
      uint8_t* pBytes = (uint8_t*)frame_.data();
      while (iOffset < bytes_) {
        int iFrameLen = BytesToUI32(pBytes + iOffset);
        if (iFrameLen + iOffset + 4 > bytes_) {
          break;
        }
        UI32ToBytes(pBytes + iOffset, 1);
        iOffset += (iFrameLen + 4);
      }
    }
    inputFrame(frame_.clone());
    return frame_.dts - tsp;
  default:
    eof_ = true;
    hlogi("Mp4Reader error %d", ret);
    return 0;
  }
}

int Mp4Reader::onAudioTrack(uint32_t track,
                            uint8_t object,
                            int channel_count,
                            int bit_per_sample,
                            int sample_rate,
                            const void* extra,
                            size_t bytes) {

  audio_channels = channel_count;
  audio_samplerate = sample_rate;
  audio_cfg_.clear();
  switch (object) { 
  case MOV_OBJECT_AAC:
    aCodec = CodecAAC;
    if (extra && bytes)
      audio_cfg_.assign((const char*)extra, bytes);
    break;
  case MOV_OBJECT_MP3:
  case MOV_OBJECT_MP1A:
    aCodec = CodecMP3;
    break;
  case MOV_OBJECT_OPUS:
    aCodec = CodecOpus;
    break;
  case MOV_OBJECT_G711a:
    aCodec = CodecG711A;
    break;
  case MOV_OBJECT_G711u:
    aCodec = CodecG711U;
    break;
  default: 
    aCodec = CodecInvalid; 
    break;
  }
  hlogi("Mp4Reader onAudioTrack %d(%s) %d %d %d", track, getCodecName(aCodec), object, channel_count, sample_rate);
  track_map_[track] = aCodec;
  if (extra && bytes){
    auto frame = std::make_shared<Frame>();
    frame->codec = aCodec;
    frame->pts = frame->dts = 0;
    frame->appendData(extra, bytes);
    inputFrame(frame);
  }
  return 0;
}

bool extra_to_frame(int codec, const void* extra, size_t bytes, Frame::Ptr cfg){
  if (extra && bytes) {
    cfg->codec = (CodecId)codec;
    switch (codec) {
    case CodecH264: {
      mpeg4_avc_t avc;
      if (mpeg4_avc_decoder_configuration_record_load((const uint8_t*)extra, bytes, &avc)) {
        for (int i = 0; i < avc.nb_sps; i++) {
          cfg->appendNal(avc.sps[i].data, avc.sps[i].bytes);
        }
        for (int i = 0; i < avc.nb_pps; i++) {
          cfg->appendNal(avc.pps[i].data, avc.pps[i].bytes);
        }
        return true;
      }
      break;
    }
    case CodecH265: {
      mpeg4_hevc_t hevc;
      if (mpeg4_hevc_decoder_configuration_record_load((const uint8_t*)extra, bytes, &hevc)){
        for (size_t i = 0; i < hevc.numOfArrays; i++) {
          cfg->appendNal(hevc.nalu[i].data, hevc.nalu[i].bytes);
        }
        return true;
      }
      break;
    }
    case CodecAAC:
      cfg->appendData((const char*)extra, bytes);
      return true;
    default:
      break;
    }
  }
  return false;
}

int Mp4Reader::onVideoTrack(uint32_t track,
                            uint8_t object,
                            int width,
                            int height,
                            const void* extra,
                            size_t bytes) {
  video_width = width;
  video_height = height;
  video_cfg_ = std::make_shared<Frame>();
  switch (object) {
  case MOV_OBJECT_H264:
    vCodec = CodecH264;
    if (extra && bytes) {
      extra_to_frame(vCodec, extra, bytes, video_cfg_);
    }
    break;
  case MOV_OBJECT_H265:
    vCodec = CodecH265;
    if (extra && bytes) {
      extra_to_frame(vCodec, extra, bytes, video_cfg_);
    }
    break;
  case MOV_OBJECT_VP8:
    vCodec = CodecVP8;
    break;
  case MOV_OBJECT_VP9:
    vCodec = CodecVP9;
    break;
  case MOV_OBJECT_AV1:
    vCodec = CodecAV1;
    break;
  default: 
    vCodec = CodecInvalid;
    break;
  }
  hlogi("Mp4Reader onVideoTrack %d(%s) %d %dx%d", track, getCodecName(vCodec), object, width, height);
  track_map_[track] = vCodec;
  if (video_cfg_ && video_cfg_->size()) {
    inputFrame(video_cfg_);
  }
  return 0;
}

int64_t Mp4Reader::Seek(int64_t tsp, bool quick) {
  mov_reader_seek(mp4_handle_, &tsp);
  frame_.pts = tsp;
  eof_ = false;
  return tsp;
}

bool Mp4Reader::hasVideo() const {
  return vCodec != CodecInvalid;
}

bool Mp4Reader::hasAudio() const {
  return aCodec != CodecInvalid;
}
