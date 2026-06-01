#ifndef ICE_STUN_MESSAGE_H_
#define ICE_STUN_MESSAGE_H_

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <memory>

#include "hsocket.h"

namespace ice {

// STUN Magic Cookie (RFC 5389)
static constexpr uint32_t STUN_MAGIC_COOKIE = 0x2112A442;

// STUN Message Types (RFC 5389 Section 6)
enum StunMethod : uint16_t {
    STUN_METHOD_BINDING          = 0x0001,
    // TURN methods (RFC 5766)
    TURN_METHOD_ALLOCATE         = 0x0003,
    TURN_METHOD_REFRESH          = 0x0004,
    TURN_METHOD_SEND             = 0x0006,
    TURN_METHOD_DATA             = 0x0007,
    TURN_METHOD_CREATE_PERMISSION = 0x0008,
    TURN_METHOD_CHANNEL_BIND     = 0x0009,
};

enum StunClass : uint16_t {
    STUN_CLASS_REQUEST           = 0x00,  // C1=0, C0=0
    STUN_CLASS_INDICATION        = 0x01,  // C1=0, C0=1
    STUN_CLASS_SUCCESS_RESPONSE  = 0x02,  // C1=1, C0=0
    STUN_CLASS_ERROR_RESPONSE    = 0x03,  // C1=1, C0=1
};

// Encode method + class into message type (RFC 5389 Section 6)
inline uint16_t stun_make_type(uint16_t method, uint16_t cls) {
    // M11-M7 | C1 | M6-M4 | C0 | M3-M0
    uint16_t m = method;
    uint16_t c = cls;
    return ((m & 0x0F80) << 2) | ((m & 0x0070) << 1) | (m & 0x000F) |
           ((c & 0x02) << 7) | ((c & 0x01) << 4);
}

inline uint16_t stun_get_method(uint16_t type) {
    return ((type & 0x3E00) >> 2) | ((type & 0x00E0) >> 1) | (type & 0x000F);
}

inline uint16_t stun_get_class(uint16_t type) {
    return ((type & 0x0100) >> 7) | ((type & 0x0010) >> 4);
}

// Packet type classification
enum class PacketType {
    STUN,           // STUN message (first byte 0x00 or 0x01)
    TURN_CHANNEL,   // TURN ChannelData (first byte 0x40-0x7F)
    DATA            // Application data (other)
};

// Classify incoming packet
inline PacketType classifyPacket(const uint8_t* data, size_t len) {
    if (len < 1) return PacketType::DATA;
    uint8_t first = data[0];
    if ((first & 0xC0) == 0x00) return PacketType::STUN;      // 0x00-0x3F
    if (first >= 0x40 && first <= 0x7F) return PacketType::TURN_CHANNEL;
    return PacketType::DATA;
}

// Helper: write big-endian
inline uint8_t* write_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
    return p + 2;
}

inline uint8_t* write_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFF);
    return p + 4;
}

inline uint8_t* write_be64(uint8_t* p, uint64_t v) {
    write_be32(p, (uint32_t)(v >> 32));
    write_be32(p + 4, (uint32_t)(v & 0xFFFFFFFF));
    return p + 8;
}

inline uint16_t read_be16(const uint8_t* p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

inline uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

inline uint64_t read_be64(const uint8_t* p) {
    return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

static constexpr uint32_t TURN_CHANNLE_HEAD_LEN = 4;
size_t TurnTcpLength(const uint8_t* view, size_t len, int* pad_bytes);

// Compare sockaddr for use as map key
struct SockaddrCompare {
    bool operator()(const sockaddr_u& a, const sockaddr_u& b) const {
        return sockaddr_compare(&a, &b) < 0;
    }
};

// STUN Attribute Types (RFC 5389 + RFC 5245 + RFC 5766)
enum StunAttrType : uint16_t {
    // Comprehension-required (0x0000 - 0x7FFF)
    STUN_ATTR_MAPPED_ADDRESS      = 0x0001,
    STUN_ATTR_USERNAME            = 0x0006,
    STUN_ATTR_MESSAGE_INTEGRITY   = 0x0008,
    STUN_ATTR_ERROR_CODE          = 0x0009,
    STUN_ATTR_UNKNOWN_ATTRIBUTES  = 0x000A,
    STUN_ATTR_REALM               = 0x0014,
    STUN_ATTR_NONCE               = 0x0015,
    STUN_ATTR_XOR_MAPPED_ADDRESS  = 0x0020,

    // ICE attributes (RFC 5245)
    STUN_ATTR_PRIORITY            = 0x0024,
    STUN_ATTR_USE_CANDIDATE       = 0x0025,
    STUN_ATTR_ICE_CONTROLLED      = 0x8029,
    STUN_ATTR_ICE_CONTROLLING     = 0x802A,

    // TURN attributes (RFC 5766)
    STUN_ATTR_CHANNEL_NUMBER      = 0x000C,
    STUN_ATTR_LIFETIME            = 0x000D,
    STUN_ATTR_XOR_PEER_ADDRESS    = 0x0012,
    STUN_ATTR_DATA                = 0x0013,
    STUN_ATTR_XOR_RELAYED_ADDRESS = 0x0016,
    STUN_ATTR_REQUESTED_TRANSPORT = 0x0019,

    // Comprehension-optional (0x8000 - 0xFFFF)
    STUN_ATTR_SOFTWARE            = 0x8022,
    STUN_ATTR_FINGERPRINT         = 0x8028,
};

// STUN Error Codes
enum StunErrorCode : uint16_t {
    STUN_ERROR_TRY_ALTERNATE      = 300,
    STUN_ERROR_BAD_REQUEST        = 400,
    STUN_ERROR_UNAUTHORIZED       = 401,
    STUN_ERROR_FORBIDDEN          = 403,
    STUN_ERROR_UNKNOWN_ATTRIBUTE  = 420,
    STUN_ERROR_STALE_NONCE        = 438,
    STUN_ERROR_ROLE_CONFLICT      = 487,
    STUN_ERROR_SERVER_ERROR       = 500,
    STUN_ERROR_INSUFFICIENT_CAPACITY = 508,
};

// Transaction ID: 96 bits
using TransactionId = std::array<uint8_t, 12>;

// Generate random transaction ID
TransactionId stun_generate_transaction_id();
std::string TransactionIdStr(const TransactionId& id);

// STUN Header: 20 bytes
struct StunHeader {
    uint16_t type;
    uint16_t length;   // payload length (excluding 20-byte header)
    uint32_t magic_cookie;
    TransactionId transaction_id;
};

static constexpr size_t STUN_HEADER_SIZE = 20;
static constexpr size_t STUN_ATTR_HEADER_SIZE = 4;

// STUN Attribute (TLV)
struct StunAttribute {
    uint16_t type;
    std::vector<uint8_t> value;

    StunAttribute() : type(0) {}
    StunAttribute(uint16_t t, const void* data, size_t len)
        : type(t), value((uint8_t*)data, (uint8_t*)data + len) {}
};

// STUN Message
class StunMessage {
public:
    StunMessage();
    StunMessage(uint16_t method, uint16_t cls);

    // Header
    void setType(uint16_t method, uint16_t cls);
    uint16_t type() const { return header_.type; }
    uint16_t method() const { return stun_get_method(header_.type); }
    uint16_t cls() const { return stun_get_class(header_.type); }

    std::string IdStr() const { return TransactionIdStr(header_.transaction_id); }
    const TransactionId& transactionId() const { return header_.transaction_id; }
    void setTransactionId(const TransactionId& id) { header_.transaction_id = id; }

    // Attributes
    const std::vector<StunAttribute>& attributes() const { return attrs_; }
    const StunAttribute* getAttribute(uint16_t type) const;
    void addAttribute(uint16_t type, const void* data, size_t len);
    void addAttribute(const StunAttribute& attr);

    // Convenient attribute setters
    void addUsername(const std::string& username);
    void addPriority(uint32_t priority);
    void addUseCandidate();
    void addIceControlling(uint64_t tiebreaker);
    void addIceControlled(uint64_t tiebreaker);
    void addXorMappedAddress(const struct sockaddr* addr);
    void addMappedAddress(const struct sockaddr* addr);
    void addErrorCode(uint16_t code, const std::string& reason);
    void addRealm(const std::string& realm);
    void addNonce(const std::string& nonce);
    void addSoftware(const std::string& software);
    void addLifetime(uint32_t seconds);
    void addChannelNumber(uint16_t channel);
    void addXorPeerAddress(const struct sockaddr* addr);
    void addXorRelayedAddress(const struct sockaddr* addr);
    void addRequestedTransport(uint8_t protocol);
    void addData(const void* data, size_t len);

    // Convenient attribute getters
    std::string getUsername() const;
    uint32_t getPriority() const;
    bool hasUseCandidate() const;
    uint64_t getIceControlling() const;
    uint64_t getIceControlled() const;
    bool getXorMappedAddress(struct sockaddr_storage* addr) const;
    bool getMappedAddress(struct sockaddr_storage* addr) const;
    bool getErrorCode(uint16_t* code, std::string* reason) const;
    std::string getRealm() const;
    std::string getNonce() const;
    uint32_t getLifetime() const;
    uint16_t getChannelNumber() const;
    bool getXorPeerAddress(struct sockaddr_storage* addr) const;
    bool getXorRelayedAddress(struct sockaddr_storage* addr) const;
    bool getData(const uint8_t** data, size_t* len) const;

    // Encode/Decode
    // Encode message to buffer (without integrity/fingerprint)
    std::vector<uint8_t> encode() const;
    void setAuth(const std::string& password) { auth_ = password; }
    // Encode with MESSAGE-INTEGRITY and FINGERPRINT
    std::vector<uint8_t> encodeWithAuth(const std::string& password) const;

    // Decode from buffer
    static bool decode(const uint8_t* data, size_t len, StunMessage* msg);

    // Validate integrity
    bool verifyIntegrity(const std::string& password) const;
    bool verifyFingerprint() const;

    // Check if buffer looks like a STUN message
    static bool isStun(const uint8_t* data, size_t len);

private:
    void encodeHeader(std::vector<uint8_t>& buf, uint16_t length) const;
    void encodeAttributes(std::vector<uint8_t>& buf) const;
    static void encodeAddress(std::vector<uint8_t>& value, const struct sockaddr* addr, bool xor_mapped, const TransactionId& tid);
    static bool decodeAddress(const uint8_t* data, size_t len, struct sockaddr_storage* addr, bool xor_mapped, const TransactionId& tid);

    StunHeader header_;
    std::vector<StunAttribute> attrs_;
    // Store raw bytes for integrity verification
    mutable std::vector<uint8_t> raw_data_;
    std::string auth_;
};

} // namespace ice

#endif // ICE_STUN_MESSAGE_H_
