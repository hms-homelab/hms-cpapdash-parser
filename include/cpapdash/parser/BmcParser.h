#pragma once

#ifdef CPAPDASH_WITH_BMC

#include "cpapdash/parser/ISessionParser.h"
#include "cpapdash/parser/Models.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cpapdash::parser {

// BMC / React Health Luna (G2S family). The format is written up in
// docs/BMC_FORMAT.md, with where every fact comes from; this header names the
// pieces and nothing more.

/** One raw packet is one second. */
constexpr size_t kBmcPacketBytes = 256;
/** Words before the timestamp. */
constexpr size_t kBmcPacketWords = 124;
constexpr uint16_t kBmcSync = 0xAAAA;
/** A word holding this is "not valid" for that second. */
constexpr uint16_t kBmcInvalid = 0xFFFF;

/** Events start after this region (header + fill) of the .evt file. */
constexpr size_t kBmcEventRegionBytes = 2048;
constexpr size_t kBmcEventRecordBytes = 32;

/** More than this between consecutive stamps starts a new session. */
constexpr int kBmcSessionGapSeconds = 60;

/** Word positions this parser reads (docs/BMC_FORMAT.md, "Words"). */
namespace bmc_word {
constexpr size_t kSync          = 0;
constexpr size_t kSessionNumber = 1;
constexpr size_t kEpap          = 2;    // 0.5 cmH2O per unit
constexpr size_t kIpap          = 3;    // 0.5 cmH2O per unit
constexpr size_t kLeak          = 98;   // 0.1 L/min per unit
constexpr size_t kRespRate      = 104;  // breaths/min
constexpr size_t kIeRatio       = 105;  // x10
}  // namespace bmc_word

/** .evt record types whose meaning is confirmed. Everything else is a span of
 *  state, counted in the notes and never emitted as an event. */
namespace bmc_event {
constexpr int kClearAirway = 1;
constexpr int kObstructive = 2;
}  // namespace bmc_event

/**
 * A packet's stamp as plain civil seconds: days since 1970-01-01 times 86400
 * plus the time of day, with NO time zone. Sorting, gap detection and the noon
 * anchor all work on this, so a daylight-saving change cannot fake a gap or
 * reorder two seconds. Only the output is converted to a time_point.
 */
using BmcCivilSeconds = int64_t;

struct BmcPacket {
    BmcCivilSeconds stamp = 0;
    std::array<uint16_t, kBmcPacketWords> words{};
};

/** nullopt when the sync is wrong or the stamp is not a real date and time. */
std::optional<BmcPacket> decodeBmcPacket(const uint8_t* packet);

/** Civil seconds for a date and time; nullopt when it is not one. */
std::optional<BmcCivilSeconds> bmcCivilSeconds(int year, int month, int day, int hour,
                                               int minute, int second);

/** The device's local wall clock for a civil stamp, as every parser here reads one. */
std::chrono::system_clock::time_point bmcToTimePoint(BmcCivilSeconds s);

struct BmcEventRecord {
    int session_number = 0;
    int type = 0;
    uint32_t onset_seconds = 0;     // since noon of the session's sleep day
    uint32_t duration_seconds = 0;
};

/** Every 0xAAAA record after the header region; `refused` counts the rest. */
std::vector<BmcEventRecord> decodeBmcEvents(const uint8_t* data, size_t len,
                                            int* refused = nullptr);

/** Noon of the sleep day a stamp belongs to (the day before when it is before noon). */
BmcCivilSeconds bmcSleepDayNoon(BmcCivilSeconds stamp);

/** One session: consecutive seconds with no gap over kBmcSessionGapSeconds. */
struct BmcSessionSpan {
    BmcCivilSeconds start = 0;
    BmcCivilSeconds end = 0;    // the last packet's second
    int seconds = 0;            // packets in it
    int session_number = 0;     // word 1 of its first packet
};

/** What the last parse found out about the card. */
struct BmcNotes {
    std::string serial;
    int packets = 0;
    int refused_packets = 0;      // wrong sync or no real stamp
    int duplicate_seconds = 0;    // a second already seen; the first is kept
    bool out_of_order = false;    // file order disagreed with time order
    int sessions = 0;
    int refused_event_records = 0;
    int events_emitted = 0;
    int events_outside_session = 0;
    std::map<int, int> span_types;  // type -> records not emitted as events
};

/** A whole card read into memory: packets sorted by time, and its events. */
struct BmcCard {
    std::string serial;
    std::vector<BmcPacket> packets;     // sorted, one per second
    std::vector<BmcEventRecord> events;
    std::vector<BmcSessionSpan> sessions;
    BmcNotes notes;
};

/**
 * Build a card from its raw files (in their numbered order) and its .evt.
 * Sorts, drops duplicate seconds, splits sessions.
 */
BmcCard buildBmcCard(const std::string& serial,
                     const std::vector<std::vector<uint8_t>>& raw_files,
                     const std::vector<uint8_t>& evt_file);

class BmcParser : public ISessionParser {
public:
    /**
     * One session of the card in `session_dir`: the one whose span contains
     * `session_start`, else the one starting nearest it, else (no hint) the
     * latest. A card holds every session in the same files, so the hint is how
     * a caller names the night it wants; listSessions() says which there are.
     */
    std::unique_ptr<ParsedSession> parseSession(
        const std::string& session_dir,
        const std::string& device_id,
        const std::string& device_name,
        std::optional<std::chrono::system_clock::time_point> session_start = std::nullopt
    ) override;

    /** Keys ending in a three-digit extension are raw files, in key order; the
     *  key ending ".evt" is the events. `session_start_str` is
     *  "YYYYMMDD_HHMMSS", or empty for the latest session. */
    std::unique_ptr<ParsedSession> parseSessionFromBuffers(
        const std::map<std::string, std::pair<const uint8_t*, size_t>>& buffers,
        const std::string& device_id,
        const std::string& device_name,
        const std::string& session_start_str = ""
    ) override;

    DeviceManufacturer manufacturer() const override { return DeviceManufacturer::BMC; }

    const BmcNotes& lastNotes() const { return notes_; }

    /** Read a card folder: its serial-named raw files and .evt. */
    static std::optional<BmcCard> readCard(const std::string& dir);

    /** The sessions on the card in `dir`, oldest first. */
    static std::vector<BmcSessionSpan> listSessions(const std::string& dir);

    /** Turn one session of a card into a ParsedSession. */
    static std::unique_ptr<ParsedSession> sessionFromCard(const BmcCard& card,
                                                          const BmcSessionSpan& span,
                                                          const std::string& device_id,
                                                          const std::string& device_name,
                                                          BmcNotes* notes = nullptr);

private:
    std::unique_ptr<ParsedSession> pick(BmcCard card, std::optional<BmcCivilSeconds> hint,
                                        const std::string& device_id,
                                        const std::string& device_name);
    BmcNotes notes_;
};

std::unique_ptr<ISessionParser> createBmcParser();

}  // namespace cpapdash::parser

#endif  // CPAPDASH_WITH_BMC
