#pragma once

// One read-only AirSense 11 request and its answer, as the Mule & Miner relay
// it (platform SDD-067).
//
// The cloud asks the machine, through the miner, for one stored page
// (StartSpool + PullSpoolFragments) or one Get. The miner answers with an
// "AR01" stream: the four bytes "AR01", then records
//
//     [type u8][len u16 little-endian][len bytes]
//
//   1 SENT     the call as the miner sent it, plaintext JSON
//   2 REPLY    the machine's reply to a call, JSON text as it came (<= 4096 B)
//   3 DATA     one spool fragment's decoded bytes, in seq order (<= 2808 B)
//   4 END      the last SpoolFragment's params without `data`, JSON text:
//              status, seq, and nextSpoolAddress when the machine gave one
//   5 OUTCOME  always last: result u8, end u8, hci_reason u16, rpc_error i32,
//              fragments u16, bytes u32 (all little-endian), then last_error
//              text to the end of the record
//
// Records never depend on transport chunk boundaries: the reader takes bytes
// in any pieces. A stream without OUTCOME was cut, and is read as broken. The
// page bytes (every DATA record, joined) go to AirSense11PagesParser.
//
// The selector list (which spools and ids the cloud may ask for) is data, in
// data/as11_selectors.json (SDD-067 D1); AirSense11Selectors reads it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cpapdash::parser {

enum class AirSense11RecordType : uint8_t {
    Sent = 1,
    Reply = 2,
    Data = 3,
    End = 4,
    Outcome = 5,
};

// How a request ended (SDD-067 §4.5): always an event of the link or the
// machine, never a new timer.
enum class AirSense11RequestEnd : uint8_t {
    Done = 0,            // the Get reply, or a SpoolFragment with a terminal status
    RpcError = 1,        // the reply carried error {code}
    SeqGap = 2,          // a fragment arrived out of order: a notification was lost
    Disconnected = 3,    // BLE_GAP_EVENT_DISCONNECT (hci_reason says why)
    NotAdvertising = 4,  // the scan ended without the paired machine
    ConnectFailed = 5,   // the connect or the GATT discovery failed
    Undecodable = 6,     // a frame did not decrypt or a fragment did not decode
    NoReply = 7,         // the existing per-call bound ran out
    SpoolSilent = 8,     // the existing fragment-silence bound ran out
};

const char* airSense11RequestEndName(uint8_t end);

struct AirSense11RequestOutcome {
    uint8_t  result     = 0;   // the miner's as11_result_t
    uint8_t  end        = 0;   // AirSense11RequestEnd
    uint16_t hci_reason = 0;
    int32_t  rpc_error  = 0;
    uint16_t fragments  = 0;
    uint32_t bytes      = 0;
    std::string last_error;
};

// The END record, read.
struct AirSense11SpoolEnd {
    std::string json;          // the record as it came
    std::string status;        // SPOOL_COMPLETE, SPOOL_COMPLETE_MORE_DATA_PENDING, ...
    int64_t     seq = -1;      // -1: absent
    std::string next_spool;    // nextSpoolAddress' one key, the spool name
    std::string next_from;     // nextSpoolAddress.<name>.fromDateTime, as given

    bool morePending() const { return status == "SPOOL_COMPLETE_MORE_DATA_PENDING"; }
    // The machine has nothing further for this spool from here.
    bool finished() const { return status == "SPOOL_COMPLETE" || status == "ERROR_DATA_UNAVAILABLE"; }
};

struct AirSense11Request {
    std::vector<std::string> sent;      // SENT records, in order
    std::vector<std::string> replies;   // REPLY records, in order
    std::vector<uint8_t>     page;      // every DATA record, joined in order
    size_t                   data_records = 0;
    std::optional<AirSense11SpoolEnd>       end;
    std::optional<AirSense11RequestOutcome> outcome;
    // Why the stream is not whole, when it is not: "" for a whole stream.
    std::string broken;

    // The request ended as the machine meant (OUTCOME end Done) and the stream
    // is whole. A page that is not complete keeps what arrived (SDD-067 D5).
    bool complete() const {
        return broken.empty() && outcome && outcome->end == static_cast<uint8_t>(AirSense11RequestEnd::Done);
    }
};

// Reads an AR01 stream given in any pieces.
class AirSense11RequestReader {
public:
    // Takes the next bytes. False once the stream is known bad (no AR01
    // magic, a record after OUTCOME, an OUTCOME too short, an unknown type);
    // the reason is in result().broken and later bytes are ignored.
    bool feed(const uint8_t* data, size_t len);
    bool feed(const std::vector<uint8_t>& data) { return feed(data.data(), data.size()); }

    // The stream so far, closed: a record still open, or no OUTCOME, is read
    // as a cut stream (broken set), keeping every whole record before it.
    AirSense11Request finish() const;

private:
    std::vector<uint8_t> pending_;   // bytes not yet a whole record
    bool magic_seen_ = false;
    bool after_outcome_ = false;
    AirSense11Request req_;
    void fail(const std::string& why);
};

// The whole stream at once.
AirSense11Request decodeAirSense11Request(const uint8_t* data, size_t len);
inline AirSense11Request decodeAirSense11Request(const std::vector<uint8_t>& data) {
    return decodeAirSense11Request(data.data(), data.size());
}

// Builds an AR01 stream: for tests and for any C++ device that answers one.
class AirSense11RequestWriter {
public:
    AirSense11RequestWriter();
    // A record; false (and nothing written) when len does not fit u16.
    bool record(AirSense11RecordType type, const uint8_t* data, size_t len);
    bool record(AirSense11RecordType type, const std::string& text) {
        return record(type, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }
    bool outcome(const AirSense11RequestOutcome& o);
    const std::vector<uint8_t>& bytes() const { return out_; }

private:
    std::vector<uint8_t> out_;
};

// The selector list (SDD-067 D1): the spools and Get ids the cloud may ask
// for. Read from data/as11_selectors.json.
struct AirSense11Selectors {
    std::vector<std::string> spool;
    std::vector<std::string> get;

    bool allowsSpool(const std::string& name) const;
    bool allowsGet(const std::string& id) const;

    // {"spool": [names...], "get": [names...]}. nullopt when the text is not
    // that shape or a name is not a valid selector.
    static std::optional<AirSense11Selectors> parse(const std::string& json);
};

// A selector as the firmware accepts it: 1 to 47 of [A-Za-z0-9_-].
bool isAirSense11SelectorName(const std::string& name);

}  // namespace cpapdash::parser
