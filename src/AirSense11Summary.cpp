#include "cpapdash/parser/AirSense11Summary.h"

#include <utility>

namespace cpapdash::parser {

namespace {

using Clock = std::chrono::system_clock;

// ── a minimal protobuf wire reader ──────────────────────────────────────────
//
// Wire types 0 (varint), 1 (64-bit), 2 (length-delimited) and 5 (32-bit). The
// deprecated group types stop the message: nothing here is expected to carry
// one, and reading on past an unknown shape would misfile every later field.

struct Field {
    uint32_t       number = 0;
    uint32_t       wire   = 0;
    uint64_t       varint = 0;        // wire 0, 1 and 5 (as the raw little-endian value)
    const uint8_t* bytes  = nullptr;  // wire 2
    size_t         len    = 0;
};

class Reader {
public:
    Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}

    bool done() const { return i_ >= n_; }

    // The next field, or false at the end or at a shape that cannot be read.
    bool next(Field& f) {
        uint64_t key;
        if (!varint(key)) return false;
        f.number = static_cast<uint32_t>(key >> 3);
        f.wire   = static_cast<uint32_t>(key & 7);
        f.bytes  = nullptr;
        f.len    = 0;
        switch (f.wire) {
        case 0:
            return varint(f.varint);
        case 1:
            return fixed(8, f.varint);
        case 5:
            return fixed(4, f.varint);
        case 2: {
            uint64_t ln;
            if (!varint(ln) || ln > n_ - i_) return fail();
            f.bytes = d_ + i_;
            f.len   = static_cast<size_t>(ln);
            i_ += f.len;
            return true;
        }
        default:
            return fail();
        }
    }

private:
    bool fail() { i_ = n_; return false; }

    bool varint(uint64_t& out) {
        out = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (i_ >= n_) return fail();
            const uint8_t b = d_[i_++];
            out |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
        }
        return fail();   // more than ten bytes: not a varint
    }

    bool fixed(size_t width, uint64_t& out) {
        if (n_ - i_ < width) return fail();
        out = 0;
        for (size_t k = 0; k < width; k++) out |= static_cast<uint64_t>(d_[i_ + k]) << (8 * k);
        i_ += width;
        return true;
    }

    const uint8_t* d_;
    size_t         n_;
    size_t         i_ = 0;
};

Clock::time_point fromMs(uint64_t ms) {
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(
        std::chrono::milliseconds(static_cast<int64_t>(ms))));
}

double hundredths(uint64_t v) { return static_cast<double>(v) * 0.01; }

// A percentile message: {member number: value x 100}.
AirSense11Stats stats(const uint8_t* b, size_t n, bool blower) {
    AirSense11Stats s;
    Reader r(b, n);
    Field f;
    while (r.next(f)) {
        if (f.wire != 0) continue;
        const double v = hundredths(f.varint);
        if (blower) {
            // {1: 5th, 3: 95th}
            if (f.number == 1) s.p5 = v;
            else if (f.number == 3) s.p95 = v;
        } else {
            // {2: 50th, 3: 70th or 95th, 4: 95th or max, 5: max}; leak is the
            // only four-member set, and the caller tells them apart below.
            if (f.number == 2) s.p50 = v;
            else if (f.number == 3) s.p95 = v;
            else if (f.number == 4) s.max = v;
            else if (f.number == 5) s.max = v;
        }
    }
    return s;
}

// Leak has four members: {2: 50, 3: 70, 4: 95, 5: max}.
AirSense11Stats leakStats(const uint8_t* b, size_t n) {
    AirSense11Stats s;
    Reader r(b, n);
    Field f;
    while (r.next(f)) {
        if (f.wire != 0) continue;
        const double v = hundredths(f.varint);
        if (f.number == 2) s.p50 = v;
        else if (f.number == 3) s.p70 = v;
        else if (f.number == 4) s.p95 = v;
        else if (f.number == 5) s.max = v;
    }
    return s;
}

// A single-value message: {2: value x 100}.
std::optional<double> single(const uint8_t* b, size_t n) {
    Reader r(b, n);
    Field f;
    while (r.next(f))
        if (f.wire == 0 && f.number == 2) return hundredths(f.varint);
    return std::nullopt;
}

std::vector<AirSense11Session> sessions(const uint8_t* b, size_t n) {
    std::vector<AirSense11Session> out;
    Reader r(b, n);
    Field f;
    while (r.next(f)) {
        if (f.wire != 2) continue;
        AirSense11Session s;
        Reader sr(f.bytes, f.len);
        Field sf;
        while (sr.next(sf)) {
            if (sf.wire != 0) continue;
            if (sf.number == 1) s.start = fromMs(sf.varint);
            else if (sf.number == 2) s.minutes = static_cast<int>(sf.varint);
        }
        out.push_back(s);
    }
    return out;
}

bool dayRecord(const uint8_t* b, size_t n, AirSense11Day& d) {
    bool has_start = false;
    Reader r(b, n);
    Field f;
    while (r.next(f)) {
        if (f.wire == 0) {
            switch (f.number) {
            case 2:  d.day_start = fromMs(f.varint); has_start = f.varint > 0; break;
            case 3:  d.day_end = fromMs(f.varint); break;
            case 5:  d.usage_minutes = static_cast<int>(f.varint); break;
            case 7:  d.ahi = hundredths(f.varint); break;
            case 8:  d.ai  = hundredths(f.varint); break;
            case 9:  d.hi  = hundredths(f.varint); break;
            case 10: d.oai = hundredths(f.varint); break;
            case 11: d.cai = hundredths(f.varint); break;
            case 12: d.uai = hundredths(f.varint); break;
            case 13: d.rin = hundredths(f.varint); break;
            case 16: d.csr_minutes = hundredths(f.varint); break;
            case 39: d.mask_events = static_cast<int>(f.varint); break;
            case 40: d.updated = fromMs(f.varint); break;
            default: break;
            }
        } else if (f.wire == 2) {
            switch (f.number) {
            case 6:  d.sessions = sessions(f.bytes, f.len); break;
            case 14: d.leak_lps = leakStats(f.bytes, f.len); break;
            case 15: d.target_ipap = stats(f.bytes, f.len, false); break;
            case 20: d.target_epap = stats(f.bytes, f.len, false); break;
            case 21: d.mask_pressure = stats(f.bytes, f.len, false); break;
            case 22: d.tidal_volume_l = stats(f.bytes, f.len, false); break;
            case 23: d.minute_vent = stats(f.bytes, f.len, false); break;
            case 25: d.resp_rate = stats(f.bytes, f.len, false); break;
            case 29: d.ambient_humidity = single(f.bytes, f.len); break;
            case 30: d.humidifier_temp = single(f.bytes, f.len); break;
            case 31: d.heated_tube_temp = single(f.bytes, f.len); break;
            case 32: d.humidifier_power = single(f.bytes, f.len); break;
            case 33: d.heated_tube_power = single(f.bytes, f.len); break;
            case 36: d.blower_pressure = stats(f.bytes, f.len, true); break;
            case 37: d.flow_lps = stats(f.bytes, f.len, true); break;
            case 38: d.blower_flow_lps = single(f.bytes, f.len); break;
            default: break;
            }
        }
    }
    // The indices mean nothing on a day without usage: the machine reports
    // zeros there, which would read as a perfect night.
    if (!d.used()) {
        d.ahi.reset(); d.ai.reset(); d.hi.reset();
        d.oai.reset(); d.cai.reset(); d.uai.reset(); d.rin.reset();
        d.csr_minutes.reset();
    }
    return has_start;
}

double orZero(const std::optional<double>& v) { return v.value_or(0.0); }

}  // namespace

std::vector<AirSense11Day> AirSense11SummaryParser::parse(const uint8_t* data, size_t len) {
    std::vector<AirSense11Day> days;
    if (!data || len == 0) return days;
    Reader r(data, len);
    Field f;
    while (r.next(f)) {
        if (f.wire != 2 || f.number != 2) continue;
        AirSense11Day d;
        if (dayRecord(f.bytes, f.len, d)) days.push_back(std::move(d));
    }
    return days;
}

bool AirSense11SummaryParser::looksLike(const uint8_t* data, size_t len) {
    // Field 2, length-delimited: key byte 0x12. Then the stream has to read
    // as at least one day with a start, which nothing else we store does.
    if (!data || len < 4 || data[0] != 0x12) return false;
    return !parse(data, len).empty();
}

STRDailyRecord AirSense11SummaryParser::toStrRecord(const AirSense11Day& day,
                                                    const std::string& device_id) {
    STRDailyRecord r;
    r.device_id   = device_id;
    r.record_date = day.day_start;
    r.duration_minutes = day.usage_minutes;
    r.mask_events = day.mask_events;
    for (const auto& s : day.sessions)
        r.mask_pairs.emplace_back(s.start, s.start + std::chrono::minutes(s.minutes));

    r.ahi = orZero(day.ahi);
    r.ai  = orZero(day.ai);
    r.hi  = orZero(day.hi);
    r.oai = orZero(day.oai);
    r.cai = orZero(day.cai);
    r.uai = orZero(day.uai);
    r.rin = orZero(day.rin);
    r.csr = orZero(day.csr_minutes);

    r.blow_press_95  = orZero(day.blower_pressure.p95);
    r.blow_press_5   = orZero(day.blower_pressure.p5);
    r.mask_press_50  = orZero(day.mask_pressure.p50);
    r.mask_press_95  = orZero(day.mask_pressure.p95);
    r.mask_press_max = orZero(day.mask_pressure.max);

    // The STR reader hands leak out in L/min (the EDF stores L/s, times 60);
    // the summary carries L/s like the EDF, so the same conversion applies.
    r.leak_50  = orZero(day.leak_lps.p50) * 60.0;
    r.leak_70  = orZero(day.leak_lps.p70) * 60.0;
    r.leak_95  = orZero(day.leak_lps.p95) * 60.0;
    r.leak_max = orZero(day.leak_lps.max) * 60.0;

    r.resp_rate_50  = orZero(day.resp_rate.p50);
    r.resp_rate_95  = orZero(day.resp_rate.p95);
    r.resp_rate_max = orZero(day.resp_rate.max);
    r.tid_vol_50    = orZero(day.tidal_volume_l.p50);
    r.tid_vol_95    = orZero(day.tidal_volume_l.p95);
    r.tid_vol_max   = orZero(day.tidal_volume_l.max);
    r.min_vent_50   = orZero(day.minute_vent.p50);
    r.min_vent_95   = orZero(day.minute_vent.p95);
    r.min_vent_max  = orZero(day.minute_vent.max);

    r.tgt_ipap_50  = day.target_ipap.p50;
    r.tgt_ipap_95  = day.target_ipap.p95;
    r.tgt_ipap_max = day.target_ipap.max;
    r.tgt_epap_50  = day.target_epap.p50;
    r.tgt_epap_95  = day.target_epap.p95;
    r.tgt_epap_max = day.target_epap.max;
    return r;
}

std::vector<STRDailyRecord> AirSense11SummaryParser::toStrRecords(
    const std::vector<AirSense11Day>& days, const std::string& device_id) {
    std::vector<STRDailyRecord> out;
    for (const auto& d : days)
        if (d.used()) out.push_back(toStrRecord(d, device_id));
    return out;
}

} // namespace cpapdash::parser
