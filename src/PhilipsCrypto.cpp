#include "cpapdash/parser/PhilipsCrypto.h"

#ifdef CPAPDASH_WITH_PHILIPS

#include <openssl/evp.h>
#include <openssl/aes.h>
#include <cstring>
#include <fstream>
#include <functional>

namespace cpapdash::parser {

// OSCAR constants for DS2 decryption
static const uint8_t OSCAR_KEY[] = "Patient access to their own data";  // 32 bytes + null
static const uint8_t COMMON_KEY_ENC[] = {
    0x75, 0xB3, 0xA2, 0x12, 0x4A, 0x65, 0xAF, 0x97,
    0x54, 0xD8, 0xC1, 0xF3, 0xE5, 0x2E, 0xB6, 0xF0,
    0x23, 0x20, 0x57, 0x69, 0x7E, 0x38, 0x0E, 0xC9,
    0x4A, 0xDC, 0x46, 0x45, 0xB6, 0x92, 0x5A, 0x98
};

// DS2 header: magic(2) + version(2) + GUID(2+36) + IV(2+12) + salt(2+16) +
// reserved(2) + import_key(2+32) + import_tag(2+16) + export_key(2+32) +
// export_tag(2+16) + payload_tag(2+16) = 198 bytes
// Note: OSCAR uses 0xCA (202); the 4-byte difference may be additional reserved
// bytes. Will validate against real DS2 files when sample data arrives.
static constexpr size_t DS2_HEADER_SIZE = 198;

// ── AES helpers ──────────────────────────────────────────────────────────────

static bool aes256_ecb_decrypt(const uint8_t* key, const uint8_t* in,
                                uint8_t* out, size_t len) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    int outlen = 0, finallen = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_ecb(), nullptr, key, nullptr) &&
        EVP_CIPHER_CTX_set_padding(ctx, 0) &&
        EVP_DecryptUpdate(ctx, out, &outlen, in, static_cast<int>(len)) &&
        EVP_DecryptFinal_ex(ctx, out + outlen, &finallen)) {
        ok = true;
    }

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static bool aes256_gcm_decrypt(const uint8_t* key, const uint8_t* iv, size_t iv_len,
                                const uint8_t* ciphertext, size_t ct_len,
                                const uint8_t* tag, size_t tag_len,
                                uint8_t* plaintext) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    int outlen = 0, finallen = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv_len), nullptr) &&
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, iv) &&
        EVP_DecryptUpdate(ctx, plaintext, &outlen, ciphertext, static_cast<int>(ct_len)) &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag_len),
                            const_cast<uint8_t*>(tag)) &&
        EVP_DecryptFinal_ex(ctx, plaintext + outlen, &finallen)) {
        ok = true;
    }

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static bool pbkdf2_sha256(const uint8_t* password, size_t pass_len,
                           const uint8_t* salt, size_t salt_len,
                           int iterations, uint8_t* out, size_t out_len) {
    return PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(password),
                              static_cast<int>(pass_len),
                              salt, static_cast<int>(salt_len),
                              iterations, EVP_sha256(),
                              static_cast<int>(out_len), out) == 1;
}

// ── DS2Header parsing ────────────────────────────────────────────────────────

bool PhilipsCrypto::parseHeader(const uint8_t* data, size_t len, DS2Header& header) {
    if (len < DS2_HEADER_SIZE) return false;

    // Magic: 0D 01
    if (data[0] != 0x0D || data[1] != 0x01) return false;

    header.magic[0] = data[0];
    header.magic[1] = data[1];
    header.version[0] = data[2];
    header.version[1] = data[3];

    size_t pos = 4;

    // GUID: length (2 LE) + data
    uint16_t guid_len = data[pos] | (data[pos + 1] << 8);
    pos += 2;
    if (guid_len != 36 || pos + guid_len > DS2_HEADER_SIZE) return false;
    header.guid.assign(data + pos, data + pos + guid_len);
    pos += guid_len;

    // IV: length (2 LE) + data
    uint16_t iv_len = data[pos] | (data[pos + 1] << 8);
    pos += 2;
    if (iv_len != 12 || pos + iv_len > DS2_HEADER_SIZE) return false;
    header.iv.assign(data + pos, data + pos + iv_len);
    pos += iv_len;

    // Salt: length (2 LE) + data
    uint16_t salt_len = data[pos] | (data[pos + 1] << 8);
    pos += 2;
    if (salt_len != 16 || pos + salt_len > DS2_HEADER_SIZE) return false;
    header.salt.assign(data + pos, data + pos + salt_len);
    pos += salt_len;

    // Reserved: 2 bytes (00 01)
    pos += 2;

    // Import key: length (2 LE) + 32 bytes
    uint16_t ik_len = data[pos] | (data[pos + 1] << 8);
    pos += 2;
    if (ik_len != 32 || pos + ik_len > DS2_HEADER_SIZE) return false;
    header.import_key.assign(data + pos, data + pos + ik_len);
    pos += ik_len;

    // Import tag: length (2 LE) + 16 bytes
    uint16_t it_len = data[pos] | (data[pos + 1] << 8);
    pos += 2;
    if (it_len != 16 || pos + it_len > DS2_HEADER_SIZE) return false;
    header.import_tag.assign(data + pos, data + pos + it_len);
    pos += it_len;

    // Export key: length (2 LE) + 32 bytes
    uint16_t ek_len = data[pos] | (data[pos + 1] << 8);
    pos += 2;
    if (ek_len != 32 || pos + ek_len > DS2_HEADER_SIZE) return false;
    header.export_key.assign(data + pos, data + pos + ek_len);
    pos += ek_len;

    // Export tag: length (2 LE) + 16 bytes
    uint16_t et_len = data[pos] | (data[pos + 1] << 8);
    pos += 2;
    if (et_len != 16 || pos + et_len > DS2_HEADER_SIZE) return false;
    header.export_tag.assign(data + pos, data + pos + et_len);
    pos += et_len;

    // Payload tag: length (2 LE) + 16 bytes
    uint16_t pt_len = data[pos] | (data[pos + 1] << 8);
    pos += 2;
    if (pt_len != 16 || pos + pt_len > DS2_HEADER_SIZE) return false;
    header.payload_tag.assign(data + pos, data + pos + pt_len);

    return true;
}

// ── Key derivation ───────────────────────────────────────────────────────────

bool PhilipsCrypto::derivePayloadKey(const DS2Header& header,
                                      std::vector<uint8_t>& payload_key) {
    // Build cache key from iv + salt + export_key + export_tag
    std::string cache_key;
    cache_key.append(reinterpret_cast<const char*>(header.iv.data()), header.iv.size());
    cache_key.append(reinterpret_cast<const char*>(header.salt.data()), header.salt.size());
    cache_key.append(reinterpret_cast<const char*>(header.export_key.data()), header.export_key.size());
    cache_key.append(reinterpret_cast<const char*>(header.export_tag.data()), header.export_tag.size());

    auto it = key_cache_.find(cache_key);
    if (it != key_cache_.end()) {
        payload_key = it->second;
        return true;
    }

    // Step 1: Decrypt COMMON_KEY_ENC with OSCAR_KEY via AES-256-ECB
    uint8_t common_key[32];
    if (!aes256_ecb_decrypt(OSCAR_KEY, COMMON_KEY_ENC, common_key, 32)) {
        return false;
    }

    // Step 2: PBKDF2-SHA256(common_key, salt, 10000) -> salted_key
    uint8_t salted_key[32];
    if (!pbkdf2_sha256(common_key, 32, header.salt.data(), header.salt.size(),
                       10000, salted_key, 32)) {
        return false;
    }

    // Step 3: AES-256-GCM decrypt export_key -> payload_key
    payload_key.resize(32);
    if (!aes256_gcm_decrypt(salted_key, header.iv.data(), header.iv.size(),
                            header.export_key.data(), header.export_key.size(),
                            header.export_tag.data(), header.export_tag.size(),
                            payload_key.data())) {
        return false;
    }

    key_cache_[cache_key] = payload_key;
    return true;
}

// ── Public API ───────────────────────────────────────────────────────────────

std::vector<uint8_t> PhilipsCrypto::decrypt(const uint8_t* data, size_t len) {
    if (len < DS2_HEADER_SIZE) return {};

    DS2Header header;
    if (!parseHeader(data, len, header)) return {};

    std::vector<uint8_t> payload_key;
    if (!derivePayloadKey(header, payload_key)) return {};

    // Decrypt payload (everything after 0xCA header)
    const uint8_t* ciphertext = data + DS2_HEADER_SIZE;
    size_t ct_len = len - DS2_HEADER_SIZE;

    std::vector<uint8_t> plaintext(ct_len);
    if (!aes256_gcm_decrypt(payload_key.data(),
                            header.iv.data(), header.iv.size(),
                            ciphertext, ct_len,
                            header.payload_tag.data(), header.payload_tag.size(),
                            plaintext.data())) {
        return {};
    }

    return plaintext;
}

std::vector<uint8_t> PhilipsCrypto::decryptFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) return {};

    auto size = file.tellg();
    file.seekg(0);

    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);

    return decrypt(data.data(), data.size());
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_PHILIPS
