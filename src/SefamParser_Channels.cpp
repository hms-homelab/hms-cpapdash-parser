#ifdef CPAPDASH_WITH_SEFAM

#include "cpapdash/parser/SefamParser.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cpapdash::parser {

namespace {

// Bytes per second a channel writes, which is what relates a file's size to the
// span of time it covers.
double bytesPerSecond(const SefamChannel& c) {
    return static_cast<double>(c.bytesPerSample()) * static_cast<double>(c.freq);
}

// A header longer than this in front of a stream of one-byte samples is not a
// header. The cap only bounds the search; it is not a claim about the format.
constexpr size_t kMaxHeaderBytes = 65536;

// How far apart two channels' spans may be and still be called the same
// recording. A channel can stop a sample or two short of its neighbours; a
// second of slack absorbs that without letting a genuinely different length
// through.
constexpr double kSpanToleranceSeconds = 1.0;

// Enough of a file to judge a scrambling candidate by. A minute of 25 Hz flow is
// 1500 bytes; this is well past the point where more samples change the answer,
// and it keeps a 256-key sweep cheap on a card holding hours of data.
constexpr size_t kScoringBytes = 16384;

// Mean absolute difference between neighbouring bytes, after XOR with `key`.
//
// Flow, pressure and leak are physical signals sampled tens of times a second,
// so consecutive samples sit close together. XOR against the wrong key does not
// preserve that: flipping a high bit turns a step of 1 into a step of 128, and
// the average step size climbs.
double roughnessUnderKey(const std::vector<std::vector<uint8_t>>& buffers, uint8_t key) {
    double total = 0;
    size_t steps = 0;

    for (const auto& buf : buffers) {
        const size_t n = std::min(buf.size(), kScoringBytes);
        if (n < 2) continue;
        for (size_t i = 1; i < n; ++i) {
            const int a = static_cast<uint8_t>(buf[i - 1] ^ key);
            const int b = static_cast<uint8_t>(buf[i] ^ key);
            total += std::abs(b - a);
            ++steps;
        }
    }

    return steps ? total / static_cast<double>(steps) : 0.0;
}

// A stretch long enough that only an idle channel explains it, and the byte it
// repeats. Thirty-two samples is over a second on the slowest channel we expect
// and a fraction of a second on the fastest, which is far longer than a real
// signal holds perfectly still and far shorter than an idle stretch runs.
constexpr size_t kIdleRunBytes = 32;

std::optional<uint8_t> longestIdleRunValue(const std::vector<std::vector<uint8_t>>& buffers) {
    size_t best_len = 0;
    uint8_t best_val = 0;

    for (const auto& buf : buffers) {
        if (buf.empty()) continue;
        size_t run = 1;
        for (size_t i = 1; i <= buf.size(); ++i) {
            if (i < buf.size() && buf[i] == buf[i - 1]) { ++run; continue; }
            if (run > best_len) { best_len = run; best_val = buf[i - 1]; }
            run = 1;
        }
    }

    if (best_len < kIdleRunBytes) return std::nullopt;
    return best_val;
}

} // anonymous namespace

// ── Where the samples start ─────────────────────────────────────────────────

std::optional<size_t> SefamParser::deduceHeaderLength(
    const SefamIni& ini,
    const std::map<std::string, size_t>& file_sizes)
{
    struct Entry { const SefamChannel* ch; size_t size; };
    std::vector<Entry> entries;

    for (const auto& ch : ini.channels) {
        auto it = file_sizes.find(ch.name);
        if (it == file_sizes.end()) continue;
        entries.push_back({&ch, it->second});
    }
    if (entries.empty()) return std::nullopt;

    const size_t min_size = std::min_element(
        entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b) { return a.size < b.size; })->size;

    const size_t limit = std::min(min_size, kMaxHeaderBytes);

    // Search rather than solve.
    //
    // The channels of one session cover one span of wall-clock time, at whatever
    // rate each of them runs. So the right header length is the one that makes
    // every file agree on how long the recording was, and a wrong one makes them
    // disagree -- by more, the further off it is, because the channels divide by
    // different rates.
    //
    // Two derivations fall out of that without being written down separately. A
    // pair of channels at different rates pins the length arithmetically, and a
    // channel that was declared but never recorded is a file of exactly the
    // header length, whose payload goes to zero at the right answer and to a
    // nonsense one-sample recording anywhere else.
    size_t best_h = 0;
    double best_spread = std::numeric_limits<double>::infinity();
    double runner_up = std::numeric_limits<double>::infinity();
    bool found = false;

    for (size_t h = 0; h <= limit; ++h) {
        bool usable = true;
        double lo = std::numeric_limits<double>::infinity();
        double hi = -std::numeric_limits<double>::infinity();
        size_t populated = 0;
        double first_rate = 0;
        bool two_rates = false;

        for (const auto& e : entries) {
            const size_t payload = e.size - h;  // h <= min_size, so never negative
            const int bps = e.ch->bytesPerSample();
            if (bps <= 0 || payload % static_cast<size_t>(bps) != 0) { usable = false; break; }
            if (payload == 0) continue;  // a channel that was never recorded
            if (e.ch->freq <= 0) { usable = false; break; }

            const double span = static_cast<double>(payload / static_cast<size_t>(bps)) /
                                e.ch->freq;
            lo = std::min(lo, span);
            hi = std::max(hi, span);

            const double rate = bytesPerSecond(*e.ch);
            if (populated == 0) first_rate = rate;
            else if (std::abs(rate - first_rate) > 1e-9) two_rates = true;
            ++populated;
        }
        if (!usable) continue;

        // Nothing is learned from channels that cannot check each other. One
        // populated channel agrees with itself at every offset, and so does a
        // set that all run at the same rate -- they shift together. Only two
        // different rates constrain the answer, because only they disagree about
        // how long the recording was when the offset is wrong.
        //
        // This is also what stops a large offset from looking perfect by
        // declaring every channel but one to be a stub.
        if (populated == 1 || (populated > 1 && !two_rates)) continue;

        // Every file exactly the header length: consistent, and a session with
        // nothing in it. parseSession is what rejects that, on the honest ground
        // that there is no data rather than that it could not be read.
        const double spread = populated == 0 ? 0.0 : hi - lo;

        if (spread < best_spread) {
            runner_up = best_spread;
            best_spread = spread;
            best_h = h;
            found = true;
        } else if (spread < runner_up) {
            runner_up = spread;
        }
    }

    // Two things have to hold. The winner must actually reconcile the files, and
    // it must be the only length that does -- a single populated channel agrees
    // with itself at every offset, and picking one of those would be a guess
    // that shifts every sample in the night while looking perfectly plausible.
    if (!found) return std::nullopt;
    if (best_spread > kSpanToleranceSeconds) return std::nullopt;
    if (!(best_spread < runner_up)) return std::nullopt;

    return best_h;
}

// ── How the bytes are scrambled ─────────────────────────────────────────────

SefamDescrambler SefamParser::deduceDescrambler(
    const std::vector<std::vector<uint8_t>>& samples)
{
    SefamDescrambler out;
    out.identity_roughness = roughnessUnderKey(samples, 0);
    out.roughness = out.identity_roughness;

    size_t usable = 0;
    for (const auto& b : samples) if (b.size() >= 2) ++usable;
    if (usable == 0) return out;  // nothing to judge; identity stands

    double best = out.identity_roughness;
    int best_key = 0;
    for (int key = 1; key <= 255; ++key) {
        const double r = roughnessUnderKey(samples, static_cast<uint8_t>(key));
        if (r < best) { best = r; best_key = key; }
    }

    // Smoothness narrows the key to two, and never to one.
    //
    // XOR by 0xFF is x -> 255 - x, which mirrors a signal without changing the
    // distance between neighbouring samples at all. So a key and its complement
    // score IDENTICALLY, always, and no amount of smoothing evidence separates
    // them. Reading a card upside down is not a near miss: a pressure of 10
    // becomes 15.5, and it still looks like breathing.
    //
    // An idle stretch is what settles it. A channel that recorded nothing
    // recorded zero, so the byte that stretch repeats IS the key, outright.
    const uint8_t complement = static_cast<uint8_t>(best_key ^ 0xFF);
    const auto idle = longestIdleRunValue(samples);

    if (idle && (*idle == static_cast<uint8_t>(best_key) || *idle == complement)) {
        best_key = *idle;
        best = roughnessUnderKey(samples, *idle);
        out.confirmed_by_idle_run = true;
    } else {
        // Either there is no idle stretch to read, or it names a key that the
        // smoothness search does not, which is what a repeating multi-byte key
        // would look like through a single-byte search. Both are "we do not
        // know", and the caller is told so rather than handed a coin flip.
        out.ambiguous = true;
    }

    out.kind = best_key == 0 ? SefamDescrambler::Kind::Identity
                             : SefamDescrambler::Kind::SingleByteXor;
    out.key = static_cast<uint8_t>(best_key);
    out.roughness = best;
    return out;
}

// ── Decoding one channel ────────────────────────────────────────────────────

std::vector<double> SefamParser::decodeChannel(
    const std::vector<uint8_t>& raw,
    size_t header,
    const SefamChannel& channel,
    const SefamDescrambler& descrambler)
{
    std::vector<double> out;
    if (raw.size() <= header) return out;

    const int bps = channel.bytesPerSample();
    if (bps <= 0 || bps > 4) return out;

    const size_t count = (raw.size() - header) / static_cast<size_t>(bps);
    out.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const size_t at = header + i * static_cast<size_t>(bps);

        // Little-endian for a multi-byte sample. Byte order is UNKNOWN and no
        // multi-byte channel is surfaced in v1 (SDD-005 section 4), so nothing
        // downstream rests on this choice; it is here so the reader is complete
        // rather than half-written.
        uint32_t v = 0;
        for (int b = 0; b < bps; ++b)
            v |= static_cast<uint32_t>(descrambler.apply(raw[at + b])) << (8 * b);

        out.push_back(channel.toPhysical(v));
    }

    return out;
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_SEFAM
