// One read-only AirSense 11 request and its answer (platform SDD-067). See
// AirSense11Request.h for the AR01 stream.

#include "cpapdash/parser/AirSense11Request.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace cpapdash::parser {

namespace {

constexpr uint8_t kMagic[4] = {'A', 'R', '0', '1'};
constexpr size_t  kRecordHeader = 3;
constexpr size_t  kOutcomeFixed = 1 + 1 + 2 + 4 + 2 + 4;   // before last_error

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void wr16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(v & 0xFF); o.push_back(v >> 8); }
void wr32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; i++) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// ── a bounded reader for the few JSON values the END record carries ──
// The machine's END params are a flat object plus one nested
// nextSpoolAddress {name: {fromDateTime: ..}}. This finds a key's value
// without a JSON library; it never reads past the text.

size_t skipWs(const std::string& s, size_t i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    return i;
}

// The position just after `"key"` followed by ':' (whitespace allowed), or npos.
size_t valueAfterKey(const std::string& s, const std::string& key, size_t from = 0) {
    const std::string quoted = "\"" + key + "\"";
    for (size_t at = s.find(quoted, from); at != std::string::npos; at = s.find(quoted, at + 1)) {
        size_t i = skipWs(s, at + quoted.size());
        if (i < s.size() && s[i] == ':') return skipWs(s, i + 1);
    }
    return std::string::npos;
}

// A JSON string starting at i (at its opening quote); escapes \" and \\ kept simple.
std::optional<std::string> stringAt(const std::string& s, size_t i, size_t* after = nullptr) {
    if (i >= s.size() || s[i] != '"') return std::nullopt;
    std::string out;
    for (size_t j = i + 1; j < s.size(); j++) {
        if (s[j] == '\\' && j + 1 < s.size()) { out.push_back(s[++j]); continue; }
        if (s[j] == '"') { if (after) *after = j + 1; return out; }
        out.push_back(s[j]);
    }
    return std::nullopt;
}

std::optional<int64_t> intAt(const std::string& s, size_t i) {
    size_t j = i;
    if (j < s.size() && s[j] == '-') j++;
    if (j >= s.size() || !std::isdigit(static_cast<unsigned char>(s[j]))) return std::nullopt;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) j++;
    try { return std::stoll(s.substr(i, j - i)); } catch (...) { return std::nullopt; }
}

AirSense11SpoolEnd readEnd(const std::string& json) {
    AirSense11SpoolEnd e;
    e.json = json;
    if (size_t v = valueAfterKey(json, "status"); v != std::string::npos)
        if (auto s = stringAt(json, v)) e.status = *s;
    if (size_t v = valueAfterKey(json, "seq"); v != std::string::npos)
        if (auto n = intAt(json, v)) e.seq = *n;
    if (size_t v = valueAfterKey(json, "nextSpoolAddress"); v != std::string::npos && v < json.size() &&
                                                            json[v] == '{') {
        size_t after = 0;
        if (auto name = stringAt(json, skipWs(json, v + 1), &after)) {
            e.next_spool = *name;
            if (size_t f = valueAfterKey(json, "fromDateTime", after); f != std::string::npos)
                if (auto from = stringAt(json, f)) e.next_from = *from;
        }
    }
    return e;
}

}  // namespace

const char* airSense11RequestEndName(uint8_t end) {
    switch (static_cast<AirSense11RequestEnd>(end)) {
    case AirSense11RequestEnd::Done:           return "done";
    case AirSense11RequestEnd::RpcError:       return "rpc_error";
    case AirSense11RequestEnd::SeqGap:         return "seq_gap";
    case AirSense11RequestEnd::Disconnected:   return "disconnected";
    case AirSense11RequestEnd::NotAdvertising: return "not_advertising";
    case AirSense11RequestEnd::ConnectFailed:  return "connect_failed";
    case AirSense11RequestEnd::Undecodable:    return "undecodable";
    case AirSense11RequestEnd::NoReply:        return "no_reply";
    case AirSense11RequestEnd::SpoolSilent:    return "spool_silent";
    }
    return "unknown";
}

// ── reader ──

void AirSense11RequestReader::fail(const std::string& why) {
    if (req_.broken.empty()) req_.broken = why;
}

bool AirSense11RequestReader::feed(const uint8_t* data, size_t len) {
    if (!req_.broken.empty()) return false;
    pending_.insert(pending_.end(), data, data + len);

    if (!magic_seen_) {
        if (pending_.size() < sizeof(kMagic)) return true;
        if (std::memcmp(pending_.data(), kMagic, sizeof(kMagic)) != 0) {
            fail("not an AR01 stream");
            return false;
        }
        pending_.erase(pending_.begin(), pending_.begin() + sizeof(kMagic));
        magic_seen_ = true;
    }

    size_t at = 0;
    while (pending_.size() - at >= kRecordHeader) {
        const uint8_t  type = pending_[at];
        const uint16_t rlen = rd16(&pending_[at + 1]);
        if (pending_.size() - at - kRecordHeader < rlen) break;   // the rest comes later
        const uint8_t* body = &pending_[at + kRecordHeader];
        if (after_outcome_) { fail("a record after OUTCOME"); return false; }

        switch (static_cast<AirSense11RecordType>(type)) {
        case AirSense11RecordType::Sent:
            req_.sent.emplace_back(reinterpret_cast<const char*>(body), rlen);
            break;
        case AirSense11RecordType::Reply:
            req_.replies.emplace_back(reinterpret_cast<const char*>(body), rlen);
            break;
        case AirSense11RecordType::Data:
            req_.page.insert(req_.page.end(), body, body + rlen);
            req_.data_records++;
            break;
        case AirSense11RecordType::End:
            req_.end = readEnd(std::string(reinterpret_cast<const char*>(body), rlen));
            break;
        case AirSense11RecordType::Outcome: {
            if (rlen < kOutcomeFixed) { fail("OUTCOME too short"); return false; }
            AirSense11RequestOutcome o;
            o.result     = body[0];
            o.end        = body[1];
            o.hci_reason = rd16(body + 2);
            o.rpc_error  = static_cast<int32_t>(rd32(body + 4));
            o.fragments  = rd16(body + 8);
            o.bytes      = rd32(body + 10);
            o.last_error.assign(reinterpret_cast<const char*>(body + kOutcomeFixed), rlen - kOutcomeFixed);
            req_.outcome = o;
            after_outcome_ = true;
            break;
        }
        default:
            fail("unknown record type " + std::to_string(type));
            return false;
        }
        at += kRecordHeader + rlen;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(at));
    return true;
}

AirSense11Request AirSense11RequestReader::finish() const {
    AirSense11Request r = req_;
    if (!r.broken.empty()) return r;
    if (!magic_seen_)       r.broken = "not an AR01 stream";
    else if (!pending_.empty()) r.broken = "cut inside a record";
    else if (!r.outcome)    r.broken = "cut before OUTCOME";
    return r;
}

AirSense11Request decodeAirSense11Request(const uint8_t* data, size_t len) {
    AirSense11RequestReader r;
    r.feed(data, len);
    return r.finish();
}

// ── writer ──

AirSense11RequestWriter::AirSense11RequestWriter() : out_(kMagic, kMagic + sizeof(kMagic)) {}

bool AirSense11RequestWriter::record(AirSense11RecordType type, const uint8_t* data, size_t len) {
    if (len > 0xFFFF) return false;
    out_.push_back(static_cast<uint8_t>(type));
    wr16(out_, static_cast<uint16_t>(len));
    out_.insert(out_.end(), data, data + len);
    return true;
}

bool AirSense11RequestWriter::outcome(const AirSense11RequestOutcome& o) {
    std::vector<uint8_t> b;
    b.push_back(o.result);
    b.push_back(o.end);
    wr16(b, o.hci_reason);
    wr32(b, static_cast<uint32_t>(o.rpc_error));
    wr16(b, o.fragments);
    wr32(b, o.bytes);
    b.insert(b.end(), o.last_error.begin(), o.last_error.end());
    return record(AirSense11RecordType::Outcome, b.data(), b.size());
}

// ── selectors ──

bool isAirSense11SelectorName(const std::string& name) {
    if (name.empty() || name.size() > 47) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
    });
}

bool AirSense11Selectors::allowsSpool(const std::string& name) const {
    return std::find(spool.begin(), spool.end(), name) != spool.end();
}

bool AirSense11Selectors::allowsGet(const std::string& id) const {
    return std::find(get.begin(), get.end(), id) != get.end();
}

namespace {

// The array of strings that is key's value, or nullopt when it is not one.
std::optional<std::vector<std::string>> stringArray(const std::string& s, const std::string& key) {
    size_t i = valueAfterKey(s, key);
    if (i == std::string::npos || i >= s.size() || s[i] != '[') return std::nullopt;
    std::vector<std::string> out;
    i = skipWs(s, i + 1);
    if (i < s.size() && s[i] == ']') return out;
    while (i < s.size()) {
        size_t after = 0;
        auto v = stringAt(s, i, &after);
        if (!v) return std::nullopt;
        out.push_back(*v);
        i = skipWs(s, after);
        if (i < s.size() && s[i] == ',') { i = skipWs(s, i + 1); continue; }
        if (i < s.size() && s[i] == ']') return out;
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

std::optional<AirSense11Selectors> AirSense11Selectors::parse(const std::string& json) {
    auto spools = stringArray(json, "spool");
    auto gets   = stringArray(json, "get");
    if (!spools || !gets) return std::nullopt;
    for (const auto& n : *spools) if (!isAirSense11SelectorName(n)) return std::nullopt;
    for (const auto& n : *gets)   if (!isAirSense11SelectorName(n)) return std::nullopt;
    AirSense11Selectors sel;
    sel.spool = std::move(*spools);
    sel.get   = std::move(*gets);
    return sel;
}

}  // namespace cpapdash::parser
