#ifdef CPAPDASH_WITH_SEFAM

#include "cpapdash/parser/SefamParser.h"
#include "ParseNum.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>

namespace cpapdash::parser {

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Two-digit years have to land in a century somewhere. A CPAP card is a recent
// object, so the usual POSIX window applies: 69 and below is the 2000s.
int expandYear(int y) {
    if (y >= 1000) return y;
    return y <= 69 ? 2000 + y : 1900 + y;
}

// Local wall-clock, deliberately, and not UTC.
//
// The device stamps the recording with the time on its own face, the same as a
// ResMed EDF header does, and EDFFile::getStartTime() reads those through
// std::mktime. A Sefam night and a ResMed night have to land on the same axis for
// a sleep day to mean anything, so this follows that convention rather than the
// oximetry CSV one.
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

// "13/12/25 23:45:50" -- day first, which is what the vendor's own software
// displays (its manual states dates are DD/MM/YYYY). Used only as the fallback
// when [Start Record] is missing, since that section states the six numbers
// separately and leaves nothing to interpret.
std::optional<std::chrono::system_clock::time_point> parseCreateDate(const std::string& raw) {
    int d = 0, mo = 0, y = 0, h = 0, mi = 0, s = 0;
    char c1 = 0, c2 = 0, c3 = 0, c4 = 0;
    std::istringstream in(trim(raw));
    in >> d >> c1 >> mo >> c2 >> y >> h >> c3 >> mi >> c4 >> s;
    if (in.fail()) return std::nullopt;
    if (c1 != '/' || c2 != '/' || c3 != ':' || c4 != ':') return std::nullopt;
    return makeLocalTime(expandYear(y), mo, d, h, mi, s);
}

// A [ChanN] section header: the literal "chan" and then digits, nothing else.
std::optional<int> channelIndexOf(const std::string& section_lower) {
    if (section_lower.rfind("chan", 0) != 0) return std::nullopt;
    if (section_lower.size() <= 4) return std::nullopt;
    for (size_t i = 4; i < section_lower.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(section_lower[i]))) return std::nullopt;
    return parseIntOr(section_lower.substr(4), -1);
}

using Section = std::vector<std::pair<std::string, std::string>>;  // key(lower) -> value

const std::string* lookup(const Section& s, const std::string& key_lower) {
    for (const auto& kv : s)
        if (kv.first == key_lower) return &kv.second;
    return nullptr;
}

std::string get(const Section& s, const std::string& key_lower) {
    const std::string* v = lookup(s, key_lower);
    return v ? *v : std::string();
}

} // anonymous namespace

// ── SefamIni ────────────────────────────────────────────────────────────────

const SefamChannel* SefamIni::find(const std::string& name) const {
    const std::string want = lower(name);
    for (const auto& c : channels)
        if (lower(c.name) == want) return &c;
    return nullptr;
}

// ── The reader ──────────────────────────────────────────────────────────────

SefamIni SefamParser::parseIniText(const std::string& text) {
    SefamIni ini;

    std::vector<std::pair<std::string, Section>> sections;  // name(lower) -> keys
    Section* current = nullptr;

    std::istringstream in(text);
    std::string line;
    bool first_line = true;

    while (std::getline(in, line)) {
        if (first_line) {
            first_line = false;
            // Strip a UTF-8 BOM. The file is written by a Windows toolchain and
            // a leading BOM would otherwise become part of the first section name.
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }

        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;

        if (t.front() == '[' && t.back() == ']') {
            std::string name = lower(trim(t.substr(1, t.size() - 2)));
            sections.emplace_back(name, Section{});
            current = &sections.back().second;
            continue;
        }

        auto eq = t.find('=');
        if (eq == std::string::npos || current == nullptr) continue;

        // Values keep their spacing and punctuation: a firmware string reads
        // "VER :A020400" and the colon is part of it, not a separator.
        current->emplace_back(lower(trim(t.substr(0, eq))), trim(t.substr(eq + 1)));
    }

    if (sections.empty()) return ini;

    for (const auto& [name, keys] : sections) {
        if (name == "create info") {
            ini.created_by    = get(keys, "created by");
            ini.serial_number = get(keys, "serial number");
            ini.firmware      = get(keys, "version");
            if (const std::string* d = lookup(keys, "date"))
                ini.start = parseCreateDate(*d);
            continue;
        }

        if (name == "oximeter") {
            ini.oximeter_type = get(keys, "type");
            continue;
        }

        if (name == "start record") {
            const int year  = expandYear(parseIntOr(get(keys, "year"), -1));
            const int month = parseIntOr(get(keys, "month"), -1);
            const int day   = parseIntOr(get(keys, "day"), -1);
            const int hour  = parseIntOr(get(keys, "hour"), -1);
            const int min   = parseIntOr(get(keys, "min"), -1);
            const int sec   = parseIntOr(get(keys, "sec"), -1);

            // [Start Record] states the six numbers outright, so where it is
            // present it beats the [Create Info] date string.
            if (auto ts = makeLocalTime(year, month, day, hour, min, sec)) ini.start = ts;

            ini.programmed_duration_s = parseIntOr(get(keys, "programmed record duration"), 0);
            ini.real_duration_s       = parseIntOr(get(keys, "real record duration"), 0);
            continue;
        }

        auto idx = channelIndexOf(name);
        if (!idx) continue;

        SefamChannel ch;
        ch.index       = *idx;
        // The INI's Type= is a per-quantity kind code, distinct from the name:
        // the donor card writes 4 for flow and leak, 11 for pressure, 13 for the
        // several unitless channels, 16 for SpO2 and 17 for heart rate. Carried
        // because it may turn out to be a better key than the name; nothing
        // reads it yet.
        ch.type        = parseIntOr(get(keys, "type"), 0);
        ch.name        = get(keys, "name");
        ch.description = get(keys, "description");
        ch.unit        = get(keys, "unit");
        ch.min         = parseDoubleOr(get(keys, "min"), 0.0);
        ch.max         = parseDoubleOr(get(keys, "max"), 0.0);
        ch.freq        = parseIntOr(get(keys, "freq"), 0);
        ch.bits        = parseIntOr(get(keys, "bit"), 8);

        // A channel with no name has no file to point at, so it is not a channel.
        if (ch.name.empty()) continue;
        ini.channels.push_back(std::move(ch));
    }

    std::sort(ini.channels.begin(), ini.channels.end(),
              [](const SefamChannel& a, const SefamChannel& b) { return a.index < b.index; });

    // An INI with no channels describes no data, whatever else it declares.
    ini.valid = !ini.channels.empty();
    return ini;
}

SefamIni SefamParser::parseIni(const std::string& filepath) {
    std::ifstream f(filepath, std::ios::binary);
    if (!f.is_open()) return SefamIni{};
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseIniText(text);
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_SEFAM
