#ifndef SRC_ICE_RTCP_PACKET_H_
#define SRC_ICE_RTCP_PACKET_H_

#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <memory>
#include "hplatform.h"
#include "hv/Buffer.h"
namespace rtcp {
using BufferPtr = hv::BufferPtr;
// ============================================================================
// RTCP Packet Types (RFC 3550 Section 6)
// ============================================================================

enum class RtcpType : uint8_t {
    SR    = 200,  // Sender Report
    RR    = 201,  // Receiver Report
    SDES  = 202,  // Source Description
    BYE   = 203,  // Goodbye
    APP   = 204,  // Application-defined
    RTPFB = 205,  // Transport layer feedback (RFC 4585)
    PSFB  = 206,  // Payload-specific feedback (RFC 4585)
};

const char *rtcpTypeString(RtcpType type);

// ============================================================================
// RTCP Feedback Message Types (FMT)
// ============================================================================

enum class RtcpFbFmt : uint8_t {
    // Transport layer feedback (RTPFB = 205)
    NACK  = 1,    // Negative Acknowledgement (RFC 4585)
    TMMBR = 3,    // Temporary Maximum Media Bit Rate Request (RFC 5104)
    TMMBN = 4,    // Temporary Maximum Media Bit Rate Notification (RFC 5104)
    TWCC  = 15,   // Transport-Wide Congestion Control (draft-holmer)

    // Payload-specific feedback (PSFB = 206)
    PLI   = 1,    // Picture Loss Indication (RFC 4585)
    SLI   = 2,    // Slice Loss Indication (RFC 4585)
    RPSI  = 3,    // Reference Picture Selection Indication (RFC 4585)
    FIR   = 4,    // Full Intra Request (RFC 5104)
    REMB  = 15,   // Receiver Estimated Maximum Bitrate (draft-alvestrand, APP-like)
};

// ============================================================================
// Common RTCP Header (RFC 3550 Section 6.1)
// ============================================================================

#pragma pack(push, 1)

// Common RTCP fixed header (4 bytes)
//
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V=2|P|    RC   |   PT=SR=200   |             length            |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// For feedback messages:
// |V=2|P|   FMT   |   PT=205/206  |             length            |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
struct RtcpHeader {
#if __BYTE_ORDER == __BIG_ENDIAN
    uint8_t version : 2;
    uint8_t padding : 1;
    uint8_t rc : 5;     // reception report count / feedback message type (FMT)
#else
    uint8_t rc : 5;     // reception report count / feedback message type (FMT)
    uint8_t padding : 1;
    uint8_t version : 2;
#endif
    uint8_t pt;
    uint16_t length;    // in 32-bit words minus one (network byte order)

    // Packet size in bytes: (length + 1) * 4
    size_t getSize() const { return (ntohs(length) + 1) * 4; }

    RtcpType getType() const { return static_cast<RtcpType>(pt); }
    uint8_t getFMT() const { return rc; }
    uint8_t getRC() const { return rc; }

    // Set length from byte count (must be multiple of 4)
    void setLength(size_t bytes) {
        length = htons((uint16_t)(bytes / 4 - 1));
    }

    // Validate basic structure
    bool isValid() const {
        return version == 2;
    }
};

// ============================================================================
// Report Block (used in SR and RR)
// ============================================================================

// RFC 3550 Section 6.4.1
struct RtcpReportBlock {
    uint32_t ssrc;            // SSRC of source being reported
    uint8_t fractionLost;     // fraction lost (8.8 fixed point)
    uint8_t cumLost[3];       // cumulative number of packets lost (24 bits)
    uint32_t highestSeq;      // extended highest sequence number received
    uint32_t jitter;          // interarrival jitter
    uint32_t lsr;             // last SR timestamp (NTP middle 32 bits)
    uint32_t dlsr;            // delay since last SR (1/65536 seconds)

    // Helpers
    uint32_t getSSRC() const { return ntohl(ssrc); }
    uint32_t getCumLost() const {
        return ((uint32_t)cumLost[0] << 16) | ((uint32_t)cumLost[1] << 8) | cumLost[2];
    }
    uint32_t getHighestSeq() const { return ntohl(highestSeq); }
    uint32_t getJitter() const { return ntohl(jitter); }
    uint32_t getLSR() const { return ntohl(lsr); }
    uint32_t getDLSR() const { return ntohl(dlsr); }

    void setSSRC(uint32_t val) { ssrc = htonl(val); }
    void setCumLost(uint32_t val) {
        cumLost[0] = (val >> 16) & 0xFF;
        cumLost[1] = (val >> 8) & 0xFF;
        cumLost[2] = val & 0xFF;
    }
    void setHighestSeq(uint32_t val) { highestSeq = htonl(val); }
    void setJitter(uint32_t val) { jitter = htonl(val); }
    void setLSR(uint32_t val) { lsr = htonl(val); }
    void setDLSR(uint32_t val) { dlsr = htonl(val); }
};

// ============================================================================
// Sender Report (SR, PT=200)
// ============================================================================

// RFC 3550 Section 6.4.1
struct RtcpSR {
    RtcpHeader header;
    uint32_t ssrc;            // sender SSRC
    uint32_t ntpSec;          // NTP timestamp, most significant word
    uint32_t ntpFrac;         // NTP timestamp, least significant word
    uint32_t rtpTs;           // RTP timestamp
    uint32_t packetCount;     // sender's packet count
    uint32_t octetCount;      // sender's octet count
    // Followed by zero or more RtcpReportBlock

    uint32_t getSSRC() const { return ntohl(ssrc); }
    uint32_t getNtpSec() const { return ntohl(ntpSec); }
    uint32_t getNtpFrac() const { return ntohl(ntpFrac); }
    uint32_t getRtpTs() const { return ntohl(rtpTs); }
    uint32_t getPacketCount() const { return ntohl(packetCount); }
    uint32_t getOctetCount() const { return ntohl(octetCount); }

    void setSSRC(uint32_t val) { ssrc = htonl(val); }
    void setNtpTimestamp(uint32_t sec, uint32_t frac) {
        ntpSec = htonl(sec);
        ntpFrac = htonl(frac);
    }
    void setRtpTs(uint32_t val) { rtpTs = htonl(val); }
    void setPacketCount(uint32_t val) { packetCount = htonl(val); }
    void setOctetCount(uint32_t val) { octetCount = htonl(val); }

    // Get the first report block (after the SR header)
    RtcpReportBlock *getFirstReportBlock() {
        return reinterpret_cast<RtcpReportBlock *>(this + 1);
    }
    const RtcpReportBlock *getFirstReportBlock() const {
        return reinterpret_cast<const RtcpReportBlock *>(this + 1);
    }

    uint8_t getReportCount() const { return header.rc; }

    static constexpr size_t kMinSize = sizeof(RtcpHeader) + 24; // 28 bytes
};

// ============================================================================
// Receiver Report (RR, PT=201)
// ============================================================================

// RFC 3550 Section 6.4.2
struct RtcpRR {
    RtcpHeader header;
    uint32_t ssrc;            // receiver SSRC
    // Followed by one or more RtcpReportBlock

    uint32_t getSSRC() const { return ntohl(ssrc); }
    void setSSRC(uint32_t val) { ssrc = htonl(val); }

    RtcpReportBlock *getFirstReportBlock() {
        return reinterpret_cast<RtcpReportBlock *>(this + 1);
    }
    const RtcpReportBlock *getFirstReportBlock() const {
        return reinterpret_cast<const RtcpReportBlock *>(this + 1);
    }

    uint8_t getReportCount() const { return header.rc; }

    static constexpr size_t kMinSize = sizeof(RtcpHeader) + 4; // 8 bytes
};

// ============================================================================
// Source Description (SDES, PT=202)
// ============================================================================

enum class SdesType : uint8_t {
    END   = 0,
    CNAME = 1,
    NAME  = 2,
    EMAIL = 3,
    PHONE = 4,
    LOC   = 5,
    TOOL  = 6,
    NOTE  = 7,
};

struct RtcpSdesItem {
    SdesType type;
    uint8_t length;
    // followed by 'length' bytes of value
    const char *getValue() const {
        return reinterpret_cast<const char *>(this) + 2;
    }
    size_t getSize() const { return 2 + length; }
};

struct RtcpSdesChunk {
    uint32_t ssrc;
    // Followed by SDES items, terminated by END item

    uint32_t getSSRC() const { return ntohl(ssrc); }

    // Iterate over SDES items
    RtcpSdesItem *getFirstItem() {
        return reinterpret_cast<RtcpSdesItem *>(this + 1);
    }

    // Find CNAME value
    std::string getCNAME() const;
};

// ============================================================================
// Goodbye (BYE, PT=203)
// ============================================================================

// RFC 3550 Section 6.6
struct RtcpBYE {
    RtcpHeader header;
    // Followed by one or more SSRC/CSRC (uint32_t each)
    // Optionally followed by reason string (uint8_t length + UTF-8 text)

    uint32_t *getSSRCArray() {
        return reinterpret_cast<uint32_t *>(this + 1);
    }

    uint32_t getSSRC() const {
        if (header.rc > 0) {
            return ntohl(*reinterpret_cast<const uint32_t *>(this + 1));
        }
        return 0;
    }

    std::string getReason() const;
};

// ============================================================================
// Feedback Common Header (RTPFB/PSFB)
// ============================================================================

// RFC 4585 Section 6.1
// Common to NACK, PLI, FIR, REMB, TWCC etc.
struct RtcpFbHeader {
    RtcpHeader header;
    uint32_t senderSsrc;      // SSRC of sender of this feedback
    uint32_t mediaSsrc;       // SSRC of media source (unused for some types)

    uint32_t getSenderSsrc() const { return ntohl(senderSsrc); }
    uint32_t getMediaSsrc() const { return ntohl(mediaSsrc); }

    void setSenderSsrc(uint32_t val) { senderSsrc = htonl(val); }
    void setMediaSsrc(uint32_t val) { mediaSsrc = htonl(val); }
};

// ============================================================================
// NACK Feedback Item (RFC 4585 Section 6.2.1)
// ============================================================================

struct RtcpNackItem {
    uint16_t pid;             // Packet ID (sequence number)
    uint16_t blp;             // Bitmask of following lost packets

    uint16_t getPid() const { return ntohs(pid); }
    uint16_t getBlp() const { return ntohs(blp); }

    void setPid(uint16_t val) { pid = htons(val); }
    void setBlp(uint16_t val) { blp = htons(val); }

    // Get all lost sequence numbers (PID + bits set in BLP)
    std::vector<uint16_t> getLostSeqs() const;
};

// ============================================================================
// PLI (Picture Loss Indication, PSFB FMT=1)
// ============================================================================

// RFC 4585 Section 6.3.1
// PLI has no additional fields beyond RtcpFbHeader
// The FCI section is empty (mediaSsrc field is unused for PLI)
struct RtcpPLI {
    RtcpFbHeader fbHeader;

    static RtcpPLI *create(std::vector<uint8_t> &buf, uint32_t senderSsrc, uint32_t mediaSsrc);
    static constexpr size_t kSize = sizeof(RtcpFbHeader); // 12 bytes
};

// ============================================================================
// FIR (Full Intra Request, PSFB FMT=4)
// ============================================================================

// RFC 5104 Section 4.3.1
struct RtcpFirItem {
    uint32_t ssrc;
    uint8_t seqNum;
    uint8_t reserved[3];

    uint32_t getSSRC() const { return ntohl(ssrc); }
    uint8_t getSeqNum() const { return seqNum; }
    void setSSRC(uint32_t val) { ssrc = htonl(val); }
    void setSeqNum(uint8_t val) { seqNum = val; }
};

// ============================================================================
// REMB (Receiver Estimated Maximum Bitrate)
// ============================================================================

// draft-alvestrand-rmcat-remb-03
// This is a PSFB with FMT=15 (AFB - Application Layer Feedback)
// with a REMB-specific payload:
//
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |  Unique identifier 'R' 'E' 'M' 'B'                            |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |  Num SSRC     | BR Exp    |  BR Mantissa (bit 0..17)         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |   SSRC feedback                                               |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

struct RtcpRembItem {
    uint32_t ssrc;
    uint64_t bitrate;     // in bits per second
};

struct RtcpREMB {
    RtcpFbHeader fbHeader;
    uint32_t rembId;      // 'R' 'E' 'M' 'B' = 0x52454D42
    uint8_t numSsrc;
    uint8_t brExpAndMantissa[3]; // brExp (6 bits) + brMantissa (18 bits)
    // Followed by numSsrc SSRC values

    bool isValid() const {
        return ntohl(rembId) == 0x52454D42;
    }

    uint8_t getNumSsrc() const { return numSsrc; }

    uint64_t getBitrate() const {
        uint8_t brExp = (brExpAndMantissa[0] >> 2) & 0x3F;
        uint32_t brMantissa = ((uint32_t)(brExpAndMantissa[0] & 0x03) << 16) |
                              ((uint32_t)brExpAndMantissa[1] << 8) |
                              brExpAndMantissa[2];
        return (uint64_t)brMantissa << brExp;
    }

    void setBitrate(uint64_t bitrate) {
        // Find the best exp/mantissa representation
        uint8_t exp = 0;
        uint32_t mantissa = 0;
        for (uint8_t e = 0; e < 64; ++e) {
            uint64_t m = bitrate >> e;
            if (m <= 0x3FFFF) {
                exp = e;
                mantissa = (uint32_t)m;
                break;
            }
        }
        brExpAndMantissa[0] = (exp << 2) | ((mantissa >> 16) & 0x03);
        brExpAndMantissa[1] = (mantissa >> 8) & 0xFF;
        brExpAndMantissa[2] = mantissa & 0xFF;
    }

    uint32_t *getSsrcArray() {
        return reinterpret_cast<uint32_t *>(&numSsrc + 1 + 3);
    }

    static constexpr uint32_t kRembId = 0x52454D42; // "REMB"
};

// ============================================================================
// TWCC (Transport-Wide Congestion Control) Feedback
// ============================================================================

// draft-holmer-rmcat-transport-wide-cc-extensions-01
// This is a RTPFB with FMT=15

struct RtcpTWCCHeader {
    RtcpFbHeader fbHeader;
    uint16_t baseSeq;
    uint16_t packetStatusCount;
    uint8_t refTime[3];        // 24-bit reference time (64ms units)
    uint8_t fbPktCount;

    uint16_t getBaseSeq() const { return ntohs(baseSeq); }
    uint16_t getPacketStatusCount() const { return ntohs(packetStatusCount); }
    uint32_t getRefTime() const {
        return ((uint32_t)refTime[0] << 16) | ((uint32_t)refTime[1] << 8) | refTime[2];
    }
    uint8_t getFbPktCount() const { return fbPktCount; }

    void setBaseSeq(uint16_t val) { baseSeq = htons(val); }
    void setPacketStatusCount(uint16_t val) { packetStatusCount = htons(val); }
    void setRefTime(uint32_t val) {
        refTime[0] = (val >> 16) & 0xFF;
        refTime[1] = (val >> 8) & 0xFF;
        refTime[2] = val & 0xFF;
    }
    void setFbPktCount(uint8_t val) { fbPktCount = val; }
};

#pragma pack(pop)

// Packet status symbol sizes
enum class TwccSymbolSize : uint8_t {
    OneBit = 0,
    TwoBit = 1,
};

// Packet received status
enum class TwccPacketStatus : uint8_t {
    NotReceived = 0,  // (2-bit: 00) not received
    ReceivedSmallDelta = 1,  // (2-bit: 01) received, small delta (1 byte)
    ReceivedLargeDelta = 2,  // (2-bit: 10) received, large or negative delta (2 bytes)
    Reserved = 3,
};

// ============================================================================
// RtcpPacket: High-level RTCP compound packet parser
// ============================================================================

class RtcpPacket {
public:
    using Ptr = std::shared_ptr<RtcpPacket>;

    // Parse a compound RTCP packet (may contain multiple RTCP packets)
    // Returns list of individual RTCP packet pointers (point into the data)
    static std::vector<const RtcpHeader *> parse(const uint8_t *data, size_t size);

    // Get a human-readable description
    static std::string describe(const RtcpHeader *header);

    // ========================================================================
    // Factory methods for creating RTCP packets
    // ========================================================================

    // Create a Sender Report (SR)
    static BufferPtr createSR(uint32_t ssrc, uint32_t ntpSec, uint32_t ntpFrac,
                                          uint32_t rtpTs, uint32_t pktCount, uint32_t octetCount,
                                          const std::vector<RtcpReportBlock> &reports = {});

    // Create a Receiver Report (RR)
    static BufferPtr createRR(uint32_t ssrc,
                                          const std::vector<RtcpReportBlock> &reports);

    // Create a SDES packet with CNAME
    static BufferPtr createSDES(uint32_t ssrc, const std::string &cname);

    // Create a BYE packet
    static BufferPtr createBYE(const std::vector<uint32_t> &ssrcs,
                                           const std::string &reason = "");

    // Create a NACK feedback packet
    static BufferPtr createNACK(uint32_t senderSsrc, uint32_t mediaSsrc,
                                            const std::vector<uint16_t> &lostSeqs);

    // Create a PLI (Picture Loss Indication)
    static BufferPtr createPLI(uint32_t senderSsrc, uint32_t mediaSsrc);

    // Create a FIR (Full Intra Request)
    static BufferPtr createFIR(uint32_t senderSsrc, uint32_t mediaSsrc,
                                           uint8_t seqNum);

    // Create a REMB (Receiver Estimated Maximum Bitrate)
    static BufferPtr createREMB(uint32_t senderSsrc, uint64_t bitrate,
                                            const std::vector<uint32_t> &ssrcs);

private:
    // Helper to build NACK items from a list of sequence numbers
    static std::vector<RtcpNackItem> buildNackItems(const std::vector<uint16_t> &lostSeqs);
};

} // namespace ice

#endif // SRC_ICE_RTCP_PACKET_H_
