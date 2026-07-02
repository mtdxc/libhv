#include <map>
#include "Frame.h"
#include "hlog.h"
#include "rtp/RtpPacket.h"
using namespace std;
using namespace ice;

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

CodecId getCodecId(const string &str) {
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

const char* getTrackString(TrackType type) {
    switch (type) {
        case TrackVideo : return "video";
        case TrackAudio : return "audio";
        case TrackApplication : return "application";
        default: return "invalid";
    }
}

const char *CodecInfo::getCodecName() const {
    return ::getCodecName(getCodecId());
}

TrackType CodecInfo::getTrackType() const {
    return ::getTrackType(getCodecId());
}

std::string CodecInfo::getTrackTypeStr() const {
    return getTrackString(getTrackType());
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
        // AAC uses dynamic PT, default to common sample rate
        if (codec == CodecAAC) return 44100;
        if (codec == CodecOpus) return 48000;
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

bool RtpPacket::IsKeyFrame() const {
    auto payload = getPayload();
    size_t payloadSize = getPayloadSize();
    if (payloadSize == 0) {
        return false;
    }
    auto it = extMap.find((uint8_t)RtpExtType::framemarking);
    if (it != extMap.end()) {
        auto mark = it->second.getFrameMarking();
        return mark && mark->independent && mark->start;
    }

    switch (codec)
    {
    case CodecH264:
    {
        uint8_t nalType = payload[0] & 0x1F;
        if (nalType == 5) { // IDR
            return true;
        }
        if (nalType == 28 && payloadSize >= 2) { // FU-A
            uint8_t fuHeader = payload[1];
            if ((fuHeader & 0x80) && ((fuHeader & 0x1F) == 5)) {
                return true;
            }
        }
        break;
    }
    case CodecVP8:
    {
        size_t idx = 0;
        uint8_t desc = payload[idx++];
        bool startOfPartition = (desc & 0x10) != 0;

        if (desc & 0x80) { // X: extension
            if (idx >= payloadSize) break;
            uint8_t ext = payload[idx++];
            if (ext & 0x80) { // I: PictureID
                if (idx >= payloadSize) break;
                if (payload[idx] & 0x80) idx++; // 2-byte PID
                idx++;
            }
            if (ext & 0x40) idx++; // L: TL0PICIDX
            if (ext & 0x20) idx++; // T: TID
            if (ext & 0x10) idx++; // K: KEYIDX
        }

        if (startOfPartition && idx < payloadSize) {
            if ((payload[idx] & 0x01) == 0) { // Keyframe
                return true;
            }
        }
        break;
    }
    case CodecVP9:
    {
        uint8_t desc = payload[0];
        bool pBit = (desc & 0x40) != 0; // P: 0 = keyframe
        if (!pBit) {
            return true;
        }
        break;
    }
    case CodecAV1:
    {
        uint8_t aggHeader = payload[0];
        bool nBit = (aggHeader & 0x08) != 0; // N: new coded video sequence
        if (nBit) {
            return true;
        }
        uint8_t wField = (aggHeader >> 4) & 0x03;
        if (wField <= 1 && payloadSize >= 2) {
            uint8_t obuHeader = payload[1];
            uint8_t obuType = (obuHeader >> 3) & 0x0F;
            if (obuType == 1) { // OBU_SEQUENCE_HEADER
                return true;
            }
        }
        break;
    }
    case CodecH265:
    {
        uint8_t nalType = (payload[0] >> 1) & 0x3F;
        // IDR_W_RADL=19, IDR_N_LP=20, CRA=21
        if (nalType == 19 || nalType == 20 || nalType == 21) {
            return true;
        }
        if (nalType == 49 && payloadSize >= 3) { // FU
            uint8_t fuHeader = payload[2];
            uint8_t fuType = fuHeader & 0x3F;
            if ((fuHeader & 0x80) && (fuType == 19 || fuType == 20 || fuType == 21)) {
                return true;
            }
        }
        break;
    }
    default:
        break;
    }
    return false;
}

// Frame实现
Frame::Ptr Frame::clone() const {
    auto ret = std::make_shared<Frame>();
    ret->codec = codec;
    ret->is_key = is_key;
    ret->dts = dts;
    ret->pts = pts;
    ret->data_ = data_;
    return ret;
}

void Frame::appendNal(const uint8_t *bytes, size_t len) {
    static uint8_t nalHdr[] = {0x00, 0x00, 0x00, 0x01};
    appendData(nalHdr, 4);
    appendData(bytes, len);
}

size_t findNalStart(const uint8_t *data, size_t size, size_t &startCodeLen) {
    for (size_t i = 0; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                startCodeLen = 3;
                return i;
            }
            if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                startCodeLen = 4;
                return i;
            }
        }
    }
    return std::string::npos;
}

void Frame::forEachNal(std::function<bool(const uint8_t* nal, size_t size)> cb) const {
    const uint8_t *data = data_.data();
    size_t size = data_.size();
    int pos = 0;
    while (pos < size) {
        size_t startCodeLen = 0;
        size_t nalStart = findNalStart(data + pos, size - pos, startCodeLen);
        if (nalStart == std::string::npos) {
            break;
        }
        nalStart += pos + startCodeLen;
        size_t nalEnd = findNalStart(data + nalStart, size - nalStart, startCodeLen);
        if (nalEnd == std::string::npos) {
            nalEnd = size;
        } else {
            nalEnd += nalStart;
        }
        if (!cb(data + nalStart, nalEnd - nalStart)) {
            return;
        }
        pos = nalEnd;
    }
    if (pos != size) {
        cb(data + pos, size - pos);
    }
}

bool Frame::keyFrame() const{
    bool ret = false;
    switch (codec)
    {
    case CodecH264:
        // H264 keyframe detection
        forEachNal([&](const uint8_t* nal, size_t size) {
            uint8_t nalType = nal[0] & 0x1F;
            if (nalType == 5 || nalType == 7 || nalType == 8) { // IDR, SPS, PPS
                ret = true;
                return false; // stop iteration
            }
            return true; // continue iteration
        });
        break;
    case CodecH265:
        // H265 keyframe detection
        forEachNal([&](const uint8_t* nal, size_t size) {
            uint8_t nalType = (nal[0] >> 1) & 0x3F;
            if (nalType == 19 || nalType == 20 || nalType == 21) { // IDR_W_RADL, IDR_N_LP, CRA
                ret = true;
                return false; // stop iteration
            }
            return true; // continue iteration
        });
        break;
    case CodecVP8:
        // VP8 keyframe detection
        if (size() >= 1) {
            uint8_t desc = data()[0];
            bool startOfPartition = (desc & 0x10) != 0;
            if (startOfPartition && (data()[0] & 0x01) == 0) { // Keyframe
                return true;
            }
        }
        break;
    case CodecVP9:
        // VP9 keyframe detection
        if (size() >= 1) {
            uint8_t desc = data()[0];
            bool pBit = (desc & 0x40) != 0; // P: 0 = keyframe
            if (!pBit) {
                return true;
            }
        }
        break;
    case CodecAV1:
        // AV1 keyframe detection
        if (size() >= 1) {
            uint8_t aggHeader = data()[0];
            bool nBit = (aggHeader & 0x08) != 0; // N: new coded video sequence
            if (nBit) {
                return true;
            }
            uint8_t wField = (aggHeader >> 4) & 0x03;
            if (wField <= 1 && size() >= 2) {
                uint8_t obuHeader = data()[1];
                uint8_t obuType = (obuHeader >> 3) & 0x0F;
                if (obuType == 1) { // OBU_SEQUENCE_HEADER
                    return true;
                }
            }
        }
        break;
    default:
        break;
    }
    return ret;
}

// 解析 Annex-B 格式的 H264/H265 NAL 单元
struct NalUnit { const uint8_t *data; size_t len; };

std::vector<NalUnit> parseAnnexB(const uint8_t *data, size_t len) {
    std::vector<NalUnit> nals;
    size_t i = 0;
    // 找第一个 start code
    while (i + 2 < len) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) { i += 3; break; }
            if (i + 3 < len && data[i + 2] == 0 && data[i + 3] == 1) { i += 4; break; }
        }
        i++;
    }
    // 逐个提取 NAL
    size_t start = i;
    while (i + 2 < len) {
        if (data[i] == 0 && data[i + 1] == 0) {
            size_t sc_len = 2;
            if (data[i + 2] == 1) {
                sc_len = 3;
            } else if (i + 3 < len && data[i + 2] == 0 && data[i + 3] == 1) {
                sc_len = 4;
            } else {
                i++;
                continue;
            }
            if (i > start) {
                nals.push_back({data + start, i - start});
            }
            i += sc_len;
            start = i;
        } else {
            i++;
        }
    }
    // 最后一个 NAL
    if (start < len) {
        nals.push_back({data + start, len - start});
    }
    return nals;
}

// ---------- H264 ----------
bool Frame::extractH264Nal(const RtpPacket::Ptr &rtp) {
    const uint8_t *payload = rtp->getPayload();
    size_t psz = rtp->getPayloadSize();
    if (psz < 1) return false;

    uint8_t nalType = payload[0] & 0x1F;

    if (nalType >= 1 && nalType <= 23) {
        // Single NAL unit
        appendNal(payload, psz);
        if (nalType == 5 || nalType == 7 || nalType == 8) is_key = true;   // IDR
    } else if (nalType == 24) {
        // STAP-A: aggregated NAL units
        size_t offset = 1;
        while (offset + 2 <= psz) {
            uint16_t nalSize = (payload[offset] << 8) | payload[offset + 1];
            offset += 2;
            if (offset + nalSize > psz) return false;
            appendNal(payload + offset, nalSize);
            uint8_t t = payload[offset] & 0x1F;
            if (t == 5 || t == 7 || t == 8) is_key = true;
            offset += nalSize;
        }
    } else if (nalType == 28) {
        // FU-A: fragmented NAL unit
        if (psz < 2) return false;
        uint8_t fuHeader = payload[1];
        bool startBit = (fuHeader & 0x80) != 0;
        uint8_t fuNalType = fuHeader & 0x1F;

        if (startBit) {
            // 重建原始NAL头：F | NRI | type
            uint8_t nalHeader = (payload[0] & 0xE0) | fuNalType;
            std::vector<uint8_t> nal;
            nal.push_back(nalHeader);
            nal.insert(nal.end(), payload + 2, payload + psz);
            appendNal(nal.data(), nal.size());
            if (fuNalType == 5) is_key = true;
        } else {
            // 后续分片：直接追加数据
            appendData(payload + 2, psz - 2);
        }
    } else {
        // 未处理的NAL类型(STAP-B/FU-B/MTAP等)，作为原始数据追加
        appendNal(payload, psz);
    }
    return true;
}

Frame::RtpPackets Frame::packetizeH264(uint8_t pt, uint32_t ssrc,
                                                  uint16_t &seq, uint16_t mtu) {
    RtpPackets pkts;
    auto nals = parseAnnexB(this->data(), this->size());
    if (nals.empty()) return pkts;

    size_t stap_max = mtu - 1; // 1 byte STAP-A header
    std::vector<uint8_t> stap;
    int stap_count = 0;

    auto emitStap = [&](bool is_last_nal_group) {
        if (stap_count > 0 && !stap.empty()) {
            pkts.push_back(RtpPacket::create(codec, pt, ssrc, seq++, pts, is_last_nal_group, stap.data(), stap.size()));
        }
        stap.clear();
        stap_count = 0;
    };

    for (size_t idx = 0; idx < nals.size(); idx++) {
        auto &nal = nals[idx];
        bool last = (idx == nals.size() - 1);

        if (nal.len <= mtu) {
            // 尝试 STAP-A 聚合（仅限小 NAL：SPS/PPS/SEI 等）
            size_t needed = 2 + nal.len; // 2-byte size + nal data
            if (stap_count > 0 && stap.size() + needed <= stap_max) {
                stap.push_back((nal.len >> 8) & 0xFF);
                stap.push_back(nal.len & 0xFF);
                stap.insert(stap.end(), nal.data, nal.data + nal.len);
                stap_count++;
            } else if (stap_count > 0) {
                // 当前 STAP 装不下了，先输出
                emitStap(false);
                // 新 STAP
                stap.push_back(24); // STAP-A type
                stap.push_back((nal.len >> 8) & 0xFF);
                stap.push_back(nal.len & 0xFF);
                stap.insert(stap.end(), nal.data, nal.data + nal.len);
                stap_count++;
            } else {
                // 第一个小 NAL，开始新的 STAP
                stap.push_back(24); // STAP-A type
                stap.push_back((nal.len >> 8) & 0xFF);
                stap.push_back(nal.len & 0xFF);
                stap.insert(stap.end(), nal.data, nal.data + nal.len);
                stap_count++;
            }
        } else {
            // 大 NAL 无法聚合，先刷出 STAP
            emitStap(false);

            // FU-A 分片
            uint8_t nal_header = nal.data[0];
            uint8_t fu_indicator = (nal_header & 0xE0) | 28;
            const uint8_t *nal_body = nal.data + 1;
            size_t remaining = nal.len - 1;
            size_t chunk = mtu - 2; // FU indicator + FU header
            bool first = true;

            while (remaining > 0) {
                size_t frag = std::min(chunk, remaining);
                bool last_frag = (remaining - frag == 0);

                std::vector<uint8_t> payload(2 + frag);
                payload[0] = fu_indicator;
                payload[1] = (nal_header & 0x1F);
                if (first)     payload[1] |= 0x80; // Start bit
                if (last_frag) payload[1] |= 0x40; // End bit
                memcpy(payload.data() + 2, nal_body, frag);

                bool mark = last && last_frag;
                pkts.push_back(RtpPacket::create(codec, pt, ssrc, seq++, pts, mark, payload.data(), payload.size()));

                nal_body += frag;
                remaining -= frag;
                first = false;
            }
        }
    }

    // 刷出残留的 STAP
    emitStap(true);
    return pkts;
}

// ---------- H265 ----------
bool Frame::extractH265Nal(const RtpPacket::Ptr &rtp) {
    const uint8_t *payload = rtp->getPayload();
    size_t psz = rtp->getPayloadSize();
    if (psz < 2) return false;

    uint8_t nalType = (payload[0] >> 1) & 0x3F;

    if (nalType <= 47) {
        // Single NAL unit
        appendNal(payload, psz);
        // VPS=32, SPS=33, PPS=34, IDR_W_RADL=19, IDR_N_LP=20, CRA=21
        if (nalType == 19 || nalType == 20 || nalType == 21 ||
            nalType == 32 || nalType == 33 || nalType == 34) {
            is_key = true;
        }
    } else if (nalType == 48) {
        // AP: aggregated NAL units
        size_t offset = 2;
        while (offset + 2 <= psz) {
            uint16_t nalSize = (payload[offset] << 8) | payload[offset + 1];
            offset += 2;
            if (offset + nalSize > psz) return false;
            appendNal(payload + offset, nalSize);
            uint8_t t = (payload[offset] >> 1) & 0x3F;
            if (t == 19 || t == 20 || t == 21 || t == 32 || t == 33 || t == 34) is_key = true;
            offset += nalSize;
        }
    } else if (nalType == 49) {
        // FU: fragmented NAL unit
        if (psz < 3) return false;
        uint8_t fuHeader = payload[2];
        bool startBit = (fuHeader & 0x80) != 0;
        uint8_t fuNalType = fuHeader & 0x3F;

        if (startBit) {
            // 重建原始2字节NAL头
            uint8_t nalHeader0 = (payload[0] & 0x81) | (fuNalType << 1);
            uint8_t nalHeader1 = payload[1];
            std::vector<uint8_t> nal;
            nal.push_back(nalHeader0);
            nal.push_back(nalHeader1);
            nal.insert(nal.end(), payload + 3, payload + psz);
            appendNal(nal.data(), nal.size());
            if (fuNalType == 19 || fuNalType == 20 || fuNalType == 21 ||
                fuNalType == 32 || fuNalType == 33 || fuNalType == 34) {
                is_key = true;
            }
        } else {
            // 后续分片：直接追加数据
            appendData(payload + 3, psz - 3);
        }
    } else {
        appendNal(payload, psz);
    }
    return true;
}

Frame::RtpPackets Frame::packetizeH265(uint8_t pt, uint32_t ssrc,
                                                  uint16_t &seq, uint16_t mtu) {
    RtpPackets pkts;
    auto nals = parseAnnexB(this->data(), this->size());
    if (nals.empty()) return pkts;

    size_t ap_max = mtu - 2; // 2-byte AP header
    std::vector<uint8_t> ap;
    int ap_count = 0;

    auto emitAp = [&](bool is_last_nal_group) {
        if (ap_count > 0 && !ap.empty()) {
            pkts.push_back(RtpPacket::create(codec, pt, ssrc, seq++, pts, is_last_nal_group, ap.data(), ap.size()));
        }
        ap.clear();
        ap_count = 0;
    };

    for (size_t idx = 0; idx < nals.size(); idx++) {
        auto &nal = nals[idx];
        bool last = (idx == nals.size() - 1);

        if (nal.len <= mtu) {
            size_t needed = 2 + nal.len;
            if (ap_count > 0 && ap.size() + needed <= ap_max) {
                ap.push_back((nal.len >> 8) & 0xFF);
                ap.push_back(nal.len & 0xFF);
                ap.insert(ap.end(), nal.data, nal.data + nal.len);
                ap_count++;
            } else if (ap_count > 0) {
                emitAp(false);
                // 新 AP
                uint8_t ap_hdr0 = (48 << 1); // AP type=48
                ap.push_back(ap_hdr0);
                ap.push_back(nal.data[1]); // 复制第二个NAL头字节
                ap.push_back((nal.len >> 8) & 0xFF);
                ap.push_back(nal.len & 0xFF);
                ap.insert(ap.end(), nal.data, nal.data + nal.len);
                ap_count++;
            } else {
                uint8_t ap_hdr0 = (48 << 1); // AP type=48
                ap.push_back(ap_hdr0);
                ap.push_back(nal.data[1]);
                ap.push_back((nal.len >> 8) & 0xFF);
                ap.push_back(nal.len & 0xFF);
                ap.insert(ap.end(), nal.data, nal.data + nal.len);
                ap_count++;
            }
        } else {
            emitAp(false);

            // FU 分片 (3-byte FU header)
            uint8_t nal_hdr0 = nal.data[0];
            uint8_t nal_hdr1 = nal.data[1];
            uint8_t fu_nal_type = (nal_hdr0 >> 1) & 0x3F;
            uint8_t fu_hdr0 = (nal_hdr0 & 0x81) | (49 << 1); // FU type=49

            const uint8_t *nal_body = nal.data + 2;
            size_t remaining = nal.len - 2;
            size_t chunk = mtu - 3; // FU header: 3 bytes
            bool first = true;

            while (remaining > 0) {
                size_t frag = std::min(chunk, remaining);
                bool last_frag = (remaining - frag == 0);

                std::vector<uint8_t> payload(3 + frag);
                payload[0] = fu_hdr0;
                payload[1] = nal_hdr1;
                payload[2] = fu_nal_type;
                if (first)     payload[2] |= 0x80; // Start
                if (last_frag) payload[2] |= 0x40; // End
                memcpy(payload.data() + 3, nal_body, frag);

                bool mark = last && last_frag;
                pkts.push_back(RtpPacket::create(codec, pt, ssrc, seq++, pts, mark, payload.data(), payload.size()));

                nal_body += frag;
                remaining -= frag;
                first = false;
            }
        }
    }

    emitAp(true);
    return pkts;
}

// ---------- VP8 ----------

bool Frame::extractVp8Data(const RtpPacket::Ptr &rtp) {
    // VP8 RTP Payload Format (RFC 7741)
    //
    //  0 1 2 3 4 5 6 7
    // +-+-+-+-+-+-+-+-+
    // |X|R|N|S|R| PID |   (REQUIRED)
    // +-+-+-+-+-+-+-+-+
    // X: Extended control bits
    // R: Reserved (must be 0)
    // N: Non-reference frame
    // S: Start of VP8 partition
    // PID: Partition index
    //
    // Optional extensions (when X=1):
    // +-+-+-+-+-+-+-+-+
    // |I|L|T|K| RSV   |
    // +-+-+-+-+-+-+-+-+
    // I: PictureID present
    // L: TL0PICIDX present
    // T/K: TID/KEYIDX present
    //
    // I=1 → PictureID (1 byte, or 2 bytes if high bit set)
    // L=1 → TL0PICIDX (1 byte)
    // T=1 or K=1 → TID/KEYIDX (1 byte)
    //
    // VP8 uncompressed data chunk follows the payload descriptor.
    // First chunk (S=1) contains 3-byte uncompressed header + keyframe detection.

    const uint8_t *payload = rtp->getPayload();
    size_t psz = rtp->getPayloadSize();
    if (psz < 1) return false;

    size_t offset = 0;
    uint8_t x = (payload[offset] >> 7) & 1;
    uint8_t s = (payload[offset] >> 4) & 1;
    offset++;

    if (x) {
        if (offset >= psz) return false;
        uint8_t i = (payload[offset] >> 7) & 1;
        uint8_t l = (payload[offset] >> 6) & 1;
        uint8_t t = (payload[offset] >> 5) & 1;
        uint8_t k = (payload[offset] >> 4) & 1;
        offset++;
        if (i) {
            if (offset >= psz) return false;
            if (payload[offset] & 0x80) offset++; // 2-byte PictureID
            offset++;
        }
        if (l) offset++;
        if (t || k) offset++;
    }

    if (offset > psz) return false;

    // 第一个分片(S=1)的VP8 uncompressed header前3字节:
    // byte0: frame_type(bit0), version(bits1-3), show_frame(bit4), etc.
    // frame_type=0 → keyframe, frame_type=1 → inter frame
    if (s && offset + 3 <= psz) {
        uint8_t frame_type = payload[offset] & 0x01;
        if (frame_type == 0) {
            is_key = true;
        }
    }

    // VP8编码数据（剥离Payload Descriptor后）
    if (psz > offset) {
        appendData(payload + offset, psz - offset);
    }
    return true;
}

Frame::RtpPackets Frame::packetizeVp8(uint8_t pt, uint32_t ssrc,
                                                 uint16_t &seq, uint16_t mtu) {
    RtpPackets pkts;
    // VP8 Payload Descriptor: 1 byte (minimal, no extensions)
    // |X=0|R=0|N=0|S|R| PID=0 |
    // S=1 for the first packet of each partition (first packet of frame)
    const uint8_t *data = this->data();
    size_t remaining = this->size();
    size_t chunk = mtu - 1; // 1 byte payload descriptor
    bool first = true;

    while (remaining > 0) {
        size_t frag = std::min(chunk, remaining);
        bool last = (remaining - frag == 0);

        std::vector<uint8_t> payload(1 + frag);
        payload[0] = first ? 0x10 : 0x00; // S=1 for first packet
        memcpy(payload.data() + 1, data, frag);

        pkts.push_back(RtpPacket::create(codec, pt, ssrc, seq++, pts, last, payload.data(), payload.size()));

        data += frag;
        remaining -= frag;
        first = false;
    }
    return pkts;
}

// ---------- VP9 ----------

bool Frame::extractVp9Data(const RtpPacket::Ptr &rtp) {
    // VP9 RTP Payload Format (draft-ietf-payload-vp9)
    //
    //  0 1 2 3 4 5 6 7
    // +-+-+-+-+-+-+-+-+
    // |I|P|L|F|B|E|V|U|   (REQUIRED)
    // +-+-+-+-+-+-+-+-+
    // I: Picture ID present
    // P: Inter-picture predicted frame
    // L: Layer indices present
    // F: Flexible mode
    // B: Start of frame
    // E: End of frame
    // V: Scalability structure present
    // U: Reserved
    //
    // I=1 → PictureID (1 byte, or 2 bytes if M=1)
    // L=1 → TL0PICIDX(1) + TID/U(1) [+ REF fields if F=0]
    // F=0 && L=1 → P_DIFF(1) [+ P_DIFF(1)] [+ P_DIFF(1)]
    // V=1 → scalability structure (variable length)

    const uint8_t *payload = rtp->getPayload();
    size_t psz = rtp->getPayloadSize();
    if (psz < 1) return false;

    size_t offset = 0;
    uint8_t i = (payload[offset] >> 7) & 1;
    uint8_t p = (payload[offset] >> 6) & 1;
    uint8_t l = (payload[offset] >> 5) & 1;
    uint8_t f = (payload[offset] >> 4) & 1;
    uint8_t b = (payload[offset] >> 3) & 1;
    offset++;

    if (i) {
        if (offset >= psz) return false;
        bool m = (payload[offset] & 0x80) != 0;
        offset++; // first byte of PictureID
        if (m) offset++; // second byte
    }

    if (l) {
        if (offset >= psz) return false;
        offset++; // TL0PICIDX
        offset++; // TID/U
        if (!f) {
            // Non-flexible mode: reference indices
            if (offset >= psz) return false;
            uint8_t ref = payload[offset++];
            if (ref & 0x04) { if (offset >= psz) return false; offset++; } // P_DIFF
            if (ref & 0x02) { if (offset >= psz) return false; offset++; }
            if (ref & 0x01) { if (offset >= psz) return false; offset++; }
        }
    }

    // V: Scalability structure (B=1时才有)
    // 实际实现中很少见到，简化跳过

    if (offset > psz) return false;

    // B=1且P=0 → 关键帧
    if (b && !p) {
        is_key = true;
    }

    // VP9编码数据（剥离Payload Descriptor后）
    if (psz > offset) {
        appendData(payload + offset, psz - offset);
    }
    return true;
}

Frame::RtpPackets Frame::packetizeVp9(uint8_t pt, uint32_t ssrc,
                                                 uint16_t &seq, uint16_t mtu) {
    RtpPackets pkts;
    // VP9 Payload Descriptor: 1 byte (minimal)
    // |I=0|P|L=0|F=0|B|E|V=0|U=0|
    // P=1 for inter frame, B=1 first packet, E=1 last packet
    const uint8_t *data = this->data();
    size_t remaining = this->size();
    size_t chunk = mtu - 1; // 1 byte payload descriptor
    bool first = true;

    while (remaining > 0) {
        size_t frag = std::min(chunk, remaining);
        bool last = (remaining - frag == 0);

        uint8_t desc = 0;
        if (!is_key) desc |= 0x40; // P=1 (inter frame)
        if (first) desc |= 0x08;            // B=1 (start of frame)
        if (last)  desc |= 0x04;            // E=1 (end of frame)

        std::vector<uint8_t> payload(1 + frag);
        payload[0] = desc;
        memcpy(payload.data() + 1, data, frag);

        pkts.push_back(RtpPacket::create(codec, pt, ssrc, seq++, pts, last, payload.data(), payload.size()));

        data += frag;
        remaining -= frag;
        first = false;
    }
    return pkts;
}

// ---------- AV1 ----------

bool Frame::extractAv1Data(const RtpPacket::Ptr &rtp) {
    // AV1 RTP Payload Format (AV1 RTP specification)
    //
    // OBU Aggregation Header (1 byte):
    //  0 1 2 3 4 5 6 7
    // +-+-+-+-+-+-+-+-+
    // |Z|Y|  N  |W| R |
    // +-+-+-+-+-+-+-+-+
    // Z: continuation of previous OBU from last packet
    // Y: last element's size is NOT present (1 = no size for last element)
    // N: number of OBU elements (2 bits), 0 means 4 elements
    // W: reserved (2 bits)
    // R: reserved (2 bits, must be 0)
    //
    // OBU elements (each has optional LEB128 size + payload):
    //   - If N > 1, all elements except the last have LEB128 size prefix
    //   - If Y=0, the last element also has LEB128 size
    //   - If Y=1, the last element runs to end of payload (no size)
    //   - If Z=1, the first element is continuation of previous packet's last OBU
    //
    // OBU types (bits 3-6 of OBU header byte):
    //   1  = Sequence Header
    //   2  = Temporal Delimiter
    //   3  = Frame Header
    //   6  = Frame (header + tile data)
    //   15 = Padding

    const uint8_t *payload = rtp->getPayload();
    size_t psz = rtp->getPayloadSize();
    if (psz < 1) return false;

    // Parse aggregation header
    uint8_t z = (payload[0] >> 7) & 1;
    uint8_t y = (payload[0] >> 6) & 1;
    uint8_t n = (payload[0] >> 4) & 3;
    if (n == 0) n = 4; // N=0 means 4 elements
    size_t offset = 1;

    // Collect element (offset, size) pairs
    struct Element { size_t off; size_t sz; };
    std::vector<Element> elements;

    for (int i = 0; i < n; i++) {
        if (offset >= psz) break;

        bool is_last = (i == n - 1);
        // last element with Y=1 has no size field, runs to end of payload
        bool has_size = !(is_last && y);

        if (has_size) {
            // LEB128 decode
            size_t obu_size = 0;
            int shift = 0;
            bool more = true;
            while (more && offset < psz) {
                uint8_t b = payload[offset++];
                obu_size |= (size_t)(b & 0x7F) << shift;
                shift += 7;
                if (!(b & 0x80)) more = false;
            }
            if (offset + obu_size > psz) return false;
            elements.push_back({offset, obu_size});
            offset += obu_size;
        } else {
            // Last element: runs to end of payload
            elements.push_back({offset, psz - offset});
        }
    }

    // Detect key frame: first OBU in a new frame (Z=0) with type=1 (Sequence Header)
    // In practice, a keyframe typically starts with SeqHdr + TD + Frame Header
    if (!z && !elements.empty()) {
        size_t off = elements[0].off;
        if (off < psz) {
            uint8_t obu_type = (payload[off] >> 3) & 0x0F;
            if (obu_type == 1) { // Sequence Header
                is_key = true;
            }
        }
    }

    // Output OBU data (strip aggregation header, keep raw OBU bytes)
    // Z=1: first element is continuation, append without OBU header
    size_t start = 0;
    if (z && !elements.empty()) {
        auto &e = elements[0];
        appendData(payload + e.off, e.sz);
        start = 1;
    }
    // Remaining elements: complete OBUs with header + payload
    for (size_t i = start; i < elements.size(); i++) {
        auto &e = elements[i];
        appendData(payload + e.off, e.sz);
    }

    return true;
}

Frame::RtpPackets Frame::packetizeAv1(uint8_t pt, uint32_t ssrc,
                                                 uint16_t &seq,
                                                 uint16_t mtu) {
    RtpPackets pkts;
    // AV1 Aggregation Header: |Z=0|Y=1|N=0(=1)|W=0|R=0| → 0x40
    // 简单模式：每个 RTP 包包含 1 个 OBU (N=1, Y=1)
    const uint8_t *data = this->data();
    size_t remaining = this->size();
    size_t chunk = mtu - 1; // 1 byte aggregation header

    while (remaining > 0) {
        size_t frag = std::min(chunk, remaining);
        bool last = (remaining - frag == 0);

        std::vector<uint8_t> payload(1 + frag);
        payload[0] = 0x40; // Z=0, Y=1, N=0(1 element)
        memcpy(payload.data() + 1, data, frag);

        pkts.push_back(RtpPacket::create(codec, pt, ssrc, seq++, pts, last, payload.data(), payload.size()));

        data += frag;
        remaining -= frag;
    }
    return pkts;
}

// ---------- AAC ----------
bool Frame::extractAacData(const RtpPacketPtr &rtp) {
    // AAC RTP Payload Format (RFC 3640, mpeg4-generic)
    //
    // AU Header Section:
    //  0                   1
    //  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |      AU-headers-length        |   (16 bits, total bits of all AU headers)
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |      AU-header (16 bits)      |   (AU-size:13 + AU-Index:3)
    // |          ...                  |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // Audio data follows the AU header section.
    // Each AU header: 13-bit size (bytes) + 3-bit index/delta.
    // Typically one AU per RTP packet for low-latency.

    const uint8_t *payload = rtp->getPayload();
    size_t psz = rtp->getPayloadSize();
    if (psz < 4) return false; // At minimum: 2-byte length + 2-byte header

    uint16_t au_headers_length_bits = (payload[0] << 8) | payload[1];
    size_t au_headers_length_bytes = (au_headers_length_bits + 7) / 8;
    size_t offset = 2 + au_headers_length_bytes;
    if (offset > psz) return false;

    // Parse AU headers
    size_t header_offset = 2;
    int num_headers = au_headers_length_bits / 16; // Each AU header is 16 bits
    for (int i = 0; i < num_headers && offset <= psz; i++) {
        if (header_offset + 2 > 2 + au_headers_length_bytes) break;
        uint16_t au_header = (payload[header_offset] << 8) | payload[header_offset + 1];
        uint16_t au_size = au_header >> 3; // 13-bit size
        // uint8_t au_index = au_header & 0x07; // 3-bit index (unused for basic assembly)
        header_offset += 2;

        if (offset + au_size > psz) {
            hlogw("AAC AU size overflow: offset=%zu, au_size=%u, psz=%zu", offset, au_size, psz);
            return false;
        }
        appendData(payload + offset, au_size);
        offset += au_size;
    }

    return true;
}

Frame::RtpPackets Frame::packetizeAac(uint8_t pt, uint32_t ssrc,
                                       uint16_t &seq, uint16_t mtu) {
    RtpPackets pkts;
    // AAC RTP (RFC 3640, mpeg4-generic):
    // Each RTP packet carries one or more AAC Access Units.
    // Format: [AU-headers-length(2B)] [AU-header(2B) per AU] [AU data...]
    //
    // AU-header: 13-bit AU-size (bytes) + 3-bit AU-Index
    // AU-headers-length: total bits of all AU headers
    //
    // For simplicity: one AU per RTP packet.
    // If the AAC frame > mtu-4, split into multiple packets with multiple AUs.

    const uint8_t *data = this->data();
    size_t remaining = this->size();
    // Reserve 4 bytes: 2-byte AU-headers-length + 2-byte AU-header
    size_t max_au = mtu - 4;

    while (remaining > 0) {
        size_t au_size = std::min(max_au, remaining);

        // Build RTP payload: [2B headers-length][2B AU-header][AU data]
        size_t payload_size = 4 + au_size;
        std::vector<uint8_t> payload(payload_size);

        // AU-headers-length = 16 bits (one 16-bit AU header)
        payload[0] = 0x00;
        payload[1] = 0x10; // 16 in big-endian

        // AU-header: size(13 bits) + index(3 bits = 0)
        uint16_t au_header = (au_size << 3) & 0xFFF8;
        payload[2] = (au_header >> 8) & 0xFF;
        payload[3] = au_header & 0xFF;

        memcpy(payload.data() + 4, data, au_size);

        // AAC clock rate is sample rate (typically 44100 or 48000)
        // timestamp is in ms, so rtp_ts = timestamp * clockRate / 1000
        bool mark = (remaining - au_size == 0); // last packet gets marker

        pkts.push_back(RtpPacket::create(codec, pt, ssrc, seq++, pts, mark, payload.data(), payload.size()));

        data += au_size;
        remaining -= au_size;
    }
    return pkts;
}

// ---------- splitToRtp dispatcher ----------

Frame::RtpPackets Frame::splitToRtp(uint8_t pt, uint32_t ssrc, uint16_t &seq, uint16_t mtu) {
    RtpPackets pkts;
    if (size() == 0) return pkts;

    switch (codec) {
    case CodecH264: pkts = packetizeH264(pt, ssrc, seq, mtu); break;
    case CodecH265: pkts = packetizeH265(pt, ssrc, seq, mtu); break;
    case CodecVP8:  pkts = packetizeVp8(pt, ssrc, seq, mtu);  break;
    case CodecVP9:  pkts = packetizeVp9(pt, ssrc, seq, mtu);  break;
    case CodecAV1:  pkts = packetizeAv1(pt, ssrc, seq, mtu);  break;
    case CodecAAC:  pkts = packetizeAac(pt, ssrc, seq, mtu);  break;
    default:
        if (size() > mtu) {
            hlogw("splitToRtp %s not support size %d", getCodecName(), size());
        }
        else {
            pkts.push_back(RtpPacket::create(codec, pt, ssrc, seq++, pts, false, data(), size()));

        }
        return pkts;
    }

    hlogd("split frame: %s, ts=%u, %zu bytes -> %zu pkts, seq %d-%d",
          is_key ? "key" : "delta",
          pts, size(), pkts.size(),
          pkts.empty() ? 0 : pkts.front()->getSeq(),
          pkts.empty() ? 0 : pkts.back()->getSeq());
    return pkts;
}

bool Frame::appendRtp(const RtpPacket::Ptr &rtp) {
    switch (codec) {
    case CodecH264: return extractH264Nal(rtp);
    case CodecH265: return extractH265Nal(rtp);
    case CodecVP8: return extractVp8Data(rtp);
    case CodecVP9: return extractVp9Data(rtp);
    case CodecAV1: return extractAv1Data(rtp);
    case CodecAAC: return extractAacData(rtp);
    default: 
        appendData(rtp->getPayload(), rtp->getPayloadSize());
        return true;
    }
}

static const int aac_sample_rates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000, 7350
};

int Frame::GetAacSampleRate(int index) {
    if (index < 0 || index >= sizeof(aac_sample_rates)/sizeof(aac_sample_rates[0])) {
        return 0;
    }
    return aac_sample_rates[index];
}

int Frame::GetAacSampleRateIndex(int sample_rate) {
    for(int i = 0; i < sizeof(aac_sample_rates)/sizeof(aac_sample_rates[0]); ++i) {
        if (aac_sample_rates[i] == sample_rate) {
            return i;
        }
    }
    return -1; // Not found
}

int Frame::parseAacConfig(const uint8_t* buff, int size, int& sample_rate, int8_t& channel, int8_t& profile) {
    if (size < 2) return -1;
    if (buff[0] == 0xFF && (buff[1] & 0xF0) == 0xF0) {
        // ADTS header
        if (size < 7) {
            return 0;
        }
        profile = ((buff[2] >> 6) & 0x03) + 1; // profile is stored as profile-1
        int sample_rate_index = (buff[2] >> 2) & 0x0F;
        sample_rate = GetAacSampleRate(sample_rate_index);
        channel = ((buff[2] & 0x01) << 2) | ((buff[3] >> 6) & 0x03);
        return 7;
    } else {
        // AudioSpecificConfig
        if (size < 2) {
            return 0;
        }
        profile = (buff[0] >> 3) & 0x1F;
        int sample_rate_index = ((buff[0] & 0x07) << 1) | ((buff[1] >> 7) & 0x01);
        sample_rate = GetAacSampleRate(sample_rate_index);
        channel = (buff[1] >> 3) & 0x0F;
        return 2;
    }
    return 0;
}

int Frame::genAacConfig(uint8_t* buff, int sample_rate, int8_t channel, int8_t profile) {
    int sample_rate_index = GetAacSampleRateIndex(sample_rate);
    if (sample_rate_index < 0) {
        return 0;
    }
    buff[0] = (uint8_t)((profile << 3) | (sample_rate_index >> 1));
    buff[1] = (uint8_t)(((sample_rate_index & 1) << 7) | (channel << 3));
    return 2;
}

int Frame::genAdtsHeader(uint8_t* buff, int size, int sample_rate, int8_t channel, int8_t profile) {
    int freqIdx = Frame::GetAacSampleRateIndex(sample_rate); // 44100 Hz
    int chanCfg = channel; // CPE
    int frameLen = size + 7;
    buff[0] = 0xFF;
    buff[1] = 0xF1;
    buff[2] = ((profile - 1) << 6) | (freqIdx << 2) | (chanCfg >> 2);
    buff[3] = ((chanCfg & 3) << 6) | (frameLen >> 11);
    buff[4] = (frameLen >> 3) & 0xFF;
    buff[5] = ((frameLen & 7) << 5) | 0x1F;
    buff[6] = 0xFC;
    return 7;
}

std::string Frame::toString() const {
    char line[64];
    snprintf(line, sizeof(line), "%s size %d tsp %lld%s", getCodecName(), size(), pts, is_key ? " key" : "");
    return line;
}


class FrameWriterInterfaceHelper : public FrameWriterInterface {
public:
    using Ptr = std::shared_ptr<FrameWriterInterfaceHelper>;
    using onWriteFrame = std::function<bool(const Frame::Ptr &frame)>;

    /**
     * inputFrame后触发onWriteFrame回调
     */
    FrameWriterInterfaceHelper(onWriteFrame cb) { _callback = std::move(cb); }

    /**
     * 写入帧数据
     */
    bool inputFrame(const Frame::Ptr &frame) override { return _callback(frame); }

private:
    onWriteFrame _callback;
};

FrameWriterInterface *FrameDispatcher::addDelegate(FrameWriterInterface::Ptr delegate) {
    FrameWriterInterface *ret = delegate.get();
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (_delegates.find(ret) == _delegates.end()) {
        _delegates[ret] = delegate;
        onSizeChange(_delegates.size());
    } else {
        _delegates[ret] = delegate;
    }
    if (_enable_gop_cache) {
        flushGop(delegate.get());
    }
    return ret;
}

FrameWriterInterface *FrameDispatcher::addDelegate(std::function<bool(const Frame::Ptr &frame)> cb) {
    return addDelegate(std::make_shared<FrameWriterInterfaceHelper>(std::move(cb)));
}

void FrameDispatcher::delDelegate(FrameWriterInterface *ptr) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    _delegates.erase(ptr);
    onSizeChange(_delegates.size());
}

int FrameDispatcher::flushGop(FrameWriterInterface* delegate) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    int ret = _gop_cache.size();
    for (auto frame : _gop_cache) {
        delegate->inputFrame(frame);
    }
    return ret;
}    

bool FrameDispatcher::inputFrame(const Frame::Ptr &frame) {
    bool ret = false;
    doStatistics(frame);
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (_enable_gop_cache) {
        bool is_key = frame->is_key;
        if (is_key && _video_key_frames && frame->getTrackType() == TrackAudio) {
            is_key = false;
        }
        if (is_key || _gop_cache.size() > 300) {
            _gop_cache.clear();
        }
        _gop_cache.push_back(frame);
    }
    for (auto &pr : _delegates) {
        if (pr.second->inputFrame(frame)) {
            ret = true;
        }
    }
    return ret;
}

float FrameDispatcher::getFps() const {
    float fps = 0.0f;
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (_gop_interval_ms) {
        fps = _gop_size * 1000.0f / _gop_interval_ms;
    }
    return fps;
}

void FrameDispatcher::doStatistics(const Frame::Ptr &frame) {
    _last_pts = frame->pts;
    ++_frames;
    if (frame->getTrackType() == TrackAudio) {
        aCodec = frame->codec;
    } else if (frame->getTrackType() == TrackVideo) {
        vCodec = frame->codec;
    }
    if (frame->is_key && frame->getTrackType() == TrackVideo) {
        // do statistics when got keyframes
        ++_video_key_frames;
        _gop_size = _frames - _last_frames;
        _gop_interval_ms = _ticker.elapsedTime();

        _last_frames = _frames;
        _ticker.resetTime();
    }
}
