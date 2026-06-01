#include "stun_message.h"
#include "stun_auth.h"

#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace ice {

TransactionId stun_generate_transaction_id() {
    TransactionId id;
    static std::once_flag init_flag;
    static uint64_t process_seed = 0;
    std::call_once(init_flag, []() {
        std::random_device rd;
        uint64_t seed = 0;
        seed ^= (uint64_t)rd();
        seed ^= (uint64_t)rd() << 16;
        seed ^= (uint64_t)rd() << 32;
        seed ^= (uint64_t)rd() << 48;
        process_seed = seed;
    });

    static std::atomic<uint64_t> sequence{0};
    uint64_t counter = sequence.fetch_add(1, std::memory_order_relaxed);

    uint32_t hi = (uint32_t)(process_seed >> 32) ^ (uint32_t)counter;
    uint32_t lo = (uint32_t)process_seed ^ (uint32_t)(counter >> 32);
    uint32_t mix = hi ^ (lo << 1) ^ 0xA5A5A5A5u;

    write_be32(id.data(), hi);
    write_be32(id.data() + 4, lo);
    write_be32(id.data() + 8, mix);
    return id;
}

std::string TransactionIdStr(const TransactionId& id) {
    std::string ret;
    char hex[3];
    for (auto& b : id) {
        snprintf(hex, 3, "%02X", b);
        ret += hex;
    }
    return ret;
}

size_t TurnTcpLength(const uint8_t* view, size_t len, int* pad_bytes) {
    *pad_bytes = 0;
    uint16_t pkt_len = read_be16(view + 2);
    size_t expected_pkt_len;
    if ((view[0] & 0xC0) == 0x00) {
        // STUN message.
        expected_pkt_len = STUN_HEADER_SIZE + pkt_len;
    }
    else {
        // TURN ChannelData message.
        expected_pkt_len = TURN_CHANNLE_HEAD_LEN + pkt_len;
        // From RFC 5766 section 11.5
        // Over TCP and TLS-over-TCP, the ChannelData message MUST be padded to
        // a multiple of four bytes in order to ensure the alignment of
        // subsequent messages.  The padding is not reflected in the length
        // field of the ChannelData message, so the actual size of a ChannelData
        // message (including padding) is (4 + Length) rounded up to the nearest
        // multiple of 4.  Over UDP, the padding is not required but MAY be
        // included.
        if (expected_pkt_len % 4) *pad_bytes = 4 - (expected_pkt_len % 4);
    }
    return expected_pkt_len;
}

StunMessage::StunMessage() {
    memset(&header_, 0, sizeof(header_));
    header_.magic_cookie = STUN_MAGIC_COOKIE;
    header_.transaction_id = stun_generate_transaction_id();
}

StunMessage::StunMessage(uint16_t method, uint16_t cls) {
    memset(&header_, 0, sizeof(header_));
    header_.type = stun_make_type(method, cls);
    header_.magic_cookie = STUN_MAGIC_COOKIE;
    header_.transaction_id = stun_generate_transaction_id();
}

void StunMessage::setType(uint16_t method, uint16_t cls) {
    header_.type = stun_make_type(method, cls);
}

void StunMessage::addAttribute(uint16_t type, const void* data, size_t len) {
    StunAttribute attr;
    attr.type = type;
    if (data && len > 0) {
        attr.value.assign((const uint8_t*)data, (const uint8_t*)data + len);
    }
    attrs_.push_back(std::move(attr));
}

void StunMessage::addAttribute(const StunAttribute& attr) {
    attrs_.push_back(attr);
}

const StunAttribute* StunMessage::getAttribute(uint16_t type) const {
    for (const auto& attr : attrs_) {
        if (attr.type == type) return &attr;
    }
    return nullptr;
}

void StunMessage::addUsername(const std::string& username) {
    addAttribute(STUN_ATTR_USERNAME, username.data(), username.size());
}

void StunMessage::addPriority(uint32_t priority) {
    uint8_t buf[4];
    write_be32(buf, priority);
    addAttribute(STUN_ATTR_PRIORITY, buf, 4);
}

void StunMessage::addUseCandidate() {
    addAttribute(STUN_ATTR_USE_CANDIDATE, nullptr, 0);
}

void StunMessage::addIceControlling(uint64_t tiebreaker) {
    uint8_t buf[8];
    write_be64(buf, tiebreaker);
    addAttribute(STUN_ATTR_ICE_CONTROLLING, buf, 8);
}

void StunMessage::addIceControlled(uint64_t tiebreaker) {
    uint8_t buf[8];
    write_be64(buf, tiebreaker);
    addAttribute(STUN_ATTR_ICE_CONTROLLED, buf, 8);
}

void StunMessage::encodeAddress(std::vector<uint8_t>& value,
                                 const struct sockaddr* addr,
                                 bool xor_mapped,
                                 const TransactionId& tid) {
    value.clear();
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in* addr4 = (const struct sockaddr_in*)addr;
        value.resize(8);
        value[0] = 0; // Reserved
        value[1] = 0x01; // IPv4
        uint16_t port = ntohs(addr4->sin_port);
        uint32_t ip = ntohl(addr4->sin_addr.s_addr);
        if (xor_mapped) {
            port ^= (uint16_t)(STUN_MAGIC_COOKIE >> 16);
            ip ^= STUN_MAGIC_COOKIE;
        }
        write_be16(&value[2], port);
        write_be32(&value[4], ip);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6* addr6 = (const struct sockaddr_in6*)addr;
        value.resize(20);
        value[0] = 0; // Reserved
        value[1] = 0x02; // IPv6
        uint16_t port = ntohs(addr6->sin6_port);
        if (xor_mapped) {
            port ^= (uint16_t)(STUN_MAGIC_COOKIE >> 16);
        }
        write_be16(&value[2], port);
        // Copy IPv6 address (16 bytes)
        memcpy(&value[4], &addr6->sin6_addr, 16);
        if (xor_mapped) {
            // XOR with magic cookie + transaction id
            uint8_t xor_key[16];
            write_be32(xor_key, STUN_MAGIC_COOKIE);
            memcpy(xor_key + 4, tid.data(), 12);
            for (int i = 0; i < 16; ++i) {
                value[4 + i] ^= xor_key[i];
            }
        }
    }
}

bool StunMessage::decodeAddress(const uint8_t* data, size_t len,
                                 struct sockaddr_storage* addr,
                                 bool xor_mapped,
                                 const TransactionId& tid) {
    if (len < 4) return false;
    uint8_t family = data[1];
    uint16_t port = read_be16(data + 2);

    if (family == 0x01) { // IPv4
        if (len < 8) return false;
        uint32_t ip = read_be32(data + 4);
        if (xor_mapped) {
            port ^= (uint16_t)(STUN_MAGIC_COOKIE >> 16);
            ip ^= STUN_MAGIC_COOKIE;
        }
        struct sockaddr_in* addr4 = (struct sockaddr_in*)addr;
        memset(addr4, 0, sizeof(*addr4));
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(port);
        addr4->sin_addr.s_addr = htonl(ip);
        return true;
    } else if (family == 0x02) { // IPv6
        if (len < 20) return false;
        struct sockaddr_in6* addr6 = (struct sockaddr_in6*)addr;
        memset(addr6, 0, sizeof(*addr6));
        addr6->sin6_family = AF_INET6;
        if (xor_mapped) {
            port ^= (uint16_t)(STUN_MAGIC_COOKIE >> 16);
        }
        addr6->sin6_port = htons(port);
        memcpy(&addr6->sin6_addr, data + 4, 16);
        if (xor_mapped) {
            uint8_t xor_key[16];
            write_be32(xor_key, STUN_MAGIC_COOKIE);
            memcpy(xor_key + 4, tid.data(), 12);
            uint8_t* ip6 = (uint8_t*)&addr6->sin6_addr;
            for (int i = 0; i < 16; ++i) {
                ip6[i] ^= xor_key[i];
            }
        }
        return true;
    }
    return false;
}

void StunMessage::addXorMappedAddress(const struct sockaddr* addr) {
    std::vector<uint8_t> value;
    encodeAddress(value, addr, true, header_.transaction_id);
    addAttribute(STUN_ATTR_XOR_MAPPED_ADDRESS, value.data(), value.size());
}

void StunMessage::addMappedAddress(const struct sockaddr* addr) {
    std::vector<uint8_t> value;
    encodeAddress(value, addr, false, header_.transaction_id);
    addAttribute(STUN_ATTR_MAPPED_ADDRESS, value.data(), value.size());
}

void StunMessage::addErrorCode(uint16_t code, const std::string& reason) {
    std::vector<uint8_t> value(4 + reason.size());
    value[0] = 0;
    value[1] = 0;
    value[2] = (uint8_t)(code / 100); // class
    value[3] = (uint8_t)(code % 100); // number
    memcpy(value.data() + 4, reason.data(), reason.size());
    addAttribute(STUN_ATTR_ERROR_CODE, value.data(), value.size());
}

void StunMessage::addRealm(const std::string& realm) {
    addAttribute(STUN_ATTR_REALM, realm.data(), realm.size());
}

void StunMessage::addNonce(const std::string& nonce) {
    addAttribute(STUN_ATTR_NONCE, nonce.data(), nonce.size());
}

void StunMessage::addSoftware(const std::string& software) {
    addAttribute(STUN_ATTR_SOFTWARE, software.data(), software.size());
}

void StunMessage::addLifetime(uint32_t seconds) {
    uint8_t buf[4];
    write_be32(buf, seconds);
    addAttribute(STUN_ATTR_LIFETIME, buf, 4);
}

void StunMessage::addChannelNumber(uint16_t channel) {
    uint8_t buf[4];
    write_be16(buf, channel);
    write_be16(buf + 2, 0); // RFFU
    addAttribute(STUN_ATTR_CHANNEL_NUMBER, buf, 4);
}

void StunMessage::addXorPeerAddress(const struct sockaddr* addr) {
    std::vector<uint8_t> value;
    encodeAddress(value, addr, true, header_.transaction_id);
    addAttribute(STUN_ATTR_XOR_PEER_ADDRESS, value.data(), value.size());
}

void StunMessage::addXorRelayedAddress(const struct sockaddr* addr) {
    std::vector<uint8_t> value;
    encodeAddress(value, addr, true, header_.transaction_id);
    addAttribute(STUN_ATTR_XOR_RELAYED_ADDRESS, value.data(), value.size());
}

void StunMessage::addRequestedTransport(uint8_t protocol) {
    uint8_t buf[4] = {protocol, 0, 0, 0};
    addAttribute(STUN_ATTR_REQUESTED_TRANSPORT, buf, 4);
}

void StunMessage::addData(const void* data, size_t len) {
    addAttribute(STUN_ATTR_DATA, data, len);
}

// Getters
std::string StunMessage::getUsername() const {
    auto* attr = getAttribute(STUN_ATTR_USERNAME);
    if (!attr) return "";
    return std::string(attr->value.begin(), attr->value.end());
}

uint32_t StunMessage::getPriority() const {
    auto* attr = getAttribute(STUN_ATTR_PRIORITY);
    if (!attr || attr->value.size() < 4) return 0;
    return read_be32(attr->value.data());
}

bool StunMessage::hasUseCandidate() const {
    return getAttribute(STUN_ATTR_USE_CANDIDATE) != nullptr;
}

uint64_t StunMessage::getIceControlling() const {
    auto* attr = getAttribute(STUN_ATTR_ICE_CONTROLLING);
    if (!attr || attr->value.size() < 8) return 0;
    return read_be64(attr->value.data());
}

uint64_t StunMessage::getIceControlled() const {
    auto* attr = getAttribute(STUN_ATTR_ICE_CONTROLLED);
    if (!attr || attr->value.size() < 8) return 0;
    return read_be64(attr->value.data());
}

bool StunMessage::getXorMappedAddress(struct sockaddr_storage* addr) const {
    auto* attr = getAttribute(STUN_ATTR_XOR_MAPPED_ADDRESS);
    if (!attr) return false;
    return decodeAddress(attr->value.data(), attr->value.size(), addr, true, header_.transaction_id);
}

bool StunMessage::getMappedAddress(struct sockaddr_storage* addr) const {
    auto* attr = getAttribute(STUN_ATTR_MAPPED_ADDRESS);
    if (!attr) return false;
    return decodeAddress(attr->value.data(), attr->value.size(), addr, false, header_.transaction_id);
}

bool StunMessage::getErrorCode(uint16_t* code, std::string* reason) const {
    auto* attr = getAttribute(STUN_ATTR_ERROR_CODE);
    if (!attr || attr->value.size() < 4) return false;
    uint16_t cls = attr->value[2];
    uint16_t num = attr->value[3];
    if (code) *code = cls * 100 + num;
    if (reason && attr->value.size() > 4) {
        *reason = std::string(attr->value.begin() + 4, attr->value.end());
    }
    return true;
}

std::string StunMessage::getRealm() const {
    auto* attr = getAttribute(STUN_ATTR_REALM);
    if (!attr) return "";
    return std::string(attr->value.begin(), attr->value.end());
}

std::string StunMessage::getNonce() const {
    auto* attr = getAttribute(STUN_ATTR_NONCE);
    if (!attr) return "";
    return std::string(attr->value.begin(), attr->value.end());
}

uint32_t StunMessage::getLifetime() const {
    auto* attr = getAttribute(STUN_ATTR_LIFETIME);
    if (!attr || attr->value.size() < 4) return 0;
    return read_be32(attr->value.data());
}

uint16_t StunMessage::getChannelNumber() const {
    auto* attr = getAttribute(STUN_ATTR_CHANNEL_NUMBER);
    if (!attr || attr->value.size() < 2) return 0;
    return read_be16(attr->value.data());
}

bool StunMessage::getXorPeerAddress(struct sockaddr_storage* addr) const {
    auto* attr = getAttribute(STUN_ATTR_XOR_PEER_ADDRESS);
    if (!attr) return false;
    return decodeAddress(attr->value.data(), attr->value.size(), addr, true, header_.transaction_id);
}

bool StunMessage::getXorRelayedAddress(struct sockaddr_storage* addr) const {
    auto* attr = getAttribute(STUN_ATTR_XOR_RELAYED_ADDRESS);
    if (!attr) return false;
    return decodeAddress(attr->value.data(), attr->value.size(), addr, true, header_.transaction_id);
}

bool StunMessage::getData(const uint8_t** data, size_t* len) const {
    auto* attr = getAttribute(STUN_ATTR_DATA);
    if (!attr) return false;
    if (data) *data = attr->value.data();
    if (len) *len = attr->value.size();
    return true;
}

// Encode
void StunMessage::encodeHeader(std::vector<uint8_t>& buf, uint16_t length) const {
    size_t offset = buf.size();
    buf.resize(offset + STUN_HEADER_SIZE);
    uint8_t* p = buf.data() + offset;
    write_be16(p, header_.type);
    write_be16(p + 2, length);
    write_be32(p + 4, STUN_MAGIC_COOKIE);
    memcpy(p + 8, header_.transaction_id.data(), 12);
}

void StunMessage::encodeAttributes(std::vector<uint8_t>& buf) const {
    for (const auto& attr : attrs_) {
        size_t offset = buf.size();
        uint16_t value_len = (uint16_t)attr.value.size();
        uint16_t padded_len = (value_len + 3) & ~3; // align to 4 bytes
        buf.resize(offset + STUN_ATTR_HEADER_SIZE + padded_len);
        uint8_t* p = buf.data() + offset;
        write_be16(p, attr.type);
        write_be16(p + 2, value_len);
        if (value_len > 0) {
            memcpy(p + 4, attr.value.data(), value_len);
        }
        // Zero padding
        for (uint16_t i = value_len; i < padded_len; ++i) {
            p[4 + i] = 0;
        }
    }
}

std::vector<uint8_t> StunMessage::encode() const {
    if (auth_.length()) {
        return encodeWithAuth(auth_);
    }
    std::vector<uint8_t> buf;
    buf.reserve(256);

    // Calculate attributes length
    uint16_t attrs_len = 0;
    for (const auto& attr : attrs_) {
        attrs_len += STUN_ATTR_HEADER_SIZE + ((attr.value.size() + 3) & ~3);
    }

    encodeHeader(buf, attrs_len);
    encodeAttributes(buf);
    return buf;
}

std::vector<uint8_t> StunMessage::encodeWithAuth(const std::string& password) const {
    std::vector<uint8_t> buf;
    buf.reserve(256);

    // Calculate attributes length (including MESSAGE-INTEGRITY and FINGERPRINT)
    uint16_t attrs_len = 0;
    for (const auto& attr : attrs_) {
        attrs_len += STUN_ATTR_HEADER_SIZE + ((attr.value.size() + 3) & ~3);
    }
    // MESSAGE-INTEGRITY: 4 (header) + 20 (HMAC-SHA1) = 24
    uint16_t len_with_integrity = attrs_len + 24;
    // FINGERPRINT: 4 (header) + 4 (CRC32) = 8
    uint16_t len_with_fingerprint = len_with_integrity + 8;

    // Encode header with length including MESSAGE-INTEGRITY (for HMAC computation)
    encodeHeader(buf, len_with_integrity);
    encodeAttributes(buf);

    // Compute HMAC-SHA1 over [header + attributes] with length field = len_with_integrity
    uint8_t hmac[20];
    stun_hmac_sha1(password, buf.data(), buf.size(), hmac);

    // Add MESSAGE-INTEGRITY attribute
    size_t mi_offset = buf.size();
    buf.resize(mi_offset + 24);
    write_be16(buf.data() + mi_offset, STUN_ATTR_MESSAGE_INTEGRITY);
    write_be16(buf.data() + mi_offset + 2, 20);
    memcpy(buf.data() + mi_offset + 4, hmac, 20);

    // Update header length to include FINGERPRINT
    write_be16(buf.data() + 2, len_with_fingerprint);

    // Compute CRC32 over everything so far
    uint32_t crc = stun_crc32(buf.data(), buf.size()) ^ 0x5354554E;

    // Add FINGERPRINT attribute
    size_t fp_offset = buf.size();
    buf.resize(fp_offset + 8);
    write_be16(buf.data() + fp_offset, STUN_ATTR_FINGERPRINT);
    write_be16(buf.data() + fp_offset + 2, 4);
    write_be32(buf.data() + fp_offset + 4, crc);

    return buf;
}

// Decode
bool StunMessage::decode(const uint8_t* data, size_t len, StunMessage* msg) {
    if (!data || len < STUN_HEADER_SIZE) return false;

    // Check magic cookie
    uint32_t cookie = read_be32(data + 4);
    if (cookie != STUN_MAGIC_COOKIE) return false;

    // Check first 2 bits are 0
    if ((data[0] & 0xC0) != 0) return false;

    msg->header_.type = read_be16(data);
    msg->header_.length = read_be16(data + 2);
    msg->header_.magic_cookie = cookie;
    memcpy(msg->header_.transaction_id.data(), data + 8, 12);

    // Verify length
    if (STUN_HEADER_SIZE + msg->header_.length > len) return false;

    // Store raw data for integrity check
    msg->raw_data_.assign(data, data + len);

    // Parse attributes
    msg->attrs_.clear();
    size_t offset = STUN_HEADER_SIZE;
    size_t end = STUN_HEADER_SIZE + msg->header_.length;
    while (offset + STUN_ATTR_HEADER_SIZE <= end) {
        uint16_t attr_type = read_be16(data + offset);
        uint16_t attr_len = read_be16(data + offset + 2);
        if (offset + STUN_ATTR_HEADER_SIZE + attr_len > end) break;

        StunAttribute attr;
        attr.type = attr_type;
        attr.value.assign(data + offset + 4, data + offset + 4 + attr_len);
        msg->attrs_.push_back(std::move(attr));

        // Move to next attribute (padded to 4 bytes)
        offset += STUN_ATTR_HEADER_SIZE + ((attr_len + 3) & ~3);
    }

    return true;
}

bool StunMessage::verifyIntegrity(const std::string& password) const {
    if (raw_data_.empty()) return false;

    // Find MESSAGE-INTEGRITY attribute position in raw data
    size_t offset = STUN_HEADER_SIZE;
    size_t mi_offset = 0;
    while (offset + STUN_ATTR_HEADER_SIZE <= raw_data_.size()) {
        uint16_t attr_type = read_be16(raw_data_.data() + offset);
        uint16_t attr_len = read_be16(raw_data_.data() + offset + 2);
        if (attr_type == STUN_ATTR_MESSAGE_INTEGRITY) {
            mi_offset = offset;
            break;
        }
        offset += STUN_ATTR_HEADER_SIZE + ((attr_len + 3) & ~3);
    }
    if (mi_offset == 0) return false;

    // Compute HMAC over data up to MESSAGE-INTEGRITY
    // Need to adjust length field in header
    std::vector<uint8_t> tmp(raw_data_.begin(), raw_data_.begin() + mi_offset);
    // Set length to include MESSAGE-INTEGRITY attribute (mi_offset - 20 + 24)
    uint16_t new_len = (uint16_t)(mi_offset - STUN_HEADER_SIZE + 24);
    write_be16(tmp.data() + 2, new_len);

    uint8_t computed_hmac[20];
    stun_hmac_sha1(password, tmp.data(), tmp.size(), computed_hmac);

    // Compare with actual HMAC
    const uint8_t* actual_hmac = raw_data_.data() + mi_offset + 4;
    return memcmp(computed_hmac, actual_hmac, 20) == 0;
}

bool StunMessage::verifyFingerprint() const {
    if (raw_data_.empty()) return false;

    // Find FINGERPRINT attribute position in raw data
    size_t offset = STUN_HEADER_SIZE;
    size_t fp_offset = 0;
    while (offset + STUN_ATTR_HEADER_SIZE <= raw_data_.size()) {
        uint16_t attr_type = read_be16(raw_data_.data() + offset);
        uint16_t attr_len = read_be16(raw_data_.data() + offset + 2);
        if (attr_type == STUN_ATTR_FINGERPRINT) {
            fp_offset = offset;
            break;
        }
        offset += STUN_ATTR_HEADER_SIZE + ((attr_len + 3) & ~3);
    }
    if (fp_offset == 0) return false;

    // CRC32 over everything before FINGERPRINT, with length adjusted
    std::vector<uint8_t> tmp(raw_data_.begin(), raw_data_.begin() + fp_offset);
    uint16_t new_len = (uint16_t)(fp_offset - STUN_HEADER_SIZE + 8);
    write_be16(tmp.data() + 2, new_len);

    uint32_t computed_crc = stun_crc32(tmp.data(), tmp.size()) ^ 0x5354554E;
    uint32_t actual_crc = read_be32(raw_data_.data() + fp_offset + 4);

    return computed_crc == actual_crc;
}

bool StunMessage::isStun(const uint8_t* data, size_t len) {
    if (len < STUN_HEADER_SIZE) return false;
    // First 2 bits must be 0
    if ((data[0] & 0xC0) != 0) return false;
    // Magic cookie check
    uint32_t cookie = read_be32(data + 4);
    return cookie == STUN_MAGIC_COOKIE;
}

} // namespace ice
