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
#define GID_MP4 "Mp4Reader"
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

static char start_code[4] = {0, 0, 0, 1};

Mp4Reader::Mp4Reader(IReader *reader) : event_(reader) {
  mp4_handle_ = nullptr;
  video_tracker_ = MP4_INVALID_TRACK_ID;
  audio_tracker_ = MP4_INVALID_TRACK_ID;
  video_codec = FLV_CODEC_NONE;
  audio_codec = FLV_CODEC_NONE;
}

Mp4Reader::~Mp4Reader() { Close(); }

bool Mp4Reader::Close() {
  bool ret = false;
  if (mp4_handle_) {
    mov_reader_destroy(mp4_handle_);
    mp4_handle_ = nullptr;
    audio_tracker_ = MP4_INVALID_TRACK_ID;
    video_tracker_ = MP4_INVALID_TRACK_ID;
    ret = true;
  }
  if (fp_) {
    fclose(fp_);
    fp_ = nullptr;
  }
  return ret;
}

bool Mp4Reader::Open(const char *path) {
  fp_ = Utf8FileOpen(path, "rb");
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
  
  if (audio_tracker_ == MP4_INVALID_TRACK_ID &&
      video_tracker_ == MP4_INVALID_TRACK_ID) {
    Close();
    return false;
  }

  duration_ = mov_reader_getduration(handle);
  return true;
}

int Mp4Reader::ReadFrame() {
  int ret = mov_reader_read2(
      mp4_handle_,
      [](void* param, uint32_t track, size_t bytes, int64_t pts, int64_t dts,
         int flags) {
        Mp4Reader* thiz = (Mp4Reader*)param;
        thiz->track_ = track;
        thiz->pts_ = pts;
        thiz->dts_ = dts;
        thiz->key_ = flags & MOV_AV_FLAG_KEYFREAME;
        thiz->bytes_ = bytes;
        if (thiz->buffer_.size() < bytes)
          thiz->buffer_.reserve(bytes);
        return (void*)thiz->buffer_.data();
      },
      this);

  switch (ret) {
    case 0: {
      eof_ = true;
      return 0;
    }

    case 1: {
      uint32_t iOffset = 0;
      uint8_t* pBytes = (uint8_t*)buffer_.data();
      while (iOffset < bytes_) {
        int iFrameLen = BytesToUI32(pBytes + iOffset);
        if (iFrameLen + iOffset + 4 > bytes_) {
          break;
        }
        memcpy(pBytes + iOffset, start_code, 4);
        iOffset += (iFrameLen + 4);
      }
      if (track_ == audio_tracker_) {
        event_->OnGotAudio(buffer_.data(), bytes_, dts_);
      } else if (track_ == video_tracker_) {
        event_->OnGotVideo(buffer_.data(), bytes_, dts_, pts_, key_);
      }
      return bytes_;
    }

    default: {
      eof_ = true;
      hlogi("Mp4读取失败%d", ret);
      return 0;
    }
  }
}

int Mp4Reader::onAudioTrack(uint32_t track,
                            uint8_t object,
                            int channel_count,
                            int bit_per_sample,
                            int sample_rate,
                            const void* extra,
                            size_t bytes) {
  audio_tracker_ = track;
  audio_channels = channel_count;
  audio_samplerate = sample_rate;
  audio_cfg_.clear();
  switch (object) { 
  case MOV_OBJECT_AAC:
    audio_codec = FLV_CODEC_AAC;
    if (extra && bytes)
      audio_cfg_.assign((const char*)extra, bytes);
    break;
  case MOV_OBJECT_MP3:
  case MOV_OBJECT_MP1A:
    audio_codec = FLV_CODEC_MP3;
    break;
  case MOV_OBJECT_OPUS:
    audio_codec = FLV_CODEC_OPUS;
    break;
  }
  if (event_ && extra && bytes)
    event_->OnGotAudio((const char*)extra, bytes, 0);
  return 0;
}

bool extra_to_frame(int codec, const void* extra, size_t bytes, std::string& cfg){
  if (extra && bytes) {
    switch (codec) {
    case FLV_CODEC_H264: {
      mpeg4_avc_t avc;
      if (mpeg4_avc_decoder_configuration_record_load((const uint8_t*)extra, bytes, &avc)) {
        for (int i = 0; i < avc.nb_sps; i++) {
          cfg.append(start_code, 4);
          cfg.append((char*)avc.sps[i].data, avc.sps[i].bytes);
        }
        for (int i = 0; i < avc.nb_pps; i++) {
          cfg.append(start_code, 4);
          cfg.append((char*)avc.pps[i].data, avc.pps[i].bytes);
        }
        return true;
      }
      break;
    }
    case FLV_CODEC_H265: {
      mpeg4_hevc_t hevc;
      if (mpeg4_hevc_decoder_configuration_record_load((const uint8_t*)extra, bytes, &hevc)){
        for (size_t i = 0; i < hevc.numOfArrays; i++) {
          cfg.append(start_code, 4);
          cfg.append((char*)hevc.nalu[i].data, hevc.nalu[i].bytes);
        }
        return true;
      }
      break;
    }
    case FLV_CODEC_AAC:
      cfg.assign((const char*)extra, bytes);
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
  video_tracker_ = track;
  video_width = width;
  video_height = height;
  video_cfg_.clear();

  switch (object) {
  case MOV_OBJECT_H264:
    video_codec = FLV_CODEC_H264;
    if (extra && bytes) {
      extra_to_frame(video_codec, extra, bytes, video_cfg_);
    }
    break;
  case MOV_OBJECT_H265:
    video_codec = FLV_CODEC_H265;
    if (extra && bytes) {
      extra_to_frame(video_codec, extra, bytes, video_cfg_);
    }
    break;
  case MOV_OBJECT_VP8:
    video_codec = FLV_CODEC_VP8;
    break;
  case MOV_OBJECT_VP9:
    video_codec = FLV_CODEC_VP9;
    break;
  case MOV_OBJECT_AV1:
    video_codec = FLV_CODEC_AV1;
    break;
  }
  if (video_cfg_.length() && event_) {
    event_->OnGotVideo(video_cfg_.data(), video_cfg_.length(), 0, 0, true);
  }
  return 0;
}

uint32_t Mp4Reader::Seek(uint32_t msTime, bool quick) {
  int64_t tsp = msTime;
  mov_reader_seek(mp4_handle_, &tsp);
  pts_ = tsp;
  return tsp;
}

bool Mp4Reader::hasVideo() const {
  return video_tracker_ != MP4_INVALID_TRACK_ID;
}

bool Mp4Reader::hasAudio() const {
  return audio_tracker_ != MP4_INVALID_TRACK_ID;
}

const char *Mp4Reader::getAacCfg(int &len) {
  len = audio_cfg_.length();
  return audio_cfg_.data();
}

int Mp4Reader::MakeSeqNal(uint8_t *nal, int nalLen) {
  int ret = video_cfg_.length();
  if (!nal)
    return ret;
  if (nalLen < ret) {
    return 0;
  }
  memcpy(nal, video_cfg_.data(), video_cfg_.length());
  return ret;
}
