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
// Rewritten 2026-09-05 against a real 242-session card. The first cut of this
// parser derived the file layout from first principles because there was no card
// to look at; the card disagreed with it on almost every point that mattered,
// and everything below is now measured rather than reasoned.
//
// A channel file is:
//
//   bytes 0..37   a 38-byte ASCII header, every byte XOR 0xBF, reading
//                 "#02/<model><serial>      /<YYMMDDhhmmss>/"
//   then          the samples, in blocks of ten seconds, each block followed by
//                 a 3-byte trailer: the sum of the block's bytes mod 256, then a
//                 big-endian uint16 block index
//
// The samples themselves are plain. A channel that was declared but never
// recorded is written as the 38-byte header and nothing else.

namespace cpapdash::parser {

/** The fixed header every channel file opens with. */
constexpr size_t kSefamFileHeaderBytes = 38;

/** Its obfuscation. XOR 0xBF turns the header into ASCII; 0x9F becomes a space. */
constexpr uint8_t kSefamHeaderXor = 0xBF;

/** Bound on the block length the search will consider. */
constexpr int kSefamMaxBlockSeconds = 120;

/**
 * How a session's samples are framed.
 *
 * Blocks of a fixed number of seconds -- the same for every channel in the
 * session, whatever each channel's rate -- each followed by a trailer.
 *
 * Neither number is a constant of the format:
 *
 *   S.Box AUTO, 1263R, VER :A020400   10 s blocks, 3-byte trailer
 *                                     [checksum][uint16 BE block index]
 *   SleepBox_AUTO, 1200R, VER :A010300   10 s blocks, 1-byte trailer
 *                                     [checksum], no index at all
 *
 * A parser that hard-codes either reads the other as corrupt, so both are
 * derived per session -- see deduceBlockLayout().
 */
struct SefamBlockLayout {
    int seconds = 0;
    int trailer_bytes = 0;

    bool hasIndex() const { return trailer_bytes >= 3; }
    bool valid() const { return seconds > 0 && trailer_bytes > 0; }
};

/** Each block is followed by [checksum][uint16 big-endian index]. */
constexpr size_t kSefamBlockTrailerBytes = 3;

/** One channel, exactly as the INI declares it. */
struct SefamChannel {
    int index = -1;          // N from [ChanN]
    int type = 0;            // the INI's Type=, a per-quantity kind code
    std::string name;        // "FLW" -- also the file extension carrying its samples
    std::string description;
    std::string unit;        // "lpm", "cmH20", ...
    double min = 0;
    double max = 0;
    int freq = 0;            // Hz
    int bits = 8;

    int bytesPerSample() const { return (bits + 7) / 8; }
};

/**
 * How a channel's raw codes become physical values: `offset` then `factor`.
 *
 * The INI's Min/Max do NOT describe this. It is tempting to read a channel
 * declaring -180..280 in 8 bits as a quantisation of that span, and the first
 * cut of this parser did; the card says otherwise. Flow is centred on the code
 * 128, not on the 99.8 that reading implies, and a pressure channel declaring
 * 0..255 would put a patient on 110 cmH2O. Min/Max look like display or clipping
 * limits, not a scale.
 *
 * See scaleFor() for what each channel actually uses and how much of it is
 * confirmed.
 */
struct SefamScale {
    double offset = 0;   // subtracted from the raw code first
    double factor = 1;   // then multiplied
    bool confirmed = false;
    const char* unit = "";

    double apply(double raw) const { return (raw - offset) * factor; }
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

/** The decoded 38-byte file header. */
struct SefamFileHeader {
    bool valid = false;
    std::string text;         // the whole decoded line, for diagnostics
    std::string record_type;  // "#02"
    std::string identity;     // model code and serial, as the device writes it
    std::optional<std::chrono::system_clock::time_point> stamp;
};

/** What de-framing a channel file found. */
struct SefamFraming {
    SefamBlockLayout layout;
    size_t blocks = 0;
    size_t checksum_failures = 0;
    size_t index_breaks = 0;   // a block index that did not follow its predecessor
    size_t samples = 0;

    // A run at the end of the file too short to be a block and whose checksum
    // does not verify: a write cut off mid-block. Its samples are dropped.
    bool truncated_tail = false;
};

/** What the last parseSession() found out about the card. */
struct SefamSessionNotes {
    SefamFileHeader header;
    std::string stem;              // the session's manifest name, without ".INI"
    SefamBlockLayout layout;       // derived, not assumed
    std::map<std::string, SefamFraming> framing;   // channel name -> framing
    std::vector<std::string> unmapped_channels;    // declared, read, nowhere to put
    std::vector<std::string> undeclared_files;     // present, not in the INI, ignored
    std::vector<std::string> stub_channels;        // declared, never recorded

    // Share of the recording each bit of the detection channel was set for.
    // DET is a bitfield, not an enumeration, and what the bits MEAN is not known,
    // so this is reported rather than turned into events. See SDD-005 section 7.
    std::map<int, double> det_bit_share;
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

    /** What the last parseSession() found out about the card. */
    const SefamSessionNotes& lastNotes() const { return notes_; }

    // ── The steps, public so they can be tested on their own and run by hand
    //    against a card ─────────────────────────────────────────────────────────

    static SefamIni parseIni(const std::string& filepath);
    static SefamIni parseIniText(const std::string& text);

    /**
     * Decode the 38-byte header at the front of a channel file.
     *
     * Every byte XOR 0xBF, which yields printable ASCII in all 242 sessions of
     * the donor card: "#02/1263R24337476       /251110222516/". The identity and
     * the timestamp both agree with the session INI in every one of them, so this
     * is a free cross-check on whether the INI and the data belong together.
     */
    static SefamFileHeader decodeFileHeader(const std::vector<uint8_t>& raw);

    /**
     * Strip the block framing, returning the sample bytes.
     *
     * Samples run in blocks of ten seconds -- 250 samples on a 25 Hz channel, 10
     * on a 1 Hz one -- each followed by three bytes: the sum of that block's
     * bytes mod 256, then a big-endian uint16 index. The final, short block is
     * framed the same way.
     *
     * The checksum is the reason to trust anything this parser says about a
     * Sefam card. Across the donor card's 209,610 flow blocks it verified every
     * single time, so a misread offset or a misread rate cannot pass quietly.
     *
     * The index is checked for continuity but not for its starting value: two
     * sessions on the card begin at 2 rather than 1, with correct checksums
     * throughout, so treating a first index of 1 as a rule would reject two
     * perfectly good nights.
     */
    static std::vector<uint8_t> deframe(const std::vector<uint8_t>& raw,
                                        const SefamChannel& channel,
                                        SefamBlockLayout layout,
                                        SefamFraming* framing = nullptr);

    /**
     * Work out how a session's samples are framed.
     *
     * The checksums make this easy and safe: try each candidate, and the right
     * one is the only one under which every block's trailer describes its block.
     * Under a wrong layout the boundaries land mid-data, the trailer bytes are
     * samples, and the sums do not match -- across hundreds of blocks, never by
     * accident.
     *
     * Two blocks are required before an answer is offered, because a single
     * block verifies under any layout long enough to contain it.
     */
    static std::optional<SefamBlockLayout> deduceBlockLayout(
        const std::vector<uint8_t>& raw, const SefamChannel& channel);

    /**
     * How to read a channel's codes as physical values.
     *
     * NONE of this is confirmed. Every reading below is the one the donor card
     * supports and the alternatives contradict, which is evidence and not proof,
     * and SDD-005's oracle step is what settles them.
     *
     *   PRE  code/10 cmH2O. Mid-session codes sit near 110; a night reads as an
     *        8.35 average peaking at 16.3, an ordinary CPAP prescription, and
     *        across all 241 readable sessions the averages run 0.5 to 12. Read
     *        through the INI's declared 0..255 the same night is 110 cmH2O,
     *        which is not a pressure anyone survives.
     *   LK   code/10 L/min, giving a 9.03 L/min mean. Through the declared
     *        0..153 the same night averages 54 L/min, a leak that would have the
     *        machine alarming all night.
     *   FLW  offset 128, factor 1.
     *
     *        The offset is the weaker of the two claims and worth stating
     *        carefully. Flow is measured at the blower, so it carries the mask's
     *        intentional leak and a night's mean is not expected to be zero --
     *        but it should be small against a signal that swings roughly +/-70.
     *        It is: across the 40 nights of four hours or more, the per-night
     *        mean sits between -14.6 and +14.9 and averages -2.7. Mid-scale is
     *        the right neighbourhood. It is not pinned to the code, and the
     *        first cut of this parser read the declared -180..280 as a
     *        quantisation, which would put the centre at 99.8 instead.
     *
     *        The factor is not evidenced at all: nothing in the data
     *        distinguishes L/min from any multiple of it.
     */
    static SefamScale scaleFor(const SefamChannel& channel);

    /** Codes to physical values, via deframe() and scaleFor(). */
    static std::vector<double> readChannel(const std::vector<uint8_t>& raw,
                                           const SefamChannel& channel,
                                           SefamBlockLayout layout,
                                           SefamFraming* framing = nullptr);

    /**
     * The session manifests in a folder, by filename stem.
     *
     * Two layouts are known, and they disagree about what a folder holds:
     *
     *   DATA_<n>/DATA_<n>.INI          one session per folder (S.Box AUTO, 1263R)
     *   <YYMMDD>/<HHMMSS>.ini          several per folder, one per start time
     *                                  (SleepBox_AUTO, 1200R)
     *
     * Under the second, a night's folder holds every recording that started that
     * day, so the session is the filename stem and not the folder.
     */
    static std::vector<std::string> listSessionStems(const std::string& dir);

    /**
     * Parse one named session out of a folder that may hold several.
     *
     * parseSession() delegates here when a folder holds exactly one manifest,
     * which is every session of the DATA_<n> layout.
     */
    std::unique_ptr<ParsedSession> parseSessionNamed(
        const std::string& session_dir,
        const std::string& stem,
        const std::string& device_id,
        const std::string& device_name,
        std::optional<std::chrono::system_clock::time_point> session_start = std::nullopt);

private:
    SefamSessionNotes notes_;
};

std::unique_ptr<ISessionParser> createSefamParser();

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_SEFAM
