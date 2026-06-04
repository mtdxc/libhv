#ifndef SRC_ICE_FRAME_H_
#define SRC_ICE_FRAME_H_

#include <string>
#include <map>
#include "hplatform.h"
struct StrCaseCompare {
    bool operator()(const std::string &__x, const std::string &__y) const { return strcasecmp(__x.data(), __y.data()) < 0; }
};

template <typename... ARGS> void Assert_ThrowCpp(int failed, const char *exp, const char *func, const char *file, int line, ARGS &&...args) {
    if (failed) {
        //std::stringstream ss;
        //toolkit::LoggerWrapper::appendLog(ss, std::forward<ARGS>(args)...);
        //Assert_Throw(failed, exp, func, file, line, ss.str().data());
    }
}
#ifndef CHECK
#define CHECK(exp, ...) Assert_ThrowCpp(!(exp), #exp, __FUNCTION__, __FILE__, __LINE__, ##__VA_ARGS__)
#endif // CHECK

typedef enum {
    TrackInvalid = -1,
    TrackVideo = 0,
    TrackAudio,
    TrackTitle,
    TrackApplication,
    TrackMax
} TrackType;

#define CODEC_MAP(XX) \
    XX(CodecH264,  TrackVideo, 0, "H264", PSI_STREAM_H264, MOV_OBJECT_H264, MKV_CODEC_VIDEO_H264)         \
    XX(CodecH265,  TrackVideo, 1, "H265", PSI_STREAM_H265, MOV_OBJECT_HEVC, MKV_CODEC_VIDEO_H265)         \
    XX(CodecAAC,   TrackAudio, 2, "mpeg4-generic", PSI_STREAM_AAC, MOV_OBJECT_AAC, MKV_CODEC_AUDIO_AAC)   \
    XX(CodecG711A, TrackAudio, 3, "PCMA", PSI_STREAM_AUDIO_G711A, MOV_OBJECT_G711a, MKV_CODEC_AUDIO_ACM)  \
    XX(CodecG711U, TrackAudio, 4, "PCMU", PSI_STREAM_AUDIO_G711U, MOV_OBJECT_G711u, MKV_CODEC_AUDIO_ACM)  \
    XX(CodecOpus,  TrackAudio, 5, "opus", PSI_STREAM_AUDIO_OPUS, MOV_OBJECT_OPUS, MKV_CODEC_AUDIO_OPUS)   \
    XX(CodecL16,   TrackAudio, 6, "L16", PSI_STREAM_RESERVED, MOV_OBJECT_NONE, MKV_CODEC_AUDIO_PCM_BE)    \
    XX(CodecVP8,   TrackVideo, 7, "VP8", PSI_STREAM_VP8, MOV_OBJECT_VP8, MKV_CODEC_VIDEO_VP8)             \
    XX(CodecVP9,   TrackVideo, 8, "VP9", PSI_STREAM_VP9, MOV_OBJECT_VP9, MKV_CODEC_VIDEO_VP9)             \
    XX(CodecAV1,   TrackVideo, 9, "AV1", PSI_STREAM_AV1, MOV_OBJECT_AV1, MKV_CODEC_VIDEO_AV1)             \
    XX(CodecJPEG,  TrackVideo, 10, "JPEG", PSI_STREAM_JPEG_2000, MOV_OBJECT_JPEG, MKV_CODEC_VIDEO_MJPEG)  \
    XX(CodecH266,  TrackVideo, 11, "H266", PSI_STREAM_H266, MOV_OBJECT_H266, MKV_CODEC_VIDEO_H266)        \
    XX(CodecTS,    TrackVideo, 12, "MP2T", PSI_STREAM_RESERVED, MOV_OBJECT_NONE, MKV_CODEC_UNKNOWN)       \
    XX(CodecPS,    TrackVideo, 13, "MPEG", PSI_STREAM_RESERVED, MOV_OBJECT_NONE, MKV_CODEC_UNKNOWN)       \
    XX(CodecMP3,   TrackAudio, 14, "MP3",  PSI_STREAM_MP3, MOV_OBJECT_MP3, MKV_CODEC_AUDIO_MP3)           \
    XX(CodecADPCM, TrackAudio, 15, "ADPCM", PSI_STREAM_RESERVED, MOV_OBJECT_NONE, MKV_CODEC_UNKNOWN)      \
    XX(CodecSVACV, TrackVideo, 16, "SVACV", PSI_STREAM_VIDEO_SVAC, MOV_OBJECT_NONE, MKV_CODEC_UNKNOWN)    \
    XX(CodecSVACA, TrackAudio, 17, "SVACA", PSI_STREAM_AUDIO_SVAC, MOV_OBJECT_NONE, MKV_CODEC_UNKNOWN)    \
    XX(CodecG722,  TrackAudio, 18, "G722", PSI_STREAM_AUDIO_G722, MOV_OBJECT_NONE, MKV_CODEC_UNKNOWN)     \
    XX(CodecG723,  TrackAudio, 19, "G723", PSI_STREAM_AUDIO_G723, MOV_OBJECT_NONE, MKV_CODEC_UNKNOWN)     \
    XX(CodecG728,  TrackAudio, 20, "G728", PSI_STREAM_RESERVED, MOV_OBJECT_NONE, MKV_CODEC_UNKNOWN)       \
    XX(CodecG729,  TrackAudio, 21, "G729", PSI_STREAM_AUDIO_G729, MOV_OBJECT_NONE, MKV_CODEC_UNKNOWN)

typedef enum {
    CodecInvalid = -1,
#define XX(name, type, value, str, mpeg_id, mp4_id, mkv_id) name = value,
    CODEC_MAP(XX)
#undef XX
    CodecMax
} CodecId;

/**
 * 字符串转媒体类型转
 * String to media type conversion
 
 * [AUTO-TRANSLATED:59850011]
 */
TrackType getTrackType(const std::string &str);

/**
 * 媒体类型转字符串
 * Media type to string conversion
 
 * [AUTO-TRANSLATED:0456e0e2]
 */
const char* getTrackString(TrackType type);

/**
 * 根据SDP中描述获取codec_id
 * @param str
 * @return
 * Get codec_id from SDP description
 * @param str
 * @return
 
 * [AUTO-TRANSLATED:024f2ed1]
 */
CodecId getCodecId(const std::string &str);

/**
 * 获取编码器名称
 * Get encoder name
 
 * [AUTO-TRANSLATED:0253534b]
 */
const char *getCodecName(CodecId codecId);

/**
 * 获取音视频类型
 * Get audio/video type
 
 * [AUTO-TRANSLATED:e2f06ac2]
 */
TrackType getTrackType(CodecId codecId);


#define RTP_PT_MAP(XX)                                     \
    XX(PCMU, TrackAudio, 0, 8000, 1, CodecG711U)           \
    XX(GSM, TrackAudio, 3, 8000, 1, CodecInvalid)          \
    XX(G723, TrackAudio, 4, 8000, 1, CodecG723)            \
    XX(DVI4_8000, TrackAudio, 5, 8000, 1, CodecInvalid)    \
    XX(DVI4_16000, TrackAudio, 6, 16000, 1, CodecInvalid)  \
    XX(LPC, TrackAudio, 7, 8000, 1, CodecInvalid)          \
    XX(PCMA, TrackAudio, 8, 8000, 1, CodecG711A)           \
    XX(G722, TrackAudio, 9, 16000, 1, CodecG722)           \
    XX(L16_Stereo, TrackAudio, 10, 44100, 2, CodecInvalid) \
    XX(L16_Mono, TrackAudio, 11, 44100, 1, CodecInvalid)   \
    XX(QCELP, TrackAudio, 12, 8000, 1, CodecInvalid)       \
    XX(CN, TrackAudio, 13, 8000, 1, CodecInvalid)          \
    XX(MP3, TrackAudio, 14, 44100, 2, CodecMP3)            \
    XX(G728, TrackAudio, 15, 8000, 1, CodecG728)           \
    XX(DVI4_11025, TrackAudio, 16, 11025, 1, CodecInvalid) \
    XX(DVI4_22050, TrackAudio, 17, 22050, 1, CodecInvalid) \
    XX(G729, TrackAudio, 18, 8000, 1, CodecG729)           \
    XX(CelB, TrackVideo, 25, 90000, 1, CodecInvalid)       \
    XX(JPEG, TrackVideo, 26, 90000, 1, CodecJPEG)          \
    XX(nv, TrackVideo, 28, 90000, 1, CodecInvalid)         \
    XX(H261, TrackVideo, 31, 90000, 1, CodecInvalid)       \
    XX(MPV, TrackVideo, 32, 90000, 1, CodecInvalid)        \
    XX(MP2T, TrackVideo, 33, 90000, 1, CodecTS)            \
    XX(H263, TrackVideo, 34, 90000, 1, CodecInvalid)

typedef enum {
#define ENUM_DEF(name, type, value, clock_rate, channel, codec_id) PT_##name = value,
    RTP_PT_MAP(ENUM_DEF)
#undef ENUM_DEF
        PT_MAX = 128
} PayloadType;

class RtpPayload {
public:
    static int getClockRate(int pt);
    static int getClockRateByCodec(CodecId codec);
    static TrackType getTrackType(int pt);
    static int getAudioChannel(int pt);
    static const char *getName(int pt);
    static CodecId getCodecId(int pt);

private:
    RtpPayload() = delete;
    ~RtpPayload() = delete;
};
#endif // SRC_ICE_FRAME_H_