#pragma once

// The recording format of the Wellue O2Ring-S (model T8520).
//
// The O2Ring-S does not write the .vld v3 files VLDParser reads. As documented
// in github.com/nglessner/o2ring-s-protocol ("Format A"), a recording is:
//
//   header   10 bytes  01 03 00 00 00 00 00 00 04 00
//   records   3 bytes  spo2, heart rate, flags; one per SECOND
//                        spo2 0         = no reading
//                        heart rate 0   = no reading, 255 = no finger contact
//                        flags non-zero = the sample is suspect
//   trailer  48 bytes  the ring's own session statistics, present once the
//                      recording is finalised; its sub-magic 48 12 5a da sits
//                      at (size - 44)
//
// The start time is not in the bytes. It is the file's name, YYYYMMDDhhmmss,
// in the wall clock the ring was set to, read here exactly as VLDParser reads
// a .vld's header time: as digits, with no zone applied.
//
// The result is the same OximetrySession VLDParser returns, so a consumer
// stores an O2Ring-S night with the code that stores a .vld night.

#include "cpapdash/parser/VLDParser.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace cpapdash::parser {

class O2RingSParser {
public:
    // The header bytes this format starts with.
    static bool looksLike(const uint8_t* data, size_t len);

    // A finalised recording: the trailer's sub-magic is where it belongs. A
    // file that reached its size without it is still being written and should
    // be read again later.
    static bool isComplete(const uint8_t* data, size_t len);

    // The recording, or nullopt when the bytes are not this format or the
    // name carries no YYYYMMDDhhmmss start time. `filename` may be a bare
    // name or a path, with or without an extension (".o2s").
    static std::optional<OximetrySession> parse(const uint8_t* data, size_t len,
                                                const std::string& filename);

    static constexpr size_t HEADER_SIZE  = 10;
    static constexpr size_t RECORD_SIZE  = 3;
    static constexpr size_t TRAILER_SIZE = 48;
};

// Which oximeter format a file's bytes are.
enum class OximetryFormat { Unknown, Vld3, O2RingS };
OximetryFormat detectOximetryFormat(const uint8_t* data, size_t len);

// One entry point for every ring recording: the format is read from the
// bytes, not the name, and the matching reader parses it.
std::optional<OximetrySession> parseOximetryFile(const uint8_t* data, size_t len,
                                                 const std::string& filename);

} // namespace cpapdash::parser
