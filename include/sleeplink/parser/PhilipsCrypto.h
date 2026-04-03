#pragma once

#ifdef SLEEPLINK_WITH_PHILIPS

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace sleeplink::parser {

/**
 * DS2 encrypted file header (0xCA = 202 bytes).
 */
struct DS2Header {
    uint8_t magic[2];           // 0D 01
    uint8_t version[2];         // 01 01
    std::vector<uint8_t> guid;  // 36-byte device GUID
    std::vector<uint8_t> iv;    // 12-byte IV (nonce)
    std::vector<uint8_t> salt;  // 16-byte PBKDF2 salt
    std::vector<uint8_t> import_key;     // 32-byte encrypted import key
    std::vector<uint8_t> import_tag;     // 16-byte import key auth tag
    std::vector<uint8_t> export_key;     // 32-byte encrypted export key
    std::vector<uint8_t> export_tag;     // 16-byte export key auth tag
    std::vector<uint8_t> payload_tag;    // 16-byte payload auth tag
};

/**
 * PhilipsCrypto - AES-256-GCM decryption for DreamStation 2 files.
 *
 * DS2 encrypts all data files (.B01, .B02, .B05) and PROP.BIN.
 * Uses PBKDF2-SHA256 key derivation with 10,000 iterations.
 * Key caching avoids redundant PBKDF2 calls across files from same device.
 */
class PhilipsCrypto {
public:
    /**
     * Decrypt a DS2 encrypted file from memory.
     * @return Decrypted payload, or empty vector on failure.
     */
    std::vector<uint8_t> decrypt(const uint8_t* data, size_t len);

    /**
     * Decrypt a DS2 encrypted file from disk.
     * @return Decrypted payload, or empty vector on failure.
     */
    std::vector<uint8_t> decryptFile(const std::string& filepath);

    /** Parse DS2 header from buffer. */
    static bool parseHeader(const uint8_t* data, size_t len, DS2Header& header);

    /** Clear the key cache. */
    void clearCache() { key_cache_.clear(); }

private:
    bool derivePayloadKey(const DS2Header& header, std::vector<uint8_t>& payload_key);

    // Cache: hash(iv + salt + export_key + export_tag) -> derived payload key
    std::unordered_map<std::string, std::vector<uint8_t>> key_cache_;
};

} // namespace sleeplink::parser

#endif // SLEEPLINK_WITH_PHILIPS
