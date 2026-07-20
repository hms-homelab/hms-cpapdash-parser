#pragma once
//
// Non-throwing numeric parsing for parser internals.
//
// Every integer field the parsers read comes from a file an uploader supplies
// (EDF headers, device.xml). std::stoi throws on non-numeric input
// (invalid_argument) and on overflow (out_of_range), and cpapdash-api runs
// these parsers on a detached background thread — where an escaping exception
// is a std::terminate of the whole service, not a failed parse. Parse numbers
// through here so malformed input degrades to a fallback instead.
//
// Internal to src/; deliberately not part of the public include/ API.

#include <string>

namespace cpapdash::parser {

// Exactly std::stoi's conversion, minus the throw: `fallback` is returned only
// where stoi would have thrown (no leading digits, or out of int range).
//
// Deliberately keeps stoi's leading-prefix behaviour ("27abc" -> 27, "5.05" ->
// 5) rather than demanding a fully-numeric string. Any value that parses today
// keeps parsing to the same number — this changes behaviour ONLY for input that
// currently crashes.
inline int parseIntOr(const std::string& s, int fallback) {
    try {
        return std::stoi(s);
    } catch (const std::exception&) {
        return fallback;  // invalid_argument (no digits) / out_of_range
    }
}

} // namespace cpapdash::parser
