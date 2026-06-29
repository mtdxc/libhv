#include "hlog.h"
#include "Mp4Writer.h"
#include "H264NalParse.h"
#include <vector>
// for mp4 container
#include "mov-format.h"
#include "mp4-writer.h"
// for mov_get_file_buffer
#include "Mp4Reader.h"
// for extra data
#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"
#include "opus-head.h"
#include "webm-vpx.h"
#include "aom-av1.h"

#define GID_MP4 "Mp4Writer"
#define SMAPLE_UNIT 90
#define MP4_INVALID_TRACK_ID -1

Mp4Writer::Mp4Writer() {
  audio_track_ = video_track_ = MP4_INVALID_TRACK_ID;
  writer_ = NULL;
  video_width = video_height = 0;
  video_codec = FLV_CODEC_NONE;
  audio_codec = FLV_CODEC_NONE;
  audio_samplerate = 44100;
  audio_channels = 1;
  aac_profile = 2;
  p264 = NULL;
  n264 = 0;
}

Mp4Writer::~Mp4Writer() {
  Close();
  if (p264) delete[] p264;
}

int Mp4Writer::SetVideo(int codec, int width, int height, int fps) {
  video_width = width;
  video_height = height;
  video_fps = fps;
  video_codec = codec;
  hlogi("SetVideo %d %dx%d@%d", codec, width, height, fps);
  return 0;
}

int Mp4Writer::SetAudio(int codec, int sampleReate, int nChannel, int profile) {
  audio_samplerate = sampleReate;
  audio_channels = nChannel;
  audio_codec = codec;
  aac_profile = profile;
  hlogi("SetAudio %d %dx%d profile %d", codec, nChannel, sampleReate, profile);
  return 0;
}

bool Mp4Writer::Open2(const char* path, int fmt) {
  bool ret = false;
  if (opened()) {
    hlogi("call close first, skip open %s", path);
    return false;
  }
  std::unique_lock<decltype(lock)> l(lock);
  fp_ = Utf8FileOpen(path, "wb");
  if (!fp_) {
    hlogi("unable to open %s", path);
    return false;
  }
  writer_ = mp4_writer_create(fmt, mov_get_file_buffer(), fp_, 0);
  ret = opened();
  hlogi("Open %d %s return %d", fmt, path, ret);
  return ret;
}

bool Mp4Writer::Close() {
  std::unique_lock<decltype(lock)> l(lock);
  if (writer_) {
    hlogi("Close");
    audio_track_ = video_track_ = MP4_INVALID_TRACK_ID;
    first_tsp = 0;
    video_codec = FLV_CODEC_NONE;
    audio_codec = FLV_CODEC_NONE;
    mp4_writer_destroy(writer_);
    writer_ = NULL;
    _sps = _pps = _vps = "";
    // MP4Optimize(path.c_str());
  }
  if (fp_) {
    fclose(fp_);
    fp_ = nullptr;
  }
  return true;
}

int Mp4Writer::WriteVideo(uint8_t* data, int len, uint32_t tsp, bool bKeyFrame,
                          int diff /*= 0*/) {
  std::unique_lock<decltype(lock)> l(lock);
  if (!writer_) return -1;

  if (n264 < len + MAX_NAL_COUNT) {
    n264 = len + MAX_NAL_COUNT;
    if (p264) delete[] p264;
    p264 = new uint8_t[n264];
  }

  static const std::string start_code = {0, 0, 0, 1};

  bKeyFrame = 0;
  switch (video_codec) {
    case FLV_CODEC_H265:
      ParseNalFrame((char*)data, len, (char*)&p264[0], len, &_sps, &_pps, &_vps, bKeyFrame, false);
      if (MP4_INVALID_TRACK_ID==video_track_ && bKeyFrame) {
        struct mpeg4_hevc_t hevc = {0};
        std::string vps_sps_pps = start_code + _sps + start_code + _pps + start_code + _vps;
        h265_annexbtomp4(&hevc, vps_sps_pps.data(), (int)vps_sps_pps.size(), NULL, 0, NULL, NULL);
        std::vector<uint8_t> extra_data(512);
        int data_size = mpeg4_hevc_decoder_configuration_record_save(&hevc, extra_data.data(), extra_data.size());
        video_track_ = mp4_writer_add_video(writer_, MOV_OBJECT_H265, 
          video_width, video_height, extra_data.data(), data_size);
      }
      break;
    case FLV_CODEC_H264:
      ParseNalFrame((char*)data, len, (char*)&p264[0], len, &_sps, &_pps, &_vps, bKeyFrame, true);
      if (MP4_INVALID_TRACK_ID == video_track_ && bKeyFrame) {
        struct mpeg4_avc_t avc = {0};
        std::string vps_sps_pps = start_code + _sps + start_code + _pps;
        h264_annexbtomp4(&avc, vps_sps_pps.data(), (int)vps_sps_pps.size(), NULL, 0, NULL, NULL);
        if (video_width <= 0 || video_height <= 0)
          h264_decode_seq_parameter_set((uint8_t*)&_sps[0], _sps.length(), video_width, video_height);
        std::vector<uint8_t> extra_data(1024);
        int data_size = mpeg4_avc_decoder_configuration_record_save(&avc, extra_data.data(), extra_data.size());
        video_track_ = mp4_writer_add_video(writer_, MOV_OBJECT_H264, 
          video_width, video_height, extra_data.data(), data_size);
      }
      break;
    case FLV_CODEC_VP8:
      bKeyFrame = !(data[0] & 0x01);
      if (MP4_INVALID_TRACK_ID == video_track_ && bKeyFrame && len >= 10) {
        webm_vpx_t vpx  = {0};
        if (0 == webm_vpx_codec_configuration_record_from_vp8(&vpx, &video_width, &video_height, data, len)) {
          std::vector<uint8_t> extra_data(8 + vpx.codec_intialization_data_size);
          int data_size = webm_vpx_codec_configuration_record_save(&vpx, extra_data.data(), extra_data.size());
          video_track_ = mp4_writer_add_video(writer_, MOV_OBJECT_VP8,
            video_width, video_height, extra_data.data(), data_size);
        }
      }
      break;
    case FLV_CODEC_VP9:
      bKeyFrame = data[0] & 0x80;
      if (MP4_INVALID_TRACK_ID == video_track_ && bKeyFrame && len >= 10) {
        webm_vpx_t vpx = {0};
        if (0 == webm_vpx_codec_configuration_record_from_vp9(&vpx, &video_width, &video_height, data, len)) {
          std::vector<uint8_t> extra_data(8 + vpx.codec_intialization_data_size);
          int data_size = webm_vpx_codec_configuration_record_save(&vpx, extra_data.data(), extra_data.size());
          video_track_ = mp4_writer_add_video(writer_, MOV_OBJECT_VP9,
            video_width, video_height, extra_data.data(), data_size);
        }
      }
      break;
    case FLV_CODEC_AV1:
      bKeyFrame = (data[0] & 0x78) >> 3 == 1;
      if (MP4_INVALID_TRACK_ID == video_track_ && bKeyFrame && len >= 10) {
        aom_av1_t av1 = {0};
        if (0 == aom_av1_codec_configuration_record_init(&av1, data, len)) {
          video_width = av1.width;
          video_height = av1.height;
          std::vector<uint8_t> extra_data(4 + av1.bytes);
          int data_size = aom_av1_codec_configuration_record_save(&av1, extra_data.data(), extra_data.size());
          video_track_ = mp4_writer_add_video(writer_, MOV_OBJECT_AV1, 
              video_width, video_height, extra_data.data(), data_size);
        }
      }
    default:
      break;
  }

  if (MP4_INVALID_TRACK_ID != video_track_) {
    if (!first_tsp)
      first_tsp = tsp;
    tsp -= first_tsp;

    int flags = bKeyFrame ? MOV_AV_FLAG_KEYFREAME : 0;
    mp4_writer_write(writer_, video_track_, p264, len, tsp + diff, tsp, flags);
  }

  return 0;
}

int Mp4Writer::WriteAudio(uint8_t* data, int len, uint32_t tsp) {
  std::unique_lock<decltype(lock)> l(lock);
  if (!writer_) return -1;
  if (audio_codec == FLV_CODEC_AAC) {
    if (parseAdtsHeader(data, len, &audio_samplerate, &audio_channels,
                        &aac_profile)) {  // remove ADTS header
      data += ADTS_HEADER_SIZE;
      len -= ADTS_HEADER_SIZE;
    }
  }
  if (!audio_track_) {
    switch (audio_codec) {
      case FLV_CODEC_AAC: {
        // Main = 1, Low = 2, SSR = 3, Lip = 4
        // MP4SetAudioProfileLevel(mp4Writer, 0x02);
        unsigned char faacDecoderInfo[2];
        genAacDecInfo((char*)faacDecoderInfo, audio_samplerate, audio_channels, aac_profile);
        audio_track_ = mp4_writer_add_audio(writer_, MOV_OBJECT_AAC, 
          audio_channels, 16, audio_samplerate, faacDecoderInfo, 2);
        break;
      }
      case FLV_CODEC_OPUS: {
        if (!audio_channels)
          audio_channels = 1;
        if (!audio_samplerate)
          audio_samplerate = 48000;
        struct opus_head_t opus = {0};
        opus.version = 1;
        opus.channels = audio_channels;
        opus.input_sample_rate = audio_samplerate;
        opus.channel_mapping_family = 0;
        uint8_t buff[29];
        int len = opus_head_save(&opus, buff, sizeof(buff));
        audio_track_ = mp4_writer_add_audio(writer_, MOV_OBJECT_AAC, 
          audio_channels, 16, audio_samplerate, buff, len);
        break;
      }
      case FLV_CODEC_MP3:
        audio_track_ = mp4_writer_add_audio(writer_, MOV_OBJECT_MP3, 
          audio_channels, 16, audio_samplerate, nullptr, 0);
      default:
        break;
    }
  }

  if (audio_track_) {
    if (!first_tsp)
      first_tsp = tsp;
    tsp -= first_tsp;
    mp4_writer_write(writer_, audio_track_, data, len, tsp, tsp, 0);
  }
  return 0;
}
