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

#ifndef SRC_MEDIAFILE_MEDIAREADER_H_
#define SRC_MEDIAFILE_MEDIAREADER_H_

#include <string>
#include <vector>
#include "AvWriter.h"
struct mov_reader_t;
struct mov_buffer_t;
mov_buffer_t* mov_get_file_buffer();

typedef uint32_t MP4TrackId;
bool extra_to_frame(int codec, const void* extra, size_t bytes, std::string& cfg);
class FLVWRITE_API IReader {
 public:
  virtual ~IReader() {}
  virtual void OnGotAudio(const char* buff, int len, uint32_t tsp) = 0;
  virtual void OnGotVideo(const char* buff, int len, uint32_t tsp, uint32_t pts, bool key) = 0;
};
class FLVWRITE_API Mp4Reader {
 public:
  Mp4Reader(IReader* reader);
  virtual ~Mp4Reader();

  bool Open(const char* path);
  bool Open(mov_buffer_t* provide, void* data);
  uint32_t Seek(uint32_t msTime, bool quick = false);
  bool Close();

  int ReadFrame();

  int duration() const { return duration_; }
  int timestamp() const { return pts_; }
  bool hasVideo() const;
  bool hasAudio() const;
  const char* getAacCfg(int& len);
  int MakeSeqNal(uint8_t* nal, int nalLen);

 public:
  uint32_t video_width = 0;
  uint32_t video_height = 0;
  int video_codec;

  uint32_t audio_samplerate = 0;
  int audio_channels = 0;
  int audio_codec;

 private:
  int onAudioTrack(uint32_t track,
                   uint8_t object,
                   int channel_count,
                   int bit_per_sample,
                   int sample_rate,
                   const void* extra,
                   size_t bytes);
  int onVideoTrack(uint32_t track,
                   uint8_t object,
                   int width,
                   int height,
                   const void* extra,
                   size_t bytes);
  IReader* event_;

  FILE* fp_ = nullptr;
  mov_reader_t* mp4_handle_;
  MP4TrackId video_tracker_;

  // for h264 seq header
  std::string video_cfg_;

  MP4TrackId audio_tracker_;
  std::string audio_cfg_;

  int duration_ = 0;
  // current read frame
  MP4TrackId track_;
  std::vector<char> buffer_;
  int64_t pts_;
  int64_t dts_;
  int64_t bytes_ = 0;
  bool key_ = false;
  bool eof_ = false;
};

#endif /* SRC_MEDIAFILE_MEDIAREADER_H_ */
