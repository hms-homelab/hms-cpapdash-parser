#pragma once

#ifdef SLEEPLINK_WITH_PHILIPS

#include <cstdint>
#include <vector>
#include <map>
#include <utility>

namespace sleeplink::parser {

/**
 * PRS1 chunk header — common binary header for all Philips data files.
 */
struct PRS1ChunkHeader {
    uint8_t file_version = 0;     // 2 or 3
    uint16_t block_size = 0;
    uint8_t htype = 0;            // 0=normal, 1=waveform
    uint8_t family = 0;           // 0=xPAP
    uint8_t family_version = 0;   // 6=DreamStation 2
    uint8_t ext = 0;              // file type (1=summary, 2=events, 5=waveform)
    uint32_t session_id = 0;
    uint32_t timestamp = 0;       // unix epoch seconds

    // FileVersion 3 extended header: event code -> data size mapping
    std::map<uint8_t, int16_t> hblock;

    // Waveform-specific (htype=1)
    uint16_t interval_count = 0;
    uint8_t interval_seconds = 0;
    int duration = 0;
};

/**
 * PRS1ChunkReader - Parses PRS1 binary chunk format.
 *
 * A decrypted PRS1 file consists of one or more chunks, each with:
 * - Header (variable length based on fileVersion)
 * - Data payload
 * - CRC-16 (Kermit polynomial 0x1021)
 */
class PRS1ChunkReader {
public:
    /**
     * Parse all chunks from a decrypted PRS1 file buffer.
     * @return Vector of (header, data) pairs.
     */
    static std::vector<std::pair<PRS1ChunkHeader, std::vector<uint8_t>>>
        parseChunks(const uint8_t* data, size_t len);

    /** Compute 8-bit additive checksum. */
    static uint8_t calcChecksum(const uint8_t* data, size_t len);

    /** Compute 16-bit Kermit CRC (polynomial 0x1021). */
    static uint16_t calcCRC16(const uint8_t* data, size_t len);

private:
    static bool readHeader(const uint8_t* data, size_t len, size_t& pos,
                           PRS1ChunkHeader& hdr, size_t& header_end);
};

} // namespace sleeplink::parser

#endif // SLEEPLINK_WITH_PHILIPS
