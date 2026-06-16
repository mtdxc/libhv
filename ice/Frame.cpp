#include <map>
#include "Frame.h"
using namespace std;

TrackType getTrackType(CodecId codecId) {
    switch (codecId) {
#define XX(name, type, value, str, mpeg_id, mp4_id, mkv_id) case name : return type;
        CODEC_MAP(XX)
#undef XX
        default : return TrackInvalid;
    }
}


const char *getCodecName(CodecId codec) {
    switch (codec) {
#define XX(name, type, value, str, mpeg_id, mp4_id, mkv_id) case name : return str;
        CODEC_MAP(XX)
#undef XX
        default : return "invalid";
    }
}

#define XX(name, type, value, str, mpeg_id, mp4_id, mkv_id) {str, name},
static map<string, CodecId, StrCaseCompare> codec_map = { CODEC_MAP(XX) };
#undef XX

CodecId getCodecId(const string &str){
    auto it = codec_map.find(str);
    return it == codec_map.end() ? CodecInvalid : it->second;
}

static map<string, TrackType, StrCaseCompare> track_str_map = {
        {"video",       TrackVideo},
        {"audio",       TrackAudio},
        {"application", TrackApplication}
};

TrackType getTrackType(const string &str) {
    auto it = track_str_map.find(str);
    return it == track_str_map.end() ? TrackInvalid : it->second;
}

const char* getTrackString(TrackType type){
    switch (type) {
        case TrackVideo : return "video";
        case TrackAudio : return "audio";
        case TrackApplication : return "application";
        default: return "invalid";
    }
}


int RtpPayload::getClockRate(int pt) {
    switch (pt) {
#define XX(name, type, value, clock_rate, channel, codec_id)                                                                                          \
    case value: return clock_rate;
        RTP_PT_MAP(XX)
#undef XX
        default: return 90000;
    }
}

int RtpPayload::getClockRateByCodec(CodecId codec) {
#define XX(name, type, value, clock_rate, channel, codec_id) { codec_id, clock_rate },
    static map<CodecId, int> s_map = { RTP_PT_MAP(XX) };
#undef XX
    auto it = s_map.find(codec);
    if (it == s_map.end()) {
        // WarnL << "Unsupported codec: " << getCodecName(codec);
        return 90000;
    }
    return it->second;
}

TrackType RtpPayload::getTrackType(int pt) {
    switch (pt) {
#define XX(name, type, value, clock_rate, channel, codec_id)                                                                                          \
    case value: return type;
        RTP_PT_MAP(XX)
#undef XX
        default: return TrackInvalid;
    }
}

int RtpPayload::getAudioChannel(int pt) {
    switch (pt) {
#define XX(name, type, value, clock_rate, channel, codec_id)                                                                                          \
    case value: return channel;
        RTP_PT_MAP(XX)
#undef XX
        default: return 1;
    }
}

const char *RtpPayload::getName(int pt) {
    switch (pt) {
#define XX(name, type, value, clock_rate, channel, codec_id)                                                                                          \
    case value: return #name;
        RTP_PT_MAP(XX)
#undef XX
        default: return "unknown payload type";
    }
}

CodecId RtpPayload::getCodecId(int pt) {
    switch (pt) {
#define XX(name, type, value, clock_rate, channel, codec_id)                                                                                          \
    case value: return codec_id;
        RTP_PT_MAP(XX)
#undef XX
        default: return CodecInvalid;
    }
}
