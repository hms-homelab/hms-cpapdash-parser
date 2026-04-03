#include "sleeplink/parser/PhilipsChunk.h"

#ifdef SLEEPLINK_WITH_PHILIPS

#include <cstring>

namespace sleeplink::parser {

uint8_t PRS1ChunkReader::calcChecksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return sum;
}

uint16_t PRS1ChunkReader::calcCRC16(const uint8_t* data, size_t len) {
    // Kermit CRC-16 (polynomial 0x8408, reflected 0x1021)
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x8408;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

bool PRS1ChunkReader::readHeader(const uint8_t* data, size_t len, size_t& pos,
                                  PRS1ChunkHeader& hdr, size_t& header_end) {
    if (pos + 15 > len) return false;  // minimum header size

    size_t start = pos;

    hdr.file_version = data[pos++];
    if (hdr.file_version != 2 && hdr.file_version != 3) return false;

    hdr.block_size = data[pos] | (data[pos + 1] << 8);
    pos += 2;

    hdr.htype = data[pos++];
    hdr.family = data[pos++];
    hdr.family_version = data[pos++];
    hdr.ext = data[pos++];

    hdr.session_id = data[pos] | (data[pos + 1] << 8) |
                     (data[pos + 2] << 16) | (data[pos + 3] << 24);
    pos += 4;

    hdr.timestamp = data[pos] | (data[pos + 1] << 8) |
                    (data[pos + 2] << 16) | (data[pos + 3] << 24);
    pos += 4;

    if (hdr.htype == 0) {
        // Normal header
        if (hdr.file_version == 3) {
            // V3 extended header: hdb_len pairs of (key, value)
            if (pos >= len) return false;
            uint8_t hdb_len = data[pos++];

            for (uint8_t i = 0; i < hdb_len; ++i) {
                if (pos + 2 > len) return false;
                uint8_t code = data[pos++];
                // Value is actually a size (int16_t) for event data length
                int16_t size = static_cast<int16_t>(data[pos] | (data[pos + 1] << 8));
                pos += 2;
                hdr.hblock[code] = size;
            }
        }

        // Checksum byte
        if (pos >= len) return false;
        uint8_t expected_checksum = data[pos++];
        uint8_t actual_checksum = calcChecksum(data + start, pos - start - 1);
        if (expected_checksum != actual_checksum) return false;

    } else if (hdr.htype == 1) {
        // Waveform header
        if (pos + 4 > len) return false;

        hdr.interval_count = data[pos] | (data[pos + 1] << 8);
        pos += 2;
        hdr.interval_seconds = data[pos++];
        hdr.duration = hdr.interval_count * hdr.interval_seconds;

        if (hdr.file_version == 3) {
            // V3 waveform also has hblock entries
            if (pos >= len) return false;
            uint8_t hdb_len = data[pos++];
            for (uint8_t i = 0; i < hdb_len; ++i) {
                if (pos + 2 > len) return false;
                uint8_t code = data[pos++];
                int16_t size = static_cast<int16_t>(data[pos] | (data[pos + 1] << 8));
                pos += 2;
                hdr.hblock[code] = size;
            }
        }

        // Checksum
        if (pos >= len) return false;
        uint8_t expected_checksum = data[pos++];
        uint8_t actual_checksum = calcChecksum(data + start, pos - start - 1);
        if (expected_checksum != actual_checksum) return false;
    } else {
        return false;  // Unknown header type
    }

    header_end = pos;
    return true;
}

std::vector<std::pair<PRS1ChunkHeader, std::vector<uint8_t>>>
PRS1ChunkReader::parseChunks(const uint8_t* data, size_t len) {
    std::vector<std::pair<PRS1ChunkHeader, std::vector<uint8_t>>> chunks;

    size_t pos = 0;
    while (pos < len) {
        PRS1ChunkHeader hdr;
        size_t header_end = 0;

        if (!readHeader(data, len, pos, hdr, header_end)) break;

        // blockSize is the data portion size (after header)
        size_t data_start = header_end;
        size_t data_end = data_start + hdr.block_size;

        if (data_end + 2 > len) break;  // +2 for CRC-16

        std::vector<uint8_t> chunk_data(data + data_start, data + data_end);

        // Skip CRC-16 validation for now — need to track chunk start offset
        // properly. Will validate against real DS2 files when sample data arrives.

        chunks.emplace_back(std::move(hdr), std::move(chunk_data));

        pos = data_end + 2;  // skip past CRC
    }

    return chunks;
}

} // namespace sleeplink::parser

#endif // SLEEPLINK_WITH_PHILIPS
