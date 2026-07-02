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
#include "mp3-header.h"
#include "webm-vpx.h"
#include "aom-av1.h"
#include "SPSParser.h"

#define SMAPLE_UNIT 90
#define MP4_INVALID_TRACK_ID -1

Mp4Writer::Mp4Writer() {
  audio_track_ = video_track_ = MP4_INVALID_TRACK_ID;
  writer_ = NULL;
  video_width = video_height = 0;
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

bool Mp4Writer::inputFrame(const Frame::Ptr &frame) {
  bool ret = false;
  switch (frame->getTrackType())
  {
  case TrackVideo:
    if (video_codec == CodecInvalid) {
      video_codec = frame->getCodecId();
    }
    ret = WriteVideo((uint8_t*)frame->data(), frame->size(), frame->pts, frame->is_key, frame->pts - frame->dts);
    break;
  case TrackAudio:
    if (audio_codec == CodecInvalid) {
      audio_codec = frame->getCodecId();
    }
    ret = WriteAudio((uint8_t*)frame->data(), frame->size(), frame->pts);
    break;
  default:
    break;
  }
  return ret;
}

int Mp4Writer::SetVideo(int codec, int width, int height, int fps) {
  video_width = width;
  video_height = height;
  video_fps = fps;
  video_codec = (CodecId)codec;
  hlogi("SetVideo %d %dx%d@%d", codec, width, height, fps);
  return 0;
}

int Mp4Writer::SetAudio(int codec, int sampleReate, int nChannel, int profile) {
  audio_samplerate = sampleReate;
  audio_channels = nChannel;
  audio_codec = (CodecId)codec;
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
  fp_ = fopen(path, "wb");
  if (!fp_) {
    hlogi("unable to open %s", path);
    return false;
  }
  path_ = path;
  writer_ = mp4_writer_create(fmt, mov_get_file_buffer(), fp_, 0);
  ret = opened();
  hlogi("Open %d %s return %d", fmt, path, ret);
  return ret;
}

bool Mp4Writer::Close() {
  std::unique_lock<decltype(lock)> l(lock);
  if (writer_) {
    hlogi("Close %s", path_.c_str());
    audio_track_ = video_track_ = MP4_INVALID_TRACK_ID;
    first_tsp = 0;
    video_codec = CodecInvalid;
    audio_codec = CodecInvalid;
    mp4_writer_destroy(writer_);
    writer_ = NULL;
    _sps = _pps = _vps = "";
    path_.clear();
  }
  if (fp_) {
    fclose(fp_);
    fp_ = nullptr;
  }
  return true;
}

static bool getAVCInfo(const char *sps, size_t sps_len, int &iVideoWidth, int &iVideoHeight, float &iVideoFps) {
    if (sps_len < 4) {
        return false;
    }
    T_GetBitContext tGetBitBuf;
    T_SPS tH264SpsInfo;
    memset(&tGetBitBuf, 0, sizeof(tGetBitBuf));
    memset(&tH264SpsInfo, 0, sizeof(tH264SpsInfo));
    tGetBitBuf.pu8Buf = (uint8_t *)sps + 1;
    tGetBitBuf.iBufSize = (int)(sps_len - 1);
    if (0 != h264DecSeqParameterSet((void *)&tGetBitBuf, &tH264SpsInfo)) {
        return false;
    }
    h264GetWidthHeight(&tH264SpsInfo, &iVideoWidth, &iVideoHeight);
    h264GeFramerate(&tH264SpsInfo, &iVideoFps);
    // ErrorL << iVideoWidth << " " << iVideoHeight << " " << iVideoFps;
    return true;
}        


bool getHEVCInfo(const char * vps, size_t vps_len,const char * sps,size_t sps_len,int &iVideoWidth, int &iVideoHeight, float  &iVideoFps){
    T_GetBitContext tGetBitBuf;
    T_HEVCSPS tH265SpsInfo;	
    T_HEVCVPS tH265VpsInfo;
    if ( vps_len > 2 ){
        memset(&tGetBitBuf,0,sizeof(tGetBitBuf));	
        memset(&tH265VpsInfo,0,sizeof(tH265VpsInfo));
        tGetBitBuf.pu8Buf = (uint8_t*)vps+2;
        tGetBitBuf.iBufSize = (int)(vps_len-2);
        if(0 != h265DecVideoParameterSet((void *) &tGetBitBuf, &tH265VpsInfo)){
            return false;
        }
    }

    if ( sps_len > 2 ){
        memset(&tGetBitBuf,0,sizeof(tGetBitBuf));
        memset(&tH265SpsInfo,0,sizeof(tH265SpsInfo));
        tGetBitBuf.pu8Buf = (uint8_t*)sps+2;
        tGetBitBuf.iBufSize = (int)(sps_len-2);
        if(0 != h265DecSeqParameterSet((void *) &tGetBitBuf, &tH265SpsInfo)){
            return false;
        }
    }
    else 
        return false;
    h265GetWidthHeight(&tH265SpsInfo, &iVideoWidth, &iVideoHeight);
    iVideoFps = 0;
    h265GeFramerate(&tH265VpsInfo, &tH265SpsInfo, &iVideoFps);
    return true;
}

int Mp4Writer::WriteVideo(uint8_t *data, int len, uint64_t tsp, bool bKeyFrame, int diff /*= 0*/) {
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
    case CodecH265:
      ParseNalFrame((char*)data, len, (char*)&p264[0], len, &_sps, &_pps, &_vps, bKeyFrame, false);
      if (MP4_INVALID_TRACK_ID==video_track_ && bKeyFrame) {
        getHEVCInfo((char*)&_vps[0], _vps.length(), (char*)&_sps[0], _sps.length(), video_width, video_height, video_fps);
        hlogi("%s addVideo %s %dx%d@%f", path_.c_str(), getCodecName(video_codec), video_width, video_height, video_fps);
        struct mpeg4_hevc_t hevc = {0};
        std::string vps_sps_pps = start_code + _sps + start_code + _pps + start_code + _vps;
        h265_annexbtomp4(&hevc, vps_sps_pps.data(), (int)vps_sps_pps.size(), NULL, 0, NULL, NULL);
        std::vector<uint8_t> extra_data(512);
        int data_size = mpeg4_hevc_decoder_configuration_record_save(&hevc, extra_data.data(), extra_data.size());
        video_track_ = mp4_writer_add_video(writer_, MOV_OBJECT_H265, 
          video_width, video_height, extra_data.data(), data_size);
      }
      break;
    case CodecH264:
      ParseNalFrame((char*)data, len, (char*)&p264[0], len, &_sps, &_pps, &_vps, bKeyFrame, true);
      if (MP4_INVALID_TRACK_ID == video_track_ && bKeyFrame) {
        getAVCInfo((char*)&_sps[0], _sps.length(), video_width, video_height, video_fps);
        hlogi("%s addVideo %s %dx%d@%f", path_.c_str(), getCodecName(video_codec), video_width, video_height, video_fps);
        struct mpeg4_avc_t avc = {0};
        std::string vps_sps_pps = start_code + _sps + start_code + _pps;
        h264_annexbtomp4(&avc, vps_sps_pps.data(), (int)vps_sps_pps.size(), NULL, 0, NULL, NULL);
        std::vector<uint8_t> extra_data(1024);
        int data_size = mpeg4_avc_decoder_configuration_record_save(&avc, extra_data.data(), extra_data.size());
        video_track_ = mp4_writer_add_video(writer_, MOV_OBJECT_H264, 
          video_width, video_height, extra_data.data(), data_size);
      }
      break;
    case CodecVP8:
      bKeyFrame = !(data[0] & 0x01);
      if (MP4_INVALID_TRACK_ID == video_track_ && bKeyFrame && len >= 10) {
        webm_vpx_t vpx  = {0};
        if (0 == webm_vpx_codec_configuration_record_from_vp8(&vpx, &video_width, &video_height, data, len)) {
          std::vector<uint8_t> extra_data(8 + vpx.codec_intialization_data_size);
          int data_size = webm_vpx_codec_configuration_record_save(&vpx, extra_data.data(), extra_data.size());
          video_track_ = mp4_writer_add_video(writer_, MOV_OBJECT_VP8,
            video_width, video_height, extra_data.data(), data_size);
          hlogi("%s addVideo %s %dx%d@%f", path_.c_str(), getCodecName(video_codec), video_width, video_height, video_fps);
        }
      }
      break;
    case CodecVP9:
      bKeyFrame = data[0] & 0x80;
      if (MP4_INVALID_TRACK_ID == video_track_ && bKeyFrame && len >= 10) {
        webm_vpx_t vpx = {0};
        if (0 == webm_vpx_codec_configuration_record_from_vp9(&vpx, &video_width, &video_height, data, len)) {
          std::vector<uint8_t> extra_data(8 + vpx.codec_intialization_data_size);
          int data_size = webm_vpx_codec_configuration_record_save(&vpx, extra_data.data(), extra_data.size());
          video_track_ = mp4_writer_add_video(writer_, MOV_OBJECT_VP9,
            video_width, video_height, extra_data.data(), data_size);
          hlogi("%s addVideo %s %dx%d@%f", path_.c_str(), getCodecName(video_codec), video_width, video_height, video_fps);
        }
      }
      break;
    case CodecAV1:
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
          hlogi("%s addVideo %s %dx%d@%f", path_.c_str(), getCodecName(video_codec), video_width, video_height, video_fps);
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

int Mp4Writer::WriteAudio(uint8_t* data, int len, uint64_t tsp) {
  std::unique_lock<decltype(lock)> l(lock);
  if (!writer_) return -1;
  if (audio_codec == CodecAAC) {
    if (parseAdtsHeader(data, len, &audio_samplerate, &audio_channels, &aac_profile)) {  // remove ADTS header
      data += ADTS_HEADER_SIZE;
      len -= ADTS_HEADER_SIZE;
    }
  }
  if (MP4_INVALID_TRACK_ID == audio_track_) {
    if (!audio_samplerate) {
      audio_samplerate = RtpPayload::getClockRateByCodec(audio_codec);
    }
    if (!audio_channels) {
      audio_channels = 1;
    }

    switch (audio_codec) {
      case CodecAAC: {
        // Main = 1, Low = 2, SSR = 3, Lip = 4
        // MP4SetAudioProfileLevel(mp4Writer, 0x02);
        unsigned char faacDecoderInfo[2];
        genAacDecInfo((char*)faacDecoderInfo, audio_samplerate, audio_channels, aac_profile);
        audio_track_ = mp4_writer_add_audio(writer_, MOV_OBJECT_AAC, 
          audio_channels, 16, audio_samplerate, faacDecoderInfo, 2);
        hlogi("%s addAudio %s %dx%d", path_.c_str(), getCodecName(audio_codec), audio_channels, audio_samplerate);
        break;
      }
      case CodecOpus: {
        struct opus_head_t opus = {0};
        opus.version = 1;
        opus.channels = audio_channels;
        opus.input_sample_rate = audio_samplerate;
        opus.channel_mapping_family = 0;
        uint8_t buff[29];
        int len = opus_head_save(&opus, buff, sizeof(buff));
        audio_track_ = mp4_writer_add_audio(writer_, MOV_OBJECT_AAC, 
          audio_channels, 16, audio_samplerate, buff, len);
        hlogi("%s addAudio %s %dx%d", path_.c_str(), getCodecName(audio_codec), audio_channels, audio_samplerate);
        break;
      }
      case CodecMP3:
      {
        struct mp3_header_t mp3 = {0};
        if (mp3_header_load(&mp3, data, len)) {
          audio_samplerate = mp3_get_frequency(&mp3);
          audio_channels = mp3_get_channel(&mp3);
          audio_track_ = mp4_writer_add_audio(writer_, MOV_OBJECT_MP3, 
            audio_channels, 16, audio_samplerate, nullptr, 0);
          hlogi("%s addAudio %s %dx%d", path_.c_str(), getCodecName(audio_codec), audio_channels, audio_samplerate);
        }
      }
      case CodecG711A:
      case CodecG711U:
        hlogi("%s addAudio %s %dx%d", path_.c_str(), getCodecName(audio_codec), audio_channels, audio_samplerate);
        audio_track_ = mp4_writer_add_audio(writer_, audio_codec == CodecG711A ? MOV_OBJECT_G711a : MOV_OBJECT_G711u, 
          audio_channels, 16, audio_samplerate, nullptr, 0);
      break;
      default:
        break;
    }
  }

  if (MP4_INVALID_TRACK_ID != audio_track_) {
    if (!first_tsp)
      first_tsp = tsp;
    tsp -= first_tsp;
    mp4_writer_write(writer_, audio_track_, data, len, tsp, tsp, 0);
  }
  return 0;
}
