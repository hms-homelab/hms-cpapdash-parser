#include "cpapdash/parser/EdfRecordCount.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace cpapdash::parser {

namespace {

bool endsWith(const std::string& s, const char* tail) {
    const std::size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

/// An EDF numeric header field: ASCII, left-justified, space-padded. Only the
/// bytes inside [len] are read, and only within [avail].
bool readInt(const char* buf, std::size_t avail, std::size_t off, std::size_t len, long& out) {
    if (len == 0 || len > 80 || off + len > avail) return false;
    const std::string s(buf + off, len);
    try {
        std::size_t used = 0;
        out = std::stol(s, &used);
        return used > 0;
    } catch (...) {
        return false;
    }
}

}  // namespace

bool isResmedSignalEdf(const std::string& name) {
    std::string tail = name;
    std::transform(tail.begin(), tail.end(), tail.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return endsWith(tail, "_brp.edf") || endsWith(tail, "_pld.edf") || endsWith(tail, "_sad.edf") ||
           endsWith(tail, "_sa2.edf") || endsWith(tail, "_tcv.edf");
}

std::size_t edfHeaderBytes(const char* buf, std::size_t buf_len) {
    long header_bytes = 0;
    if (!buf || !readInt(buf, buf_len, 184, 8, header_bytes) || header_bytes <= 256) return 0;
    return static_cast<std::size_t>(header_bytes);
}

bool repairEdfDataRecords(char* buf, std::size_t buf_len, std::uint64_t file_size) {
    if (!buf || buf_len < 256) return false;
    long header_bytes = 0;
    if (!readInt(buf, buf_len, 184, 8, header_bytes) || header_bytes <= 256) return false;
    if (static_cast<std::uint64_t>(header_bytes) > buf_len) return false;  // header not all here
    long ns = 0;
    if (!readInt(buf, buf_len, 252, 4, ns) || ns <= 0) return false;
    // Signal headers precede the samples-per-record block at 256 + ns*216.
    const std::size_t spr_off = 256 + static_cast<std::size_t>(ns) * 216;
    if (spr_off + static_cast<std::size_t>(ns) * 8 > static_cast<std::size_t>(header_bytes))
        return false;
    long rec_bytes = 0;
    for (long i = 0; i < ns; ++i) {
        long spr = 0;
        if (!readInt(buf, buf_len, spr_off + static_cast<std::size_t>(i) * 8, 8, spr) || spr < 0)
            return false;
        rec_bytes += spr * 2;  // EDF samples are 16-bit
    }
    if (rec_bytes <= 0) return false;
    if (file_size <= static_cast<std::uint64_t>(header_bytes)) return false;
    const std::uint64_t data_len = file_size - static_cast<std::uint64_t>(header_bytes);
    if (data_len % static_cast<std::uint64_t>(rec_bytes) != 0) return false;  // not a clean image
    const std::uint64_t records = data_len / static_cast<std::uint64_t>(rec_bytes);
    long stored = 0;
    if (readInt(buf, buf_len, 236, 8, stored) && stored >= 0 &&
        static_cast<std::uint64_t>(stored) == records)
        return false;  // already correct
    const std::string out = std::to_string(records);
    if (out.size() > 8) return false;  // will not fit the field
    std::memcpy(buf + 236, out.data(), out.size());
    std::memset(buf + 236 + out.size(), ' ', 8 - out.size());
    return true;
}

bool repairEdfDataRecords(char* buf, std::size_t n) {
    return repairEdfDataRecords(buf, n, static_cast<std::uint64_t>(n));
}

}  // namespace cpapdash::parser
