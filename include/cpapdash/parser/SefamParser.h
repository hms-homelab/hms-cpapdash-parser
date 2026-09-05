#pragma once

#ifdef CPAPDASH_WITH_SEFAM

#include "cpapdash/parser/ISessionParser.h"
#include "cpapdash/parser/Models.h"
#include <string>
#include <memory>
#include <optional>
#include <chrono>
#include <map>
#include <vector>
#include <cstdint>

// Sefam S.Box -- see sdd/005-sefam-sbox-support.md and docs/SEFAM_FORMAT.md.
//
// The distinguishing feature of this format is that it describes itself. Every
// session folder carries an INI declaring each channel's name, unit, sample rate,
// bit depth and range, and one binary file per declared channel named by that
// channel. So this parser hard-codes no channel table.
//
// It also hard-codes neither of the two things a channel file will not tell you
// directly: how many bytes of header sit in front of the samples, and how the
// sample bytes are scrambled. Both are DERIVED from the files at parse time
// (deduceHeaderLength, deduceDescrambler). That is partly independence -- the only
// two write-ups of this format that exist are GPL and unlicensed respectively, and
// this library is MIT, so nothing may be transcribed from either -- and partly
// robustness, since a firmware that changes either one still parses.

namespace cpapdash::parser {

/** One channel, exactly as the INI declares it. Nothing here is assumed. */
struct SefamChannel {
    int index = -1;          // N from [ChanN]
    std::string name;        // "FLW" -- also the file extension carrying its samples
    std::string description;
    std::string unit;        // "lpm", "cmH2O", ...
    double min = 0;
    double max = 0;
    int freq = 0;            // Hz
    int bits = 8;

    /** Bytes per sample, from the declared bit depth. */
    int bytesPerSample() const { return (bits + 7) / 8; }

    /**
     * Raw code -> physical value.
     *
     * The declared range routinely exceeds what the declared bit depth can hold
     * one-for-one (a flow channel spanning 460 L/min in 8 bits), so a raw code is
     * a quantisation of [min, max] rather than the value itself. Reading it as a
     * linear map is what makes the declared range mean anything, and it lands on
     * suspiciously clean scale factors for the channels we expect to meet.
     *
     * Where min == 0 and max == 2^bits - 1 this is the identity, which is the
     * degenerate case a channel with nothing to declare will use.
     */
    double toPhysical(uint32_t raw) const;
};

/** Everything the session INI declares. */
struct SefamIni {
    bool valid = false;

    std::string created_by;      // "[Create Info] Created By", the model string
    std::string serial_number;   // as written: the device's own identity string
    std::string firmware;
    std::string oximeter_type;   // "NONE" when nothing was attached

    std::optional<std::chrono::system_clock::time_point> start;
    int programmed_duration_s = 0;
    int real_duration_s = 0;

    std::vector<SefamChannel> channels;

    /** Case-insensitive lookup by channel name; nullptr when absent. */
    const SefamChannel* find(const std::string& name) const;
};

/**
 * How to turn the bytes in a channel file back into samples.
 *
 * Discovered per card, never assumed. The search and its scoring live in
 * deduceDescrambler(); this is just the answer.
 */
struct SefamDescrambler {
    enum class Kind {
        Identity,       // the bytes are already samples
        SingleByteXor   // every byte XORed with one card-wide key
    };

    Kind kind = Kind::Identity;
    uint8_t key = 0;

    // Mean absolute successive difference of the decoded bytes, the score the
    // winning candidate achieved. Lower is smoother, and a physiological signal
    // sampled tens of times a second is smooth. Carried so a caller can tell a
    // confident answer from a marginal one.
    double roughness = 0;

    // Roughness the identity candidate scored, for the same comparison.
    double identity_roughness = 0;

    // Whether a long constant stretch confirmed the key.
    //
    // Smoothness narrows the key to two and never to one: XOR by 0xFF is
    // x -> 255 - x, which mirrors a signal while leaving the distance between
    // neighbouring samples untouched, so a key and its complement score exactly
    // alike. An idle stretch is what settles it, because a channel that recorded
    // nothing recorded zero, so the byte that stretch repeats IS the key.
    bool confirmed_by_idle_run = false;

    // Set when nothing resolved that ambiguity, or when the idle stretch and the
    // smoothness search name different keys -- which is what a repeating
    // multi-byte key would look like through a single-byte search.
    //
    // Reading a card mirrored is not a near miss. A pressure of 10 comes out as
    // 15.5 and still looks like breathing, so a session whose scrambling is
    // ambiguous is refused rather than reported.
    bool ambiguous = false;

    uint8_t apply(uint8_t b) const {
        return kind == Kind::SingleByteXor ? static_cast<uint8_t>(b ^ key) : b;
    }
};

/** What a session folder turned out to contain, beyond the parsed data. */
struct SefamSessionNotes {
    size_t header_bytes = 0;
    SefamDescrambler descrambler;
    std::vector<std::string> unmapped_channels;  // declared, read, nowhere to put
    std::vector<std::string> undeclared_files;   // present, not in the INI, ignored
    int unknown_event_codes = 0;
};

class SefamParser : public ISessionParser {
public:
    std::unique_ptr<ParsedSession> parseSession(
        const std::string& session_dir,
        const std::string& device_id,
        const std::string& device_name,
        std::optional<std::chrono::system_clock::time_point> session_start = std::nullopt
    ) override;

    std::unique_ptr<ParsedSession> parseSessionFromBuffers(
        const std::map<std::string, std::pair<const uint8_t*, size_t>>& buffers,
        const std::string& device_id,
        const std::string& device_name,
        const std::string& session_start_str = ""
    ) override;

    DeviceManufacturer manufacturer() const override {
        return DeviceManufacturer::SEFAM;
    }

    /**
     * What the last parseSession() found out about the card.
     *
     * The header length and the descrambling are discovered rather than known, so
     * how they were resolved is part of the result, not a detail. This is where
     * the donor-card work reads them back, and where an unmapped channel or an
     * unrecognised detection code gets reported.
     */
    const SefamSessionNotes& lastNotes() const { return notes_; }

    // ── The discovery steps, public so they can be tested on their own and run
    //    by hand against a donor card ──────────────────────────────────────────

    static SefamIni parseIni(const std::string& filepath);
    static SefamIni parseIniText(const std::string& text);

    /**
     * How many bytes sit in front of the samples in every channel file.
     *
     * The channels of one session cover one span of wall-clock time, each at its
     * own rate. So the right header length is the one that makes every file
     * agree on how long the recording was, and every wrong one makes them
     * disagree -- by more, the further off it is, because the channels divide by
     * different rates. That is the whole method: try each length, keep the one
     * that reconciles them.
     *
     * A pair of channels at different rates pins the answer arithmetically, and
     * a channel that was declared but never recorded helps too: its file is
     * exactly the header length, so its payload goes to zero at the right answer
     * and to a nonsense one-sample recording anywhere else.
     *
     * Returns nothing unless one length reconciles the files and no other comes
     * close -- a single populated channel agrees with itself at every offset. A
     * header length off by one byte shifts every sample in the night while the
     * result still looks like therapy data, so there is no partial credit.
     *
     * @param file_sizes  channel name -> size in bytes of that channel's file
     */
    static std::optional<size_t> deduceHeaderLength(
        const SefamIni& ini,
        const std::map<std::string, size_t>& file_sizes);

    /**
     * Work out how the sample bytes are scrambled, by trying every candidate and
     * keeping the one that yields the smoothest signal.
     *
     * The declared range cannot score this, because toPhysical() maps any byte
     * into range by construction. Smoothness can: flow at 25 Hz moves a little
     * between neighbouring samples, and XOR with the wrong key turns small steps
     * into large ones wherever it flips a high bit.
     *
     * It gets us to two candidates and no further, because a key and its
     * complement mirror the signal without changing its smoothness at all. An
     * idle stretch chooses between them; without one the result is marked
     * `ambiguous` and the caller must not use it.
     *
     * A card that was never scrambled comes out as identity, which is the honest
     * answer for that card.
     *
     * @param samples  raw bytes after the header, from one or more 8-bit channels
     */
    static SefamDescrambler deduceDescrambler(const std::vector<std::vector<uint8_t>>& samples);

    /**
     * Decode one channel's file into physical values.
     *
     * @param raw     the whole file, header included
     * @param header  bytes to skip, from deduceHeaderLength()
     */
    static std::vector<double> decodeChannel(
        const std::vector<uint8_t>& raw,
        size_t header,
        const SefamChannel& channel,
        const SefamDescrambler& descrambler);

    /**
     * Turn a decoded event-detection channel into events.
     *
     * Each run of one non-zero code becomes one event. Every event is
     * EventType::OTHER carrying its raw code, because what a Sefam detection code
     * MEANS is not known yet -- see SDD-005 section 7. Under SDD-004 that puts
     * them inside total_events and inside no clinical index, so an AHI is never
     * invented from a guess.
     */
    static std::vector<SleepEvent> decodeEvents(
        const std::vector<double>& codes,
        int freq,
        std::chrono::system_clock::time_point session_start,
        int* unknown_code_count = nullptr);

private:
    SefamSessionNotes notes_;
};

std::unique_ptr<ISessionParser> createSefamParser();

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_SEFAM
