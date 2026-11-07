#ifdef CPAPDASH_WITH_PHILIPS

// Chunk container framing + checksums.
// PROVENANCE: docs/PHILIPS_FORMAT.md sections 3.1-3.4 ([A] wiki header table,
// [B] cpap-lib HeaderRecord/DataChunk + CRC.cs @f760b21) and, for
// DataFormatVersion 3, section 9.2 ([C] header block + CRC-32 form, the signal
// entry layout measured on [S]). No OSCAR source.

#include "cpapdash/parser/PhilipsParser.h"
#include <iostream>

namespace cpapdash::parser {

namespace {

// v3 data checksum (doc 9.2): CRC-32, MSB-first, polynomial 0x04C11DB7, initial
// value 0xFFFFFFFF, no final inversion, each data byte fed as the 32-bit word
// 0x000000bb, i.e. the byte sequence 00 00 00 bb.
uint32_t crc32WordFed(const uint8_t* data, size_t len) {
    static const auto kTable = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int b = 0; b < 8; ++b) {
                c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
            }
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    auto feed = [&](uint8_t byte) {
        crc = (crc << 8) ^ kTable[((crc >> 24) ^ byte) & 0xFFu];
    };
    for (size_t i = 0; i < len; ++i) {
        feed(0); feed(0); feed(0); feed(data[i]);
    }
    return crc;
}

// CRC-16/Kermit (reflected CCITT, poly 0x8408, init 0, no final xor).
// Computed bitwise; equivalent to the table in doc 3.3.
uint16_t crc16Kermit(const uint8_t* data, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0x8408)
                            : static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

// 8-bit additive checksum over the header bytes (doc 3.3).
uint8_t checksum8(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum = static_cast<uint8_t>(sum + data[i]);
    return sum;
}

uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

constexpr size_t STD_HEADER_LEN = 15;

} // namespace

uint32_t PhilipsParser::crc32v3(const uint8_t* data, size_t len) {
    return crc32WordFed(data, len);
}

std::vector<PhilipsChunk> PhilipsParser::readChunks(const uint8_t* data, size_t len,
                                                    PhilipsParseWarnings& warnings) {
    std::vector<PhilipsChunk> chunks;
    if (data == nullptr) return chunks;

    size_t pos = 0;
    while (pos + STD_HEADER_LEN + 1 + 2 <= len) {  // header + hdr checksum + data CRC
        const uint8_t* h = data + pos;

        PhilipsChunk chunk;
        chunk.data_format_version = h[0];
        chunk.block_length        = readU16(h + 1);
        chunk.file_type           = h[3];
        chunk.family              = h[4];
        chunk.family_version      = h[5];
        chunk.file_extension      = h[6];
        chunk.session_number      = readU32(h + 7);
        chunk.timestamp_raw       = readU32(h + 11);
        chunk.timestamp = std::chrono::system_clock::time_point(
            std::chrono::seconds(chunk.timestamp_raw));

        // Whole-block sanity: BlockLength includes header + checksums (doc 3.1).
        if (chunk.block_length < STD_HEADER_LEN + 1 + 2 ||
            pos + chunk.block_length > len) {
            warnings.truncated_chunks++;
            break;
        }

        // DataFormatVersion 2 (doc 3) and 3 (doc 9.2); anything else is skipped.
        const bool v3 = chunk.data_format_version == 3;
        if (chunk.data_format_version != 2 && !v3) {
            warnings.unsupported_chunks++;
            pos += chunk.block_length;
            continue;
        }
        const size_t data_crc_len = v3 ? 4 : 2;

        size_t header_len = STD_HEADER_LEN;
        if (chunk.file_type == 1) {
            // Signal extra header (doc 3.2). v3 per-signal entries are 4 bytes,
            // v2's are 3 (doc 9.2).
            if (pos + STD_HEADER_LEN + 4 > len) { warnings.truncated_chunks++; break; }
            const uint8_t* s = h + STD_HEADER_LEN;
            chunk.interval_records    = readU16(s);
            chunk.seconds_per_interval = s[2];
            uint8_t nsig = s[3];
            const size_t entry = v3 ? 4 : 3;
            size_t sig_table = 4 + static_cast<size_t>(nsig) * entry + 1;  // + terminator
            if (pos + STD_HEADER_LEN + sig_table > len) { warnings.truncated_chunks++; break; }
            for (uint8_t i = 0; i < nsig; ++i) {
                const uint8_t* e = s + 4 + i * entry;
                PhilipsChunk::WaveformInfo w;
                w.sample_format = e[0];
                w.interleave    = readU16(e + 1);
                if (v3) w.extra = e[3];
                chunk.waveforms.push_back(w);
            }
            header_len += sig_table;
        } else if (chunk.file_type == 0 && v3) {
            // v3 header block (doc 9.2): count N, then N (code, record size) pairs.
            if (pos + STD_HEADER_LEN + 1 > len) { warnings.truncated_chunks++; break; }
            size_t n = h[STD_HEADER_LEN];
            if (pos + STD_HEADER_LEN + 1 + 2 * n > len) { warnings.truncated_chunks++; break; }
            for (size_t i = 0; i < n; ++i) {
                chunk.record_size[h[STD_HEADER_LEN + 1 + 2 * i]] =
                    h[STD_HEADER_LEN + 2 + 2 * i];
            }
            header_len += 1 + 2 * n;
        } else if (chunk.file_type != 0) {
            // Unknown FileType: we cannot know its extra-header size, so we
            // cannot locate the data area. Skip the whole block by length.
            warnings.unsupported_chunks++;
            pos += chunk.block_length;
            continue;
        }

        // Header checksum (doc 3.3): additive-8 over all header bytes.
        if (pos + header_len + 1 > len) { warnings.truncated_chunks++; break; }
        uint8_t hdr_sum_stored = data[pos + header_len];
        if (checksum8(h, header_len) != hdr_sum_stored) {
            // Corrupt header: BlockLength itself is untrustworthy; stop clean.
            warnings.truncated_chunks++;
            break;
        }

        // Data area (doc 3.4 / 9.2): BlockLength - header - 1 (hdr sum) - CRC.
        if (chunk.block_length < header_len + 1 + data_crc_len) {
            warnings.truncated_chunks++;
            break;
        }
        size_t data_size = chunk.block_length - header_len - 1 - data_crc_len;
        size_t data_pos = pos + header_len + 1;
        if (data_pos + data_size + data_crc_len > len) { warnings.truncated_chunks++; break; }

        const bool crc_ok = v3
            ? readU32(data + data_pos + data_size) == crc32WordFed(data + data_pos, data_size)
            : readU16(data + data_pos + data_size) == crc16Kermit(data + data_pos, data_size);
        if (!crc_ok) {
            warnings.crc_failures++;
            pos += chunk.block_length;
            continue;  // skip this chunk, keep the session (doc 3.3 / SDD)
        }

        chunk.data.assign(data + data_pos, data + data_pos + data_size);
        chunks.push_back(std::move(chunk));
        pos += chunks.back().block_length;
    }

    return chunks;
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_PHILIPS
