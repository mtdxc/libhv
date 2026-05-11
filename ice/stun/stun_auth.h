#ifndef ICE_STUN_AUTH_H_
#define ICE_STUN_AUTH_H_

#include <cstdint>
#include <cstddef>
#include <string>

namespace ice {

// Compute HMAC-SHA1 for STUN MESSAGE-INTEGRITY
// key: ice-pwd (short-term credential)
// data: STUN message bytes up to (but not including) MESSAGE-INTEGRITY attr
// out: 20-byte HMAC result
void stun_hmac_sha1(const std::string& key, const uint8_t* data, size_t len, uint8_t out[20]);

// Compute CRC32 for STUN FINGERPRINT
// Note: FINGERPRINT value = CRC32 XOR 0x5354554E
uint32_t stun_crc32(const uint8_t* data, size_t len);

} // namespace ice

#endif // ICE_STUN_AUTH_H_
