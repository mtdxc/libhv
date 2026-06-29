#ifndef _AVWRITER_H_INCLUDED__
#define _AVWRITER_H_INCLUDED__
#pragma once

#ifdef FLVWRITE_STATIC
#define FLVWRITE_API 
#else
#ifdef _WIN32
#ifdef FLVWRITE_EXPORT
#define FLVWRITE_API __declspec(dllexport)
#else
#define FLVWRITE_API __declspec(dllimport)
#endif
#else
#define FLVWRITE_API __attribute__((visibility("default")))
#endif
#endif
#include <stdint.h>

#define		FLV_CODEC_NONE		-1
#define		FLV_CODEC_SOME		0
#define		FLV_CODEC_FLV1		2			// Sorenson H.263
#define		FLV_CODEC_FLV4		4			// On2 VP6
#define		FLV_CODEC_H264		7			// H.264
#define		FLV_CODEC_H265		12			// H.265
#define		FLV_CODEC_AV1		13			// av1
#define		FLV_CODEC_VP8		14			// VP8
#define		FLV_CODEC_VP9		15			// VP9

#define		FLV_CODEC_MP3		2			// MP3
#define		FLV_CODEC_AAC		10			// AAC
#define		FLV_CODEC_OPUS		13			// OPUS
// 当前只支持视频H264和音频Aac写入
class FLVWRITE_API IAvWriter {
public:
  virtual int SetVideo(int codec, int width, int height, int fps) { return 0; }
  virtual int SetAudio(int codec, int sampleRate, int nChannel, int profile = 2) { return 0; }

  virtual bool Open(const char* path) = 0;
  virtual bool Close() = 0;

  /*!
  @brief 写视频包.

  @param data 264数据
  @param len 数据长度
  @param timestamp_ms 时间戳
  @param bKeyFrame 是否关键帧
  @param diff 时间差值
  @return int
  */
  virtual int WriteVideo(uint8_t* data, int len, uint32_t timestamp_ms, bool keyframe, int diff = 0) = 0;
  /*!
  @brief 写入音频数据.

  @param data AAC数据
  @param len 长度
  @param timestamp_ms 时间戳
  @return int
  @retval
  */
  virtual int WriteAudio(uint8_t* data, int len, uint32_t timestamp_ms) = 0;
  virtual ~IAvWriter() {}
};

FLVWRITE_API FILE* Utf8FileOpen(const char* path, const char* mode);
FLVWRITE_API bool Utf8FileExist(const char* path);

#endif