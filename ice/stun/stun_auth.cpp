#include "stun_auth.h"
#include <cstring>
#include "md5.h"
#include "sha1.h"

namespace ice {

// HMAC-SHA1 implementation (RFC 2104)
void stun_hmac_sha1(const std::string& key, const uint8_t* data, size_t len, uint8_t out[20]) {
    const size_t BLOCK_SIZE = 64;
    const size_t HASH_SIZE = 20;

    uint8_t k_ipad[BLOCK_SIZE];
    uint8_t k_opad[BLOCK_SIZE];
    uint8_t tk[HASH_SIZE];

    const uint8_t* key_data = (const uint8_t*)key.data();
    size_t key_len = key.size();

    // If key is longer than block size, hash it first
    if (key_len > BLOCK_SIZE) {
        HV_SHA1_CTX ctx;
        HV_SHA1Init(&ctx);
        HV_SHA1Update(&ctx, key_data, (uint32_t)key_len);
        HV_SHA1Final(tk, &ctx);
        key_data = tk;
        key_len = HASH_SIZE;
    }

    // XOR key with ipad and opad
    memset(k_ipad, 0x36, BLOCK_SIZE);
    memset(k_opad, 0x5C, BLOCK_SIZE);
    for (size_t i = 0; i < key_len; ++i) {
        k_ipad[i] ^= key_data[i];
        k_opad[i] ^= key_data[i];
    }

    // Inner hash: H(K XOR ipad, data)
    HV_SHA1_CTX ctx;
    HV_SHA1Init(&ctx);
    HV_SHA1Update(&ctx, k_ipad, BLOCK_SIZE);
    HV_SHA1Update(&ctx, data, (uint32_t)len);
    uint8_t inner_hash[HASH_SIZE];
    HV_SHA1Final(inner_hash, &ctx);

    // Outer hash: H(K XOR opad, inner_hash)
    HV_SHA1Init(&ctx);
    HV_SHA1Update(&ctx, k_opad, BLOCK_SIZE);
    HV_SHA1Update(&ctx, inner_hash, HASH_SIZE);
    HV_SHA1Final(out, &ctx);
}

// RFC 5766 Section 10.2: long-term credential HMAC key
// key = MD5(username ":" realm ":" password) as 16-byte binary digest
std::string long_turn_auth_key(const std::string& username, const std::string& realm, const std::string& password) {
    std::string raw = username + ":" + realm + ":" + password;
    uint8_t hex[16] = {0};
    hv_md5((unsigned char*)raw.data(), (unsigned int)raw.size(), hex);
    return std::string((char*)hex, 16);
}

// CRC32 lookup table (standard polynomial 0xEDB88320)
static uint32_t crc32_table[256];
static bool crc32_table_init = false;

static void init_crc32_table() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
    crc32_table_init = true;
}

uint32_t stun_crc32(const uint8_t* data, size_t len) {
    if (!crc32_table_init) {
        init_crc32_table();
    }
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

} // namespace ice
