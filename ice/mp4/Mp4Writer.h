#ifndef MP4_WRITER_H_
#define MP4_WRITER_H_

#pragma once
#include <string>
#include <mutex>
#include "../Frame.h"
struct mp4_writer_t;

class Mp4Writer : public FrameWriterInterface {
 public:
  Mp4Writer();
  ~Mp4Writer();

  int SetVideo(int codec, int width, int height, int fps);
  int SetAudio(int codec, int sampleReate, int nChannel, int profile = 2);
  bool Open(const char* path) { return Open2(path, 0); }
  bool Open2(const char* path, int fmt);
  bool opened() const { return writer_ != nullptr; }
  bool Close();

  bool inputFrame(const Frame::Ptr &frame) override;
  /*!
  @brief 写视频包.

  @param data 264数据
  @param len 数据长度
  @param timestamp_ms 时间戳
  @param bKeyFrame 是否关键帧
  @param diff 时间差值
  @return int
  */
  int WriteVideo(uint8_t* data, int len, uint64_t timestamp_ms, bool bKeyFrame, int diff = 0);
  /*!
  @brief 写入音频数据.

  @param data AAC数据
  @param len 长度
  @param timestamp_ms 时间戳
  @return int
  @retval
  */
  int WriteAudio(uint8_t* data, int len, uint64_t timestamp_ms);

 protected:
  std::string path_;
  std::mutex lock;
  int audio_samplerate = 0;
  int audio_channels = 0;
  int aac_profile;
  CodecId audio_codec = CodecInvalid;
  CodecId video_codec = CodecInvalid;
  int video_width = 0;
  int video_height = 0;
  float video_fps = 0;
  std::string _sps, _pps, _vps;
  FILE* fp_ = nullptr;
  mp4_writer_t* writer_;
  int audio_track_;
  int video_track_;
  uint8_t* p264;
  int n264;
  uint32_t first_tsp = 0;
};

#endif  // MP4_WRITER_H_