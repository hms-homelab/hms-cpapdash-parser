#ifdef CPAPDASH_WITH_SEFAM

#include "cpapdash/parser/SefamParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>

namespace cpapdash::parser {

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    return true;
}

int twoDigits(const std::string& s, size_t at) {
    return (s[at] - '0') * 10 + (s[at + 1] - '0');
}

// Local wall-clock, matching SefamIni.cpp and EDFFile::getStartTime(). The
// device stamps its own face's time.
std::optional<std::chrono::system_clock::time_point>
makeLocalTime(int year, int month, int day, int hour, int min, int sec) {
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
    if (hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) return std::nullopt;

    std::tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = min;
    t.tm_sec  = sec;
    t.tm_isdst = -1;

    std::time_t epoch = std::mktime(&t);
    if (epoch == static_cast<std::time_t>(-1)) return std::nullopt;
    return std::chrono::system_clock::from_time_t(epoch);
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // anonymous namespace

// ── The 38-byte file header ─────────────────────────────────────────────────

SefamFileHeader SefamParser::decodeFileHeader(const std::vector<uint8_t>& raw) {
    SefamFileHeader out;
    if (raw.size() < kSefamFileHeaderBytes) return out;

    std::string text;
    text.reserve(kSefamFileHeaderBytes);
    for (size_t i = 0; i < kSefamFileHeaderBytes; ++i) {
        const char c = static_cast<char>(raw[i] ^ kSefamHeaderXor);
        if (c < 0x20 || c > 0x7E) return out;  // not the header we know
        text.push_back(c);
    }
    out.text = text;

    // "#02/<identity>/<YYMMDDhhmmss>/"
    const size_t s1 = text.find('/');
    if (s1 == std::string::npos) return out;
    const size_t s2 = text.find('/', s1 + 1);
    if (s2 == std::string::npos) return out;
    const size_t s3 = text.find('/', s2 + 1);
    if (s3 == std::string::npos) return out;

    out.record_type = trim(text.substr(0, s1));
    out.identity = trim(text.substr(s1 + 1, s2 - s1 - 1));

    const std::string stamp = trim(text.substr(s2 + 1, s3 - s2 - 1));
    if (stamp.size() == 12 && allDigits(stamp)) {
        out.stamp = makeLocalTime(2000 + twoDigits(stamp, 0), twoDigits(stamp, 2),
                                  twoDigits(stamp, 4), twoDigits(stamp, 6),
                                  twoDigits(stamp, 8), twoDigits(stamp, 10));
    }

    out.valid = !out.identity.empty();
    return out;
}

// ── Block framing ───────────────────────────────────────────────────────────

std::vector<uint8_t> SefamParser::deframe(const std::vector<uint8_t>& raw,
                                          const SefamChannel& channel,
                                          SefamBlockLayout layout,
                                          SefamFraming* framing) {
    std::vector<uint8_t> out;
    SefamFraming f;
    f.layout = layout;

    const int bps = channel.bytesPerSample();
    if (bps <= 0 || channel.freq <= 0 || !layout.valid() ||
        raw.size() <= kSefamFileHeaderBytes) {
        if (framing) *framing = f;
        return out;
    }

    const size_t trailer_bytes = static_cast<size_t>(layout.trailer_bytes);
    const size_t block_bytes = static_cast<size_t>(layout.seconds) *
                               static_cast<size_t>(channel.freq) *
                               static_cast<size_t>(bps);

    size_t at = kSefamFileHeaderBytes;
    bool have_previous = false;
    uint16_t previous = 0;

    while (raw.size() - at > trailer_bytes) {
        // Whatever is left, less the trailer that must follow this block. A full
        // block takes its whole length; the last one takes what remains.
        const size_t available = raw.size() - at - trailer_bytes;
        const size_t take = std::min(block_bytes, available);
        const bool final_partial = take < block_bytes;

        const auto block_begin = raw.begin() + static_cast<long>(at);
        const auto block_end = block_begin + static_cast<long>(take);

        unsigned sum = 0;
        for (auto it = block_begin; it != block_end; ++it) sum += *it;

        const size_t trailer_at = at + take;
        const uint8_t checksum = raw[trailer_at];
        const bool sum_ok = (sum & 0xFF) == checksum;

        // The 1200R firmware writes the checksum alone. Only the wider trailer
        // carries a block index.
        const uint16_t index = layout.hasIndex()
            ? static_cast<uint16_t>((raw[trailer_at + 1] << 8) | raw[trailer_at + 2])
            : 0;

        // A short run at the end of the file is ambiguous: it is either a
        // legitimate final block or a write that was cut off mid-block, and the
        // sizes alone cannot tell them apart. The checksum can. A final partial
        // block that verifies is real; one that does not is a truncated tail,
        // and its samples are dropped rather than half-trusted -- which costs at
        // most ten seconds and never reports a fabricated one.
        if (final_partial && !sum_ok) {
            f.truncated_tail = true;
            break;
        }

        at = trailer_at + trailer_bytes;

        ++f.blocks;
        if (!sum_ok) ++f.checksum_failures;

        // Continuity, not a starting value. Two sessions on the donor card open
        // at index 2 with every checksum correct, so requiring 1 would throw away
        // two good nights.
        if (layout.hasIndex()) {
            if (have_previous && index != static_cast<uint16_t>(previous + 1))
                ++f.index_breaks;
            previous = index;
            have_previous = true;
        }

        out.insert(out.end(), block_begin, block_end);
    }

    f.samples = out.size() / static_cast<size_t>(bps);
    if (framing) *framing = f;
    return out;
}

std::optional<SefamBlockLayout> SefamParser::deduceBlockLayout(
    const std::vector<uint8_t>& raw, const SefamChannel& channel) {
    if (raw.size() <= kSefamFileHeaderBytes || channel.freq <= 0) return std::nullopt;

    // The checksums do the work. Under a wrong layout the block boundaries land
    // in the middle of the data, the trailer bytes are samples, and the sums do
    // not match -- across hundreds of blocks, never by accident.
    //
    // Trailer widths seen: 1 on the 1200R firmware, 3 on the 1263R. The narrower
    // is tried first because a 3-byte trailer cannot masquerade as a 1-byte one
    // (its extra bytes would have to happen to continue the data), whereas a
    // long enough run could in principle do the reverse.
    for (const int trailer : {1, 3}) {
        for (int seconds = 1; seconds <= kSefamMaxBlockSeconds; ++seconds) {
            const SefamBlockLayout layout{seconds, trailer};
            SefamFraming f;
            deframe(raw, channel, layout, &f);

            // One block verifies under any layout long enough to hold it, so a
            // single-block file cannot settle the question and is not asked to.
            if (f.blocks >= 2 && f.checksum_failures == 0 && f.index_breaks == 0 &&
                !f.truncated_tail)
                return layout;
        }
    }

    return std::nullopt;
}

// ── Physical values ─────────────────────────────────────────────────────────

SefamScale SefamParser::scaleFor(const SefamChannel& channel) {
    const std::string name = upper(channel.name);

    // Flow. The offset is measured, not assumed: a night's flow has to integrate
    // to nothing, and the mean code over a 6h24m night is 128.51 against a range
    // of 57..199. The factor is not measured -- nothing in the data distinguishes
    // L/min from a multiple of it -- so it stays 1 and stays flagged.
    if (name == "FLW") return {128.0, 1.0, false, "L/min"};

    // Pressure and leak. A code of 110 is 11.0 cmH2O and a code of 122 is 12.2
    // L/min, which are ordinary therapy numbers; read through the INI's declared
    // range the same night is 110 cmH2O and a 54 L/min average leak, which are
    // not. Round factors picked because the alternatives are impossible.
    if (name == "PRE") return {0.0, 0.1, false, "cmH2O"};
    if (name == "LK")  return {0.0, 0.1, false, "L/min"};

    // Oximetry. Every session on the donor card has these as stubs, so there is
    // nothing to calibrate against and the code is passed through untouched.
    if (name == "SPO") return {0.0, 1.0, false, "%"};
    if (name == "HRT") return {0.0, 1.0, false, "bpm"};

    return {0.0, 1.0, false, ""};
}

std::vector<double> SefamParser::readChannel(const std::vector<uint8_t>& raw,
                                             const SefamChannel& channel,
                                             SefamBlockLayout layout,
                                             SefamFraming* framing) {
    const std::vector<uint8_t> bytes = deframe(raw, channel, layout, framing);

    std::vector<double> out;
    const int bps = channel.bytesPerSample();
    if (bps <= 0 || bps > 4) return out;

    const SefamScale scale = scaleFor(channel);
    const size_t count = bytes.size() / static_cast<size_t>(bps);
    out.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        // Little-endian for a multi-byte sample. Byte order is still UNKNOWN --
        // the donor card's only 16-bit channel, the pulse waveform, is a stub in
        // all 242 sessions -- and no multi-byte channel is surfaced, so nothing
        // rests on this.
        uint32_t v = 0;
        for (int b = 0; b < bps; ++b)
            v |= static_cast<uint32_t>(bytes[i * bps + b]) << (8 * b);
        out.push_back(scale.apply(static_cast<double>(v)));
    }

    return out;
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_SEFAM
