#include "RtcpPacket.h"
#include <cstring>
#include <sstream>
#include <algorithm>
#include <stdexcept>

namespace rtcp {

// ============================================================================
// Utility
// ============================================================================

const char *rtcpTypeString(RtcpType type) {
    switch (type) {
        case RtcpType::SR:    return "SR";
        case RtcpType::RR:    return "RR";
        case RtcpType::SDES:  return "SDES";
        case RtcpType::BYE:   return "BYE";
        case RtcpType::APP:   return "APP";
        case RtcpType::RTPFB: return "RTPFB";
        case RtcpType::PSFB:  return "PSFB";
        default:              return "Unknown";
    }
}

// ============================================================================
// RtcpNackItem
// ============================================================================

std::vector<uint16_t> RtcpNackItem::getLostSeqs() const {
    std::vector<uint16_t> seqs;
    uint16_t pid = getPid();
    uint16_t blp = getBlp();
    seqs.push_back(pid);
    for (int i = 0; i < 16; ++i) {
        if (blp & (1u << i)) {
            seqs.push_back(pid + i + 1);
        }
    }
    return seqs;
}

// ============================================================================
// RtcpSdesChunk
// ============================================================================

std::string RtcpSdesChunk::getCNAME() const {
    const RtcpSdesItem *item = reinterpret_cast<const RtcpSdesItem *>(
        reinterpret_cast<const uint8_t *>(this) + 4);
    const uint8_t *end = reinterpret_cast<const uint8_t *>(this) + 1024; // safety limit
    while (reinterpret_cast<const uint8_t *>(item) < end) {
        if (item->type == SdesType::END) break;
        if (item->type == SdesType::CNAME) {
            return std::string(item->getValue(), item->length);
        }
        // Move to next item
        size_t itemSize = 2 + item->length;
        // Align to 4-byte boundary (item size + padding)
        item = reinterpret_cast<const RtcpSdesItem *>(
            reinterpret_cast<const uint8_t *>(item) + itemSize);
    }
    return "";
}

// ============================================================================
// RtcpBYE
// ============================================================================

std::string RtcpBYE::getReason() const {
    size_t ssrcBytes = header.rc * 4;
    const uint8_t *reasonPtr = reinterpret_cast<const uint8_t *>(this) +
                                sizeof(RtcpHeader) + ssrcBytes;
    size_t packetSize = header.getSize();
    size_t reasonOffset = sizeof(RtcpHeader) + ssrcBytes;
    if (reasonOffset + 1 > packetSize) return "";
    uint8_t reasonLen = *reasonPtr;
    if (reasonOffset + 1 + reasonLen > packetSize) return "";
    return std::string(reinterpret_cast<const char *>(reasonPtr + 1), reasonLen);
}

// ============================================================================
// RtcpPacket::parse - Parse compound RTCP packet
// ============================================================================

std::vector<const RtcpHeader *> RtcpPacket::parse(const uint8_t *data, size_t size) {
    std::vector<const RtcpHeader *> packets;
    size_t offset = 0;
    while (offset + sizeof(RtcpHeader) <= size) {
        const RtcpHeader *hdr = reinterpret_cast<const RtcpHeader *>(data + offset);
        if (!hdr->isValid()) break;
        size_t pktSize = hdr->getSize();
        if (offset + pktSize > size) break;
        packets.push_back(hdr);
        offset += pktSize;
    }
    return packets;
}

// ============================================================================
// RtcpPacket::describe - Human-readable description
// ============================================================================

std::string RtcpPacket::describe(const RtcpHeader *header) {
    std::ostringstream ss;
    RtcpType type = header->getType();
    ss << rtcpTypeString(type);

    switch (type) {
        case RtcpType::SR: {
            if (header->getSize() >= RtcpSR::kMinSize) {
                const RtcpSR *sr = reinterpret_cast<const RtcpSR *>(header);
                ss << " ssrc=" << sr->getSSRC()
                   << " ntp=" << sr->getNtpSec() << "." << sr->getNtpFrac()
                   << " rtpTs=" << sr->getRtpTs()
                   << " pkts=" << sr->getPacketCount()
                   << " bytes=" << sr->getOctetCount()
                   << " rc=" << (int)sr->getReportCount();
            }
            break;
        }
        case RtcpType::RR: {
            if (header->getSize() >= RtcpRR::kMinSize) {
                const RtcpRR *rr = reinterpret_cast<const RtcpRR *>(header);
                ss << " ssrc=" << rr->getSSRC()
                   << " rc=" << (int)rr->getReportCount();
            }
            break;
        }
        case RtcpType::SDES: {
            ss << " sc=" << (int)header->rc;
            break;
        }
        case RtcpType::BYE: {
            ss << " sc=" << (int)header->rc;
            if (header->getSize() > sizeof(RtcpHeader)) {
                const RtcpBYE *bye = reinterpret_cast<const RtcpBYE *>(header);
                std::string reason = bye->getReason();
                if (!reason.empty()) {
                    ss << " reason=\"" << reason << "\"";
                }
            }
            break;
        }
        case RtcpType::RTPFB: {
            uint8_t fmt = header->getFMT();
            ss << " fmt=" << (int)fmt;
            if (header->getSize() >= sizeof(RtcpFbHeader)) {
                const RtcpFbHeader *fb = reinterpret_cast<const RtcpFbHeader *>(header);
                ss << " sender=" << fb->getSenderSsrc()
                   << " media=" << fb->getMediaSsrc();
            }
            if (fmt == (uint8_t)RtcpFbFmt::NACK) {
                ss << " (NACK)";
            } else if (fmt == (uint8_t)RtcpFbFmt::TWCC) {
                ss << " (TWCC)";
            }
            break;
        }
        case RtcpType::PSFB: {
            uint8_t fmt = header->getFMT();
            ss << " fmt=" << (int)fmt;
            if (header->getSize() >= sizeof(RtcpFbHeader)) {
                const RtcpFbHeader *fb = reinterpret_cast<const RtcpFbHeader *>(header);
                ss << " sender=" << fb->getSenderSsrc()
                   << " media=" << fb->getMediaSsrc();
            }
            if (fmt == (uint8_t)RtcpFbFmt::PLI) {
                ss << " (PLI)";
            } else if (fmt == (uint8_t)RtcpFbFmt::FIR) {
                ss << " (FIR)";
            } else if (fmt == (uint8_t)RtcpFbFmt::REMB) {
                ss << " (REMB)";
            }
            break;
        }
        default:
            break;
    }
    ss << " size=" << header->getSize();
    return ss.str();
}

// ============================================================================
// Factory: Sender Report (SR)
// ============================================================================

BufferPtr RtcpPacket::createSR(uint32_t ssrc, uint32_t ntpSec, uint32_t ntpFrac,
                                            uint32_t rtpTs, uint32_t pktCount, uint32_t octetCount,
                                            const std::vector<RtcpReportBlock> &reports) {
    size_t reportCount = std::min(reports.size(), (size_t)31);
    size_t totalSize = sizeof(RtcpHeader) + 24 + reportCount * sizeof(RtcpReportBlock);
    BufferPtr buf = std::make_shared<hv::Buffer>(totalSize);

    RtcpSR *sr = reinterpret_cast<RtcpSR *>(buf->data());
    sr->header.version = 2;
    sr->header.padding = 0;
    sr->header.rc = (uint8_t)reportCount;
    sr->header.pt = (uint8_t)RtcpType::SR;
    sr->header.setLength(totalSize);

    sr->setSSRC(ssrc);
    sr->setNtpTimestamp(ntpSec, ntpFrac);
    sr->setRtpTs(rtpTs);
    sr->setPacketCount(pktCount);
    sr->setOctetCount(octetCount);

    for (size_t i = 0; i < reportCount; ++i) {
        RtcpReportBlock *rb = sr->getFirstReportBlock() + i;
        *rb = reports[i];
    }

    return buf;
}

// ============================================================================
// Factory: Receiver Report (RR)
// ============================================================================

BufferPtr RtcpPacket::createRR(uint32_t ssrc,
                                            const std::vector<RtcpReportBlock> &reports) {
    size_t reportCount = std::min(reports.size(), (size_t)31);
    size_t totalSize = sizeof(RtcpHeader) + 4 + reportCount * sizeof(RtcpReportBlock);
    BufferPtr buf = std::make_shared<hv::Buffer>(totalSize);

    RtcpRR *rr = reinterpret_cast<RtcpRR *>(buf->data());
    rr->header.version = 2;
    rr->header.padding = 0;
    rr->header.rc = (uint8_t)reportCount;
    rr->header.pt = (uint8_t)RtcpType::RR;
    rr->header.setLength(totalSize);

    rr->setSSRC(ssrc);

    for (size_t i = 0; i < reportCount; ++i) {
        RtcpReportBlock *rb = rr->getFirstReportBlock() + i;
        *rb = reports[i];
    }

    return buf;
}

// ============================================================================
// Factory: SDES with CNAME
// ============================================================================

BufferPtr RtcpPacket::createSDES(uint32_t ssrc, const std::string &cname) {
    // Chunk: 4 bytes SSRC + SDES items (CNAME + END) + padding to 4-byte boundary
    size_t cnameItemSize = 2 + cname.size(); // type(1) + length(1) + value
    size_t endItemSize = 1;                   // END item (type=0)
    size_t chunkPayload = cnameItemSize + endItemSize;
    size_t chunkSize = 4 + chunkPayload;
    // Pad chunk to 4-byte boundary
    size_t paddedChunkSize = (chunkSize + 3) & ~3;

    size_t totalSize = sizeof(RtcpHeader) + paddedChunkSize;
    BufferPtr buf = std::make_shared<hv::Buffer>(totalSize);

    RtcpHeader *hdr = reinterpret_cast<RtcpHeader *>(buf->data());
    hdr->version = 2;
    hdr->padding = 0;
    hdr->rc = 1; // one chunk
    hdr->pt = (uint8_t)RtcpType::SDES;
    hdr->setLength(totalSize);

    // Write chunk
    uint8_t *chunk = (uint8_t *)buf->data() + sizeof(RtcpHeader);
    uint32_t netSsrc = htonl(ssrc);
    memcpy(chunk, &netSsrc, 4);

    // CNAME item
    uint8_t *item = chunk + 4;
    item[0] = (uint8_t)SdesType::CNAME;
    item[1] = (uint8_t)cname.size();
    memcpy(item + 2, cname.data(), cname.size());

    // END item
    item[2 + cname.size()] = (uint8_t)SdesType::END;

    return buf;
}

// ============================================================================
// Factory: BYE
// ============================================================================

BufferPtr RtcpPacket::createBYE(const std::vector<uint32_t> &ssrcs,
                                             const std::string &reason) {
    size_t ssrcCount = std::min(ssrcs.size(), (size_t)31);
    size_t reasonSize = 0;
    if (!reason.empty()) {
        reasonSize = 1 + reason.size(); // length byte + string
        // Pad to 4-byte boundary
        reasonSize = (reasonSize + 3) & ~3;
    }

    size_t totalSize = sizeof(RtcpHeader) + ssrcCount * 4 + reasonSize;
    BufferPtr buf = std::make_shared<hv::Buffer>(totalSize);

    RtcpHeader *hdr = reinterpret_cast<RtcpHeader *>(buf->data());
    hdr->version = 2;
    hdr->padding = 0;
    hdr->rc = (uint8_t)ssrcCount;
    hdr->pt = (uint8_t)RtcpType::BYE;
    hdr->setLength(totalSize);

    uint8_t *ptr = (uint8_t *)buf->data() + sizeof(RtcpHeader);
    for (size_t i = 0; i < ssrcCount; ++i) {
        uint32_t netSsrc = htonl(ssrcs[i]);
        memcpy(ptr, &netSsrc, 4);
        ptr += 4;
    }

    if (!reason.empty()) {
        *ptr = (uint8_t)reason.size();
        memcpy(ptr + 1, reason.data(), reason.size());
    }

    return buf;
}

// ============================================================================
// Factory: NACK
// ============================================================================

std::vector<RtcpNackItem> RtcpPacket::buildNackItems(const std::vector<uint16_t> &lostSeqs) {
    if (lostSeqs.empty()) return {};

    std::vector<uint16_t> sorted = lostSeqs;
    std::sort(sorted.begin(), sorted.end());

    std::vector<RtcpNackItem> items;
    size_t i = 0;
    while (i < sorted.size()) {
        RtcpNackItem item;
        item.setPid(sorted[i]);
        item.setBlp(0);

        uint16_t pid = sorted[i];
        ++i;
        while (i < sorted.size()) {
            uint16_t diff = sorted[i] - pid;
            if (diff >= 1 && diff <= 16) {
                uint16_t blp = item.getBlp();
                blp |= (1u << (diff - 1));
                item.setBlp(blp);
                ++i;
            } else {
                break;
            }
        }
        items.push_back(item);
    }
    return items;
}

BufferPtr RtcpPacket::createNACK(uint32_t senderSsrc, uint32_t mediaSsrc,
                                              const std::vector<uint16_t> &lostSeqs) {
    auto items = buildNackItems(lostSeqs);
    size_t fciSize = items.size() * sizeof(RtcpNackItem);
    size_t totalSize = sizeof(RtcpFbHeader) + fciSize;
    BufferPtr buf = std::make_shared<hv::Buffer>(totalSize);

    RtcpFbHeader *fb = reinterpret_cast<RtcpFbHeader *>(buf->data());
    fb->header.version = 2;
    fb->header.padding = 0;
    fb->header.rc = (uint8_t)RtcpFbFmt::NACK;
    fb->header.pt = (uint8_t)RtcpType::RTPFB;
    fb->header.setLength(totalSize);
    fb->setSenderSsrc(senderSsrc);
    fb->setMediaSsrc(mediaSsrc);

    if (!items.empty()) {
        memcpy((char *)buf->data() + sizeof(RtcpFbHeader), items.data(), fciSize);
    }

    return buf;
}

// ============================================================================
// Factory: PLI (Picture Loss Indication)
// ============================================================================

BufferPtr RtcpPacket::createPLI(uint32_t senderSsrc, uint32_t mediaSsrc) {
    size_t totalSize = sizeof(RtcpFbHeader);
    BufferPtr buf = std::make_shared<hv::Buffer>(totalSize);

    RtcpFbHeader *fb = reinterpret_cast<RtcpFbHeader *>(buf->data());
    fb->header.version = 2;
    fb->header.padding = 0;
    fb->header.rc = (uint8_t)RtcpFbFmt::PLI;
    fb->header.pt = (uint8_t)RtcpType::PSFB;
    fb->header.setLength(totalSize);
    fb->setSenderSsrc(senderSsrc);
    fb->setMediaSsrc(mediaSsrc);

    return buf;
}

// ============================================================================
// Factory: FIR (Full Intra Request)
// ============================================================================

BufferPtr RtcpPacket::createFIR(uint32_t senderSsrc, uint32_t mediaSsrc,
                                             uint8_t seqNum) {
    size_t totalSize = sizeof(RtcpFbHeader) + 8; // FCI: 8 bytes per FIR entry
    BufferPtr buf = std::make_shared<hv::Buffer>(totalSize);

    RtcpFbHeader *fb = reinterpret_cast<RtcpFbHeader *>(buf->data());
    fb->header.version = 2;
    fb->header.padding = 0;
    fb->header.rc = (uint8_t)RtcpFbFmt::FIR;
    fb->header.pt = (uint8_t)RtcpType::PSFB;
    fb->header.setLength(totalSize);
    fb->setSenderSsrc(senderSsrc);
    fb->setMediaSsrc(0); // unused for FIR

    // FIR FCI entry
    RtcpFirItem *firItem = reinterpret_cast<RtcpFirItem *>((char*)buf->data() + sizeof(RtcpFbHeader));
    firItem->setSSRC(mediaSsrc);
    firItem->setSeqNum(seqNum);

    return buf;
}

// ============================================================================
// Factory: REMB (Receiver Estimated Maximum Bitrate)
// ============================================================================

BufferPtr RtcpPacket::createREMB(uint32_t senderSsrc, uint64_t bitrate,
                                              const std::vector<uint32_t> &ssrcs) {
    size_t numSsrc = std::min(ssrcs.size(), (size_t)255);
    size_t totalSize = sizeof(RtcpFbHeader) + 8 + numSsrc * 4; // REMB ID + exp/mantissa + SSRCs
    BufferPtr buf = std::make_shared<hv::Buffer>(totalSize);

    RtcpFbHeader *fb = reinterpret_cast<RtcpFbHeader *>(buf->data());
    fb->header.version = 2;
    fb->header.padding = 0;
    fb->header.rc = (uint8_t)RtcpFbFmt::REMB; // FMT=15 (AFB)
    fb->header.pt = (uint8_t)RtcpType::PSFB;
    fb->header.setLength(totalSize);
    fb->setSenderSsrc(senderSsrc);
    fb->setMediaSsrc(0); // unused for REMB

    // Write REMB identifier
    uint8_t *rembData = (uint8_t *)buf->data() + sizeof(RtcpFbHeader);
    uint32_t rembId = htonl(RtcpREMB::kRembId);
    memcpy(rembData, &rembId, 4);

    // Write num SSRC + bitrate
    uint8_t *brData = rembData + 4;
    brData[0] = (uint8_t)numSsrc;

    // Set bitrate using exp/mantissa encoding
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
    brData[1] = (exp << 2) | ((mantissa >> 16) & 0x03);
    brData[2] = (mantissa >> 8) & 0xFF;
    brData[3] = mantissa & 0xFF;

    // Write SSRC list
    uint32_t *ssrcPtr = reinterpret_cast<uint32_t *>(brData + 4);
    for (size_t i = 0; i < numSsrc; ++i) {
        ssrcPtr[i] = htonl(ssrcs[i]);
    }

    return buf;
}

} // namespace ice
