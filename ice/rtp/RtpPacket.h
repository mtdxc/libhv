#ifndef SRC_ICE_RTP_PACKET_H_
#define SRC_ICE_RTP_PACKET_H_

#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include "hplatform.h"
#include "Frame.h"
#include "RtpExt.h"

namespace ice {

// RTP Fixed Header (RFC 3550 Section 5.1)
//
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V=2|P|X|  CC   |M|     PT      |       sequence number         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                           timestamp                           |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |           synchronization source (SSRC) identifier            |
// +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// |            contributing source (CSRC) identifiers             |
// |                             ....                              |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Extension header (if X bit set):
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |      defined by profile       |           length              |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                        header extension                       |
// |                             ....                              |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

#pragma pack(push, 1)

struct RtpHeader {
#if __BYTE_ORDER == __BIG_ENDIAN
    uint8_t version : 2;
    uint8_t padding : 1;
    uint8_t ext : 1;
    uint8_t csrc : 4;
    uint8_t mark : 1;
    uint8_t pt : 7;
#else
    uint8_t csrc : 4;
    uint8_t ext : 1;
    uint8_t padding : 1;
    uint8_t version : 2;
    uint8_t pt : 7;
    uint8_t mark : 1;
#endif
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;

    // Returns the fixed header size (12 bytes + 4*CSRC count)
    size_t getBaseSize() const { return 12 + csrc * 4; }

    // Returns extension data size in bytes (not including the 4-byte ext header)
    size_t getExtSize() const {
        if (!ext) return 0;
        uint8_t *ptr = (uint8_t *)this + getBaseSize();
        uint16_t len = ((uint16_t *)ptr)[1];
        len = ntohs(len);
        return len * 4;
    }

    // Returns the extension profile/reserved field
    uint16_t getExtReserved() const {
        if (!ext) return 0;
        uint8_t *ptr = (uint8_t *)this + getBaseSize();
        return ntohs(((uint16_t *)ptr)[0]);
    }

    // Returns pointer to extension data (past the 4-byte ext header)
    uint8_t *getExtData() {
        if (!ext) return nullptr;
        return (uint8_t *)this + getBaseSize() + 4;
    }

    const uint8_t *getExtData() const {
        if (!ext) return nullptr;
        return (const uint8_t *)this + getBaseSize() + 4;
    }

    // Returns pointer to the payload (after fixed header + CSRC + extension)
    uint8_t *getPayload() {
        size_t offset = getBaseSize();
        if (ext) {
            offset += 4 + getExtSize();
        }
        return (uint8_t *)this + offset;
    }

    const uint8_t *getPayload() const {
        size_t offset = getBaseSize();
        if (ext) {
            offset += 4 + getExtSize();
        }
        return (const uint8_t *)this + offset;
    }

    // Returns payload size given total packet size
    size_t getPayloadSize(size_t totalSize) const {
        size_t headerSize = getHeaderSize();
        if (totalSize <= headerSize) return 0;
        size_t payloadSize = totalSize - headerSize;
        // Subtract padding if present
        if (padding && totalSize > headerSize) {
            uint8_t padLen = ((const uint8_t *)this)[totalSize - 1];
            if (padLen <= payloadSize) {
                payloadSize -= padLen;
            }
        }
        return payloadSize;
    }

    // Full header size (base + extension header + extension data)
    size_t getHeaderSize() const {
        size_t size = getBaseSize();
        if (ext) {
            size += 4 + getExtSize();
        }
        return size;
    }

    // CSRC array access
    uint32_t *getCsrcArray() {
        return (uint32_t *)((uint8_t *)this + 12);
    }

    const uint32_t *getCsrcArray() const {
        return (const uint32_t *)((const uint8_t *)this + 12);
    }

    uint16_t getSeq() const { return ntohs(seq); }
    void setSeq(uint16_t s) { seq = htons(s); }
    uint32_t getTimestamp() const { return ntohl(timestamp); }
    void setTimestamp(uint32_t t) { timestamp = htonl(t); }
    uint32_t getSsrc() const { return ntohl(ssrc); }
    void setSsrc(uint32_t s) { ssrc = htonl(s); }
    
    int getCsrcCount() const { return csrc; }
    uint32_t getCsrc(int idx) const {
        if (idx >= csrc) return 0;
        return ntohl(getCsrcArray()[idx]);
    }
    void setCsrc(int idx, uint32_t val) {
        if (idx >= csrc) {
            csrc = idx + 1;
        }
        getCsrcArray()[idx] = htonl(val);
    }
};

#pragma pack(pop)

// RtpPacket wraps a raw RTP packet buffer and provides high-level access
class RtpPacket {
public:
    using Ptr = std::shared_ptr<RtpPacket>;

    RtpPacket() = default;
    ~RtpPacket() = default;

    // Parse from raw data (copies data)
    bool parse(const uint8_t *data, size_t size);

    // Create a new RTP packet with given parameters
    static RtpPacket::Ptr create(uint8_t pt, uint32_t ssrc, uint16_t seq,
                                 uint32_t timestamp, bool mark = false);

    // Create a new RTP packet with payload
    static RtpPacket::Ptr create(uint8_t pt, uint32_t ssrc, uint16_t seq,
                                 uint32_t timestamp, bool mark,
                                 const uint8_t *payload, size_t payloadSize);

    // Accessors
    RtpHeader *getHeader() { return reinterpret_cast<RtpHeader *>(buffer_.data()); }
    const RtpHeader *getHeader() const { return reinterpret_cast<const RtpHeader *>(buffer_.data()); }

    uint8_t getVersion() const { return getHeader()->version; }
    bool hasPadding() const { return getHeader()->padding; }
    bool hasExtension() const { return getHeader()->ext; }
    uint8_t getCsrcCount() const { return getHeader()->csrc; }
    bool getMarker() const { return getHeader()->mark; }
    uint8_t getPayloadType() const { return getHeader()->pt; }
    uint16_t getSeq() const { return getHeader()->getSeq(); }
    uint32_t getTimestamp() const { return getHeader()->getTimestamp(); }
    uint32_t getSSRC() const { return getHeader()->getSsrc(); }

    void setMarker(bool mark) { getHeader()->mark = mark ? 1 : 0; }
    void setPayloadType(uint8_t pt) { getHeader()->pt = pt; }
    void setSeq(uint16_t seq) { getHeader()->setSeq(seq); }
    void setTimestamp(uint32_t ts) { getHeader()->setTimestamp(ts); }
    void setSSRC(uint32_t ssrc) { getHeader()->setSsrc(ssrc); }

    // Payload access
    const uint8_t *getPayload() const { return getHeader()->getPayload(); }
    uint8_t *getPayload() { return getHeader()->getPayload(); }
    size_t getPayloadSize() const { return getHeader()->getPayloadSize(buffer_.size()); }
    size_t getHeaderSize() const { return getHeader()->getHeaderSize(); }

    // Raw buffer access
    const uint8_t *data() const { return buffer_.data(); }
    uint8_t *data() { return buffer_.data(); }
    size_t size() const { return buffer_.size(); }

    // Extension support
    void setExtension(uint16_t profile, const uint8_t *extData, size_t extLen);
    void removeExtension();

    // Padding support
    void setPadding(uint8_t padLen);
    void removePadding();

    // RTX support: wrap an RTP packet as RTX (RFC 4588)
    void RtxEncode(uint8_t payloadType, uint32_t ssrc, uint16_t seq);
    // Decode an RTX packet back into regular RTP payload.
    bool RtxDecode(uint8_t payloadType, uint32_t ssrc);

    // Extension map (populated by RtpExtContext::parseRtpExtId)
    std::map<uint8_t, RtpExt> extMap;
    std::string rid;

private:
    std::vector<uint8_t> buffer_;
};

} // namespace ice

#endif // SRC_ICE_RTP_PACKET_H_
