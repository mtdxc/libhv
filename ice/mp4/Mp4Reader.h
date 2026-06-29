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

#include <map>
#include <string>
#include <vector>
#include "../Frame.h"
#include "EventLoop.h"

struct mov_reader_t;
struct mov_buffer_t;
mov_buffer_t* mov_get_file_buffer();

typedef uint32_t MP4TrackId;
bool extra_to_frame(int codec, const void* extra, size_t bytes, Frame::Ptr cfg);

class Mp4Reader : public FrameDispatcher {
 public:
  Mp4Reader(hv::EventLoop* loop);
  virtual ~Mp4Reader();

  bool Open(const char* path);
  bool Open(mov_buffer_t* provide, void* data);
  int64_t Seek(int64_t tsp, bool quick = false);

  bool Close();
  // 读取帧数，并返回与前一帧的延迟ms
  int ReadFrame();
  void StartRead();
  bool StopRead();

  int64_t duration() const { return duration_; }
  int64_t timestamp() const { return frame_.pts; }
  bool hasVideo() const;
  bool hasAudio() const;
  bool eof() const { return eof_; }
  void onSizeChange(size_t size) override;
 public:
  hv::EventLoop* loop_ = nullptr;
  hv::TimerID timer_ = 0;
  uint32_t video_width = 0;
  uint32_t video_height = 0;
  uint32_t audio_samplerate = 0;
  int audio_channels = 0;
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

  FILE* fp_ = nullptr;
  mov_reader_t* mp4_handle_;

  // for h264 seq header
  Frame::Ptr video_cfg_;

  std::string audio_cfg_;
  std::map<MP4TrackId, CodecId> track_map_;
  int64_t duration_ = 0;

  // current read frame
  Frame frame_;
  bool eof_ = false;
};

#endif /* SRC_MEDIAFILE_MEDIAREADER_H_ */
