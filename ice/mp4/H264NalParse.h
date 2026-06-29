#ifndef _H264NAL_PARSER_H_INCLUDED__
#define _H264NAL_PARSER_H_INCLUDED__
#pragma once

#include <string>
#include <stdint.h>

uint8_t* UI08ToBytes(uint8_t* buf, unsigned char val);
uint8_t* UI16ToBytes(uint8_t* buf, unsigned short val);
uint8_t* UI24ToBytes(uint8_t* buf, unsigned int val);
uint8_t* UI32ToBytes(uint8_t* buf, unsigned int val);
uint8_t* UI64ToBytes(uint8_t* buf, uint64_t val);
uint8_t* DoubleToBytes(uint8_t* buf, double val);
uint32_t BytesToUI32(const uint8_t* buf);
uint32_t BytesToUI24(const uint8_t* buf);
uint16_t BytesToUI16(const uint8_t* buf);
enum H264NalType {
    AVC_NAL_IDR = 5,
    AVC_NAL_SEI = 6,
    AVC_NAL_SPS = 7,
    AVC_NAL_PPS = 8,
    AVC_NAL_AUD = 9,
    AVC_NAL_B_P = 1,
};

enum H265NalType{
    HEVC_NAL_TRAIL_N = 0,
    HEVC_NAL_TRAIL_R = 1,
    HEVC_NAL_TSA_N = 2,
    HEVC_NAL_TSA_R = 3,
    HEVC_NAL_STSA_N = 4,
    HEVC_NAL_STSA_R = 5,
    HEVC_NAL_RADL_N = 6,
    HEVC_NAL_RADL_R = 7,
    HEVC_NAL_RASL_N = 8,
    HEVC_NAL_RASL_R = 9,
    HEVC_NAL_BLA_W_LP = 16,
    HEVC_NAL_BLA_W_RADL = 17,
    HEVC_NAL_BLA_N_LP = 18,
    HEVC_NAL_IDR_W_RADL = 19,
    HEVC_NAL_IDR_N_LP = 20,
    HEVC_NAL_CRA_NUT = 21,
    HEVC_NAL_RSV_IRAP_VCL22 = 22,
    HEVC_NAL_RSV_IRAP_VCL23 = 23,

    HEVC_NAL_VPS = 32,
    HEVC_NAL_SPS = 33,
    HEVC_NAL_PPS = 34,
    HEVC_NAL_AUD = 35,
    HEVC_NAL_EOS_NUT = 36,
    HEVC_NAL_EOB_NUT = 37,
    HEVC_NAL_FD_NUT = 38,
    HEVC_NAL_SEI_PREFIX = 39,
    HEVC_NAL_SEI_SUFFIX = 40,
};
#define H265_TYPE(v) (((uint8_t)(v) >> 1) & 0x3f)
#define H264_TYPE(v) ((uint8_t)(v) & 0x1F)

#define MAX_NAL_COUNT 32
bool ParseNalFrame(const char* nalsbuf, int size, 
					char* outBuf, int& outLen,
					std::string* spsBuf, std::string* ppsBuf, std::string* vps,
					bool& isKeyframe, bool h264);
bool ParseAvcFrame(char* buf, int size, bool toNal, 
					std::string* spsBuf, std::string* ppsBuf, std::string* vps,
					bool& isKeyframe, bool h264);
bool AVCParseNalUnits(const char *bufIn, int inSize, uint8_t* bufOut, int* outSize);
const char* AVCFindStartCode(const char *p, const char *end);

#define ADTS_HEADER_SIZE 7
int AacSampleIndex(int nSoundRate);
int AacSampleRate(int nIndex);
int genAacDecInfo(char *buf, int samplerate, int channel, int profile);
int getAacDecInfo(uint8_t *buff, int* samplerate, int* channel, int* profile);
int genAdtsHeader(uint8_t *buf, int size, int samlerate, int channel, int profile);
bool parseAdtsHeader(uint8_t* data, int size, int* samlerate, int* channel, int* profile);
bool HasAdtsHeader(uint8_t* buff, int size);
struct AdtsHeader {
  uint16_t sync : 12;              // 固定为0xFFF
  uint16_t id : 1;                 // 0表示MPEG-4，1表示MPEG-2
  uint16_t layer : 2;              // 固定为'00'
  uint16_t protection_absent : 1;  // has crc len = 9

  uint16_t profile : 2;  // 1: AAC Main 2:AAC LC (Low Complexity) 3:AAC SSR (Scalable
                    // Sample Rate) 4:AAC LTP (Long Term Prediction)
  uint16_t samplerate_idx : 4; // AacSampleRate
  uint16_t private_bit : 1;
  uint16_t channel_configuration : 3;
  uint16_t original_copy : 1;
  uint16_t home : 1;

  uint16_t copyrighted_id_bit : 1;
  uint16_t copyrighted_id_start : 1;
  uint16_t frame_length : 13;  // frame_length = (protection_absent == 0 ? 9 : 7) + audio_data_length
  uint16_t adts_buffer_fullness : 11;  // 固定为0x7FF。表示是码率可变的码流
  uint16_t
      number_of_raw_data_blocks_in_frame : 2;  // 表示当前帧有number_of_raw_data_blocks_in_frame + 1
                                               // 个原始帧(一个AAC原始帧包含一段时间内1024个采样及相关数据)
};

bool h264_decode_seq_parameter_set(uint8_t* buf, uint32_t nLen, int &Width, int &Height);

typedef struct PutBitContext
{
    unsigned int bit_buf; ///< 32bits buffer
    int bit_left;
    char *buf, *buf_ptr, *buf_end;
    int size_in_bits;
} PutBitContext;

void init_put_bits(PutBitContext *s, char *buffer, int buffer_size);
void flush_put_bits(PutBitContext *s);
void put_bits(PutBitContext *s, int n, unsigned int value);
#endif