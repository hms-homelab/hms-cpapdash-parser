#include "cpapdash/parser/AirSense11Pages.h"

#include "ProtoWire.h"

#include <cstring>

namespace cpapdash::parser {

namespace pw = protowire;

namespace {

bool setErr(std::string* err, const char* msg) {
    if (err) *err = msg;
    return false;
}

bool readVarint(const uint8_t* d, size_t n, size_t& i, uint64_t& out) {
    out = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (i >= n) return false;
        const uint8_t b = d[i++];
        out |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

// MSB-first bit reader over the payload after the first two samples.
class Bits {
public:
    Bits(const uint8_t* d, size_t n) : d_(d), nbits_(n * 8) {}
    bool   more() const { return p_ < nbits_; }
    size_t left() const { return nbits_ - p_; }
    int    next() {
        const int b = (d_[p_ >> 3] >> (7 - (p_ & 7))) & 1;
        p_++;
        return b;
    }
private:
    const uint8_t* d_;
    size_t nbits_;
    size_t p_ = 0;
};

AirSense11PageSignal signalOf(uint32_t field) {
    switch (field) {
    case 18: return AirSense11PageSignal::Flow;
    case 19: return AirSense11PageSignal::MaskPressure;
    case 20: return AirSense11PageSignal::Leak;
    case 21: return AirSense11PageSignal::InspiratoryPressure;
    default: return AirSense11PageSignal::Unknown;
    }
}

}  // namespace

double AirSense11PageChunk::value(size_t i) const {
    return raw[i] * AirSense11PagesParser::scale(signal);
}

std::optional<std::vector<int32_t>> AirSense11PagesParser::decodeRc03(const uint8_t* blob,
                                                                      size_t len, size_t n,
                                                                      std::string* err) {
    // Header: H, "RC03", two bytes, two varints, D, E. H counts the bytes after it.
    if (len < 1 || len < static_cast<size_t>(blob[0]) + 1) {
        setErr(err, "blob shorter than its header");
        return std::nullopt;
    }
    const size_t header_end = static_cast<size_t>(blob[0]) + 1;
    if (header_end < 7 || std::memcmp(blob + 1, "RC03", 4) != 0) {
        setErr(err, "not an RC03 blob");
        return std::nullopt;
    }
    size_t i = 7;
    uint64_t lo, hi;
    if (!readVarint(blob, header_end, i, lo) || !readVarint(blob, header_end, i, hi) ||
        i + 2 != header_end) {
        setErr(err, "RC03 header length does not match its fields");
        return std::nullopt;
    }
    const uint8_t d = blob[i];
    if (d < 2 || (d & (d - 1)) != 0) {
        setErr(err, "RC03 Rice byte is not a power of two");
        return std::nullopt;
    }
    int k = -1;   // k = log2(D) - 1: 4 -> 1, 16 -> 3
    for (uint8_t v = d; v > 1; v >>= 1) k++;

    std::vector<int32_t> x;
    if (n == 0) return x;
    const uint8_t* pay = blob + header_end;
    const size_t pay_len = len - header_end;
    if (pay_len < (n > 1 ? 4u : 2u)) {
        setErr(err, "RC03 payload shorter than its first samples");
        return std::nullopt;
    }
    // The bitstream starts after the two int16 slots. A one-sample chunk has
    // not been seen; it is read with or without its second slot.
    const size_t head = pay_len >= 4 ? 4 : pay_len;
    // Each further sample takes at least k + 1 bits: refuse a count the
    // payload cannot hold before allocating for it.
    const size_t nbits = (pay_len - head) * 8;
    if (n > 2 && (n - 2) > nbits / static_cast<size_t>(k + 1)) {
        setErr(err, "RC03 bitstream too short for the sample count");
        return std::nullopt;
    }
    x.reserve(n);
    x.push_back(static_cast<int16_t>(pay[0] | (pay[1] << 8)));
    if (n > 1) x.push_back(static_cast<int16_t>(pay[2] | (pay[3] << 8)));

    Bits bits(pay + head, pay_len - head);
    while (x.size() < n) {
        uint64_t q = 0;
        for (;;) {
            if (!bits.more()) {
                setErr(err, "RC03 bitstream ran out");
                return std::nullopt;
            }
            if (!bits.next()) break;
            q++;
        }
        uint64_t r = 0;
        for (int b = 0; b < k; b++) {
            if (!bits.more()) {
                setErr(err, "RC03 bitstream ran out");
                return std::nullopt;
            }
            r = (r << 1) | static_cast<uint64_t>(bits.next());
        }
        const uint64_t u = (q << k) | r;
        const int64_t res = (u & 1) ? -static_cast<int64_t>((u + 1) >> 1)
                                    : static_cast<int64_t>(u >> 1);
        const size_t m = x.size();
        const int64_t v = 2 * static_cast<int64_t>(x[m - 1]) - x[m - 2] + res;
        if (v > INT32_MAX || v < INT32_MIN) {
            setErr(err, "RC03 sample out of range");
            return std::nullopt;
        }
        x.push_back(static_cast<int32_t>(v));
    }
    // What is left must be 1-padding to the byte.
    if (bits.left() >= 8) {
        setErr(err, "RC03 blob has bytes after its last sample");
        return std::nullopt;
    }
    while (bits.more()) {
        if (!bits.next()) {
            setErr(err, "RC03 padding is not all 1-bits");
            return std::nullopt;
        }
    }
    return x;
}

AirSense11SignalPage AirSense11PagesParser::parseSignalPage(const uint8_t* data, size_t len) {
    AirSense11SignalPage page;
    auto skip = [&page](const std::string& why) {
        if (page.skipped++ == 0) page.first_error = why;
    };

    pw::Reader top(data, len);
    pw::Field rec;
    while (top.next(rec)) {
        if (rec.wire != 2) continue;
        AirSense11PageChunk c;
        c.signal = signalOf(rec.number);

        const uint8_t* body = nullptr;
        size_t body_len = 0;
        pw::Reader outer(rec.bytes, rec.len);
        pw::Field f;
        while (outer.next(f))
            if (f.number == 2 && f.wire == 2) { body = f.bytes; body_len = f.len; }
        if (!body) { skip("record without its field 2"); continue; }

        const uint8_t* blob = nullptr;
        size_t blob_len = 0;
        bool have_iv = false, have_s = false, have_e = false;
        pw::Reader inner(body, body_len);
        while (inner.next(f)) {
            if (f.wire == 0 && f.number == 1) { c.interval_ms = static_cast<int64_t>(f.varint); have_iv = true; }
            else if (f.wire == 0 && f.number == 2) { c.start_ms = static_cast<int64_t>(f.varint); have_s = true; }
            else if (f.wire == 0 && f.number == 3) { c.end_ms = static_cast<int64_t>(f.varint); have_e = true; }
            else if (f.wire == 2 && f.number == 4) { blob = f.bytes; blob_len = f.len; }
        }
        if (!have_iv || !have_s || !have_e || !blob) { skip("record missing a field"); continue; }
        if (c.interval_ms <= 0 || c.end_ms < c.start_ms ||
            (c.end_ms - c.start_ms) % c.interval_ms != 0) {
            skip("record span is not a whole number of intervals");
            continue;
        }
        const size_t n = static_cast<size_t>((c.end_ms - c.start_ms) / c.interval_ms) + 1;
        std::string err;
        auto samples = decodeRc03(blob, blob_len, n, &err);
        if (!samples) { skip(err); continue; }
        c.raw = std::move(*samples);
        page.chunks.push_back(std::move(c));
    }
    return page;
}

std::vector<AirSense11PageEvent> AirSense11PagesParser::parseEventsPage(const uint8_t* data,
                                                                       size_t len) {
    std::vector<AirSense11PageEvent> out;
    pw::Reader top(data, len);
    pw::Field f4;
    while (top.next(f4)) {
        if (f4.number != 4 || f4.wire != 2) continue;
        pw::Reader mid(f4.bytes, f4.len);
        pw::Field f1;
        while (mid.next(f1)) {
            if (f1.number != 1 || f1.wire != 2) continue;
            pw::Reader list(f1.bytes, f1.len);
            pw::Field ev;
            while (list.next(ev)) {
                if (ev.number != 1 || ev.wire != 2) continue;
                AirSense11PageEvent e;
                bool have_type = false, have_onset = false;
                pw::Reader r(ev.bytes, ev.len);
                pw::Field v;
                while (r.next(v)) {
                    if (v.wire != 0) continue;
                    switch (v.number) {
                    case 1: e.type = static_cast<int>(v.varint); have_type = true; break;
                    case 2: e.onset_ms = static_cast<int64_t>(v.varint); have_onset = true; break;
                    case 3: e.field3_ms = static_cast<int64_t>(v.varint); break;
                    case 4: e.duration_ms = static_cast<int64_t>(v.varint); break;
                    default: break;
                    }
                }
                if (have_type && have_onset) out.push_back(e);
            }
        }
    }
    return out;
}

double AirSense11PagesParser::scale(AirSense11PageSignal s) {
    switch (s) {
    case AirSense11PageSignal::Flow:
    case AirSense11PageSignal::Leak: return 0.02;
    case AirSense11PageSignal::MaskPressure:
    case AirSense11PageSignal::InspiratoryPressure: return 0.2;
    default: return 1.0;
    }
}

const char* AirSense11PagesParser::unit(AirSense11PageSignal s) {
    switch (s) {
    case AirSense11PageSignal::Flow:
    case AirSense11PageSignal::Leak: return "L/s";
    case AirSense11PageSignal::MaskPressure:
    case AirSense11PageSignal::InspiratoryPressure: return "cmH2O";
    default: return "";
    }
}

const char* AirSense11PagesParser::eventName(int type) {
    switch (type) {
    case 3: return "Central Apnea";
    case 4: return "Obstructive Apnea";
    default: return "";
    }
}

} // namespace cpapdash::parser
