#include "RtpPacket.h"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <cinttypes>
namespace ice {

// ============================================================================
// RtpPacket
// ============================================================================

bool RtpPacket::parse(const uint8_t *data, size_t size) {
    if (size < 12) {
        // Minimum RTP header is 12 bytes
        return false;
    }

    const RtpHeader *hdr = reinterpret_cast<const RtpHeader *>(data);
    if (hdr->version != 2) {
        return false;
    }

    // Validate header size
    size_t baseSize = hdr->getBaseSize();
    if (size < baseSize) {
        return false;
    }

    size_t headerSize = hdr->getHeaderSize();
    if (size < headerSize) {
        return false;
    }

    // Validate padding
    if (hdr->padding) {
        uint8_t padLen = data[size - 1];
        if (padLen == 0 || headerSize + padLen > size) {
            return false;
        }
    }

    // Copy data into internal buffer
    buffer_.assign(data, data + size);
    extMap.clear();
    rid.clear();
    return true;
}

RtpPacket::Ptr RtpPacket::create(CodecId codec, uint8_t pt, uint32_t ssrc, uint16_t seq,
                                  uint64_t timestamp, bool mark) {
    return create(codec, pt, ssrc, seq, timestamp, mark, nullptr, 0);
}

RtpPacket::Ptr RtpPacket::create(CodecId codec, uint8_t pt, uint32_t ssrc, uint16_t seq,
                                  uint64_t timestamp, bool mark,
                                  const uint8_t *payload, size_t payloadSize) {
    auto pkt = std::make_shared<RtpPacket>();
    pkt->codec = codec;
    pkt->ntp_stamp = timestamp;
    pkt->sample_rate = RtpPayload::getClockRateByCodec(codec);
    uint32_t rtp_stamp = timestamp * pkt->sample_rate / 1000;

    size_t totalSize = 12 + payloadSize;
    pkt->buffer_.resize(totalSize);
    
    RtpHeader *hdr = reinterpret_cast<RtpHeader *>(pkt->buffer_.data());
    memset(hdr, 0, 12);
    hdr->version = 2;
    hdr->padding = 0;
    hdr->ext = 0;
    hdr->csrc = 0;
    hdr->mark = mark ? 1 : 0;
    hdr->pt = pt;
    hdr->setSeq(seq);
    hdr->setTimestamp(rtp_stamp);
    hdr->setSsrc(ssrc);

    if (payload && payloadSize > 0) {
        memcpy(pkt->buffer_.data() + 12, payload, payloadSize);
    }

    return pkt;
}

void RtpPacket::setExtension(uint16_t profile, const uint8_t *extData, size_t extLen) {
    if (!buffer_.empty()) {
        RtpHeader *hdr = getHeader();
        size_t baseSize = hdr->getBaseSize();
        size_t oldHeaderSize = hdr->getHeaderSize();
        size_t payloadSize = buffer_.size() - oldHeaderSize;

        // Extension data must be multiple of 4 bytes
        size_t paddedExtLen = extLen;
        if (paddedExtLen % 4 != 0) {
            paddedExtLen += 4 - (paddedExtLen % 4);
        }

        size_t newHeaderSize = baseSize + 4 + paddedExtLen;
        size_t newTotalSize = newHeaderSize + payloadSize;

        std::vector<uint8_t> newBuf(newTotalSize);
        // Copy base header
        memcpy(newBuf.data(), buffer_.data(), baseSize);
        // Copy payload to new position
        memcpy(newBuf.data() + newHeaderSize, buffer_.data() + oldHeaderSize, payloadSize);

        // Set extension header
        uint8_t *extHeader = newBuf.data() + baseSize;
        uint16_t netProfile = htons(profile);
        uint16_t netLen = htons((uint16_t)(paddedExtLen / 4));
        memcpy(extHeader, &netProfile, 2);
        memcpy(extHeader + 2, &netLen, 2);

        // Copy extension data
        if (extData && extLen > 0) {
            memcpy(extHeader + 4, extData, extLen);
            // Zero padding
            if (paddedExtLen > extLen) {
                memset(extHeader + 4 + extLen, 0, paddedExtLen - extLen);
            }
        }

        buffer_ = std::move(newBuf);
        getHeader()->ext = 1;
    }
}

void RtpPacket::removeExtension() {
    if (!buffer_.empty()) {
        RtpHeader *hdr = getHeader();
        if (hdr->ext) {
            size_t baseSize = hdr->getBaseSize();
            size_t oldHeaderSize = hdr->getHeaderSize();
            size_t payloadSize = buffer_.size() - oldHeaderSize;

            std::vector<uint8_t> newBuf(baseSize + payloadSize);
            memcpy(newBuf.data(), buffer_.data(), baseSize);
            memcpy(newBuf.data() + baseSize, buffer_.data() + oldHeaderSize, payloadSize);

            buffer_ = std::move(newBuf);
            getHeader()->ext = 0;
        }
    }
}

void RtpPacket::setPadding(uint8_t padLen) {
    if (!buffer_.empty() && padLen > 0) {
        RtpHeader *hdr = getHeader();
        size_t headerSize = hdr->getHeaderSize();
        size_t payloadSize = buffer_.size() - headerSize;

        // Remove existing padding if any
        if (hdr->padding) {
            uint8_t oldPadLen = buffer_[buffer_.size() - 1];
            payloadSize -= oldPadLen;
        }

        size_t newSize = headerSize + payloadSize + padLen;
        buffer_.resize(newSize);
        // Set all padding bytes except last to 0, last to padLen
        memset(buffer_.data() + headerSize + payloadSize, 0, padLen - 1);
        buffer_[newSize - 1] = padLen;
        getHeader()->padding = 1;
    }
}

void RtpPacket::removePadding() {
    if (!buffer_.empty()) {
        RtpHeader *hdr = getHeader();
        if (hdr->padding) {
            hdr->padding = 0;
            uint8_t padLen = buffer_[buffer_.size() - 1];
            buffer_.resize(buffer_.size() - padLen);
        }
    }
}

void RtpPacket::RtxEncode(uint8_t payloadType, uint32_t ssrc, uint16_t seq) {
    if (buffer_.size() < 12) {
        return;
    }

    // RTX payload prepends original RTP sequence number.
    if (getHeader()->padding) {
        removePadding();
    }

    size_t size = buffer_.size();
    buffer_.resize(size + 2);
    RtpHeader* hdr = getHeader();
    size_t payloadSize = hdr->getPayloadSize(size);
    uint8_t* payload = hdr->getPayload();

    std::memmove(payload + 2, payload, payloadSize);
    std::memcpy(payload, &hdr->seq, 2);

    hdr->pt = payloadType;
    hdr->setSsrc(ssrc);
    hdr->setSeq(seq);
}

bool RtpPacket::RtxDecode(uint8_t payloadType, uint32_t ssrc) {
    if (buffer_.size() < 12) {
        return false;
    }

    RtpHeader* hdr = getHeader();
    size_t payloadSize = hdr->getPayloadSize(buffer_.size());

    // Some senders may emit empty RTX packets at stream startup.
    if (payloadSize < 2) {
        return false;
    }

    uint8_t* payload = hdr->getPayload();
    std::memcpy(&hdr->seq, payload, 2);
    std::memmove(payload, payload + 2, payloadSize - 2);

    hdr->pt = payloadType;
    hdr->setSsrc(ssrc);

    if (hdr->padding) {
        hdr->padding = 0;
        uint8_t padLen = buffer_[buffer_.size() - 1];
        buffer_.resize(buffer_.size() - 2 - padLen);
    } else {
        buffer_.resize(buffer_.size() - 2);
    }
    return true;
}

std::string RtpPacket::toString() const {
    auto hdr = getHeader();
    char line[256];
    snprintf(line, sizeof(line), 
      "ssrc=%" PRIu32 ", pt=%d, seq=%" PRIu16 ", stamp=%" PRIu32 ", size=%d,%d", 
        hdr->getSsrc(), (int)hdr->pt, hdr->getSeq(), hdr->getTimestamp(), (int)size(), (int)hdr->mark);
    return line;
}

} // namespace ice
