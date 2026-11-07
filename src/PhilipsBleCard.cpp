#ifdef CPAPDASH_WITH_PHILIPS

// A DreamStation 2 card tree, or sessions, from the plaintext Bluetooth records.
// PROVENANCE: docs/PHILIPS_FORMAT.md 9.2 (container, CRC) and 9.10 (card tree,
// LOG.SEQ, PROP.BIN), measured on a real card [S]. No OSCAR source.

#include "cpapdash/parser/PhilipsBleCard.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

namespace cpapdash::parser {

namespace fs = std::filesystem;

namespace {

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint32_t getU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string hex8(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

// The blocks of one record, checked: every block a v3 FileType 0 block that
// passes its header sum and data CRC, tiling the body exactly; streams 1 and 2
// carry the record's session and their stream as the file extension. "" = good.
std::string checkRecord(const Ds2BleRecord& r, std::vector<PhilipsChunk>& chunks) {
    const std::string what = "stream " + std::to_string(r.stream) +
                             (r.stream <= 2 ? " session " + std::to_string(r.session) : "");
    if (r.body.empty()) return what + ": empty body";
    PhilipsParseWarnings w;
    chunks = PhilipsParser::readChunks(r.body.data(), r.body.size(), w);
    if (w.total() > 0) {
        return what + ": a block fails its checks (crc=" + std::to_string(w.crc_failures) +
               " truncated=" + std::to_string(w.truncated_chunks) +
               " unsupported=" + std::to_string(w.unsupported_chunks) + ")";
    }
    size_t covered = 0;
    for (const auto& c : chunks) {
        covered += c.block_length;
        if (c.data_format_version != 3 || c.file_type != 0)
            return what + ": not a v3 record block";
        if (c.file_extension != r.stream)
            return what + ": block says file extension " + std::to_string(c.file_extension);
        if (r.stream <= 2 && c.session_number != r.session)
            return what + ": block says session " + std::to_string(c.session_number);
    }
    if (chunks.empty() || covered != r.body.size())
        return what + ": blocks do not fill the body";
    return "";
}

bool validFolderName(const std::string& s) {
    return !s.empty() && s.size() <= 64 &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) != 0; });
}

} // namespace

// ── Card-level files ────────────────────────────────────────────────────────

const std::vector<std::string>& Ds2CardWriter::propKeyOrder() {
    static const std::vector<std::string> kOrder = {
        "CF", "SN", "MN", "PT", "DF", "DFV", "F", "FV", "SV", "FD", "LD",
        "FN", "PFN", "EFN", "DFN", "SID", "BK", "SK", "EK", "DK", "TS", "DC"};
    return kOrder;
}

std::vector<uint8_t> Ds2CardWriter::buildLogSeq(uint32_t d_files, uint32_t e_files,
                                                uint32_t p_files, uint32_t p_folder,
                                                const std::map<int, Ds2Bookmark>& bookmarks) {
    std::vector<uint8_t> out;
    out.reserve(74);
    for (uint32_t n : {d_files, e_files, p_files, p_folder}) {
        putU32(out, n);
        out.push_back(0x5a);
    }
    for (int s = 0; s < 5; ++s) {
        auto it = bookmarks.find(s);
        if (it != bookmarks.end()) out.insert(out.end(), it->second.begin(), it->second.end());
        else out.insert(out.end(), 10, 0);
    }
    putU32(out, PhilipsParser::crc32v3(out.data(), out.size()));
    return out;
}

std::string Ds2CardWriter::bookmarkToProp(const Ds2Bookmark& b) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08x%02x%02x%08x", getU32(b.data()), b[4], b[5],
                  getU32(b.data() + 6));
    return buf;
}

std::vector<uint8_t> Ds2CardWriter::buildPropBin(const std::map<std::string, std::string>& values) {
    std::string text;
    for (const auto& key : propKeyOrder()) {
        auto it = values.find(key);
        text += key + "=" + (it != values.end() ? it->second : std::string()) + "\r\n";
    }
    text += "VC=";
    std::vector<uint8_t> out(text.begin(), text.end());
    char vc[16];
    std::snprintf(vc, sizeof(vc), "0x%08X", PhilipsParser::crc32v3(out.data(), out.size()));
    out.insert(out.end(), vc, vc + std::char_traits<char>::length(vc));
    return out;
}

// ── The tree ────────────────────────────────────────────────────────────────

Ds2CardTree Ds2CardWriter::build(const std::vector<Ds2BleRecord>& records,
                                 const Ds2MachineIdentity& identity,
                                 const Ds2CardOptions& options) {
    Ds2CardTree tree;
    auto fail = [&](const std::string& why) {
        tree.files.clear();
        tree.error = why;
        return tree;
    };

    if (!validFolderName(options.machine_folder))
        return fail("machine_folder must be letters and digits, got \"" +
                    options.machine_folder + "\"");
    if (identity.serial.empty() || identity.model.empty() || identity.firmware.empty())
        return fail("identity needs serial, model and firmware");

    const std::string root = "P-SERIES/" + options.machine_folder + "/";
    tree.session_dir = root + "P0";

    uint32_t d_files = 0, e_files = 0, p_files = 0;
    std::optional<uint32_t> log_time;
    std::optional<PhilipsChunk> first_block;   // F, FV, DFV come from a block header

    for (const auto& r : records) {
        if (r.stream < 1 || r.stream > 4)
            return fail("stream " + std::to_string(r.stream) + ": no card file is known for it");
        std::vector<PhilipsChunk> chunks;
        const std::string bad = checkRecord(r, chunks);
        if (!bad.empty()) return fail(bad);
        if (!first_block) first_block = chunks.front();

        std::string rel;
        if (r.stream <= 2) {
            rel = root + "P0/" + hex8(r.session) + ".B0" + std::to_string(r.stream);
            ++p_files;
        } else {
            uint32_t& n = r.stream == 3 ? d_files : e_files;
            char name[16];
            std::snprintf(name, sizeof(name), "%03u.B0%u", n, static_cast<unsigned>(r.stream));
            rel = root + (r.stream == 3 ? "D/" : "E/") + name;
            ++n;
            if (!log_time) log_time = chunks.front().timestamp_raw;
        }
        if (tree.files.count(rel)) return fail("two records for " + rel);
        tree.files[rel] = r.body;
    }
    if (p_files == 0) return fail("no stream 1 or 2 record");
    if (p_files > kMaxFilesPerFolder)
        return fail(std::to_string(p_files) + " files for P0; the rollover past " +
                    std::to_string(kMaxFilesPerFolder) + " is not measured");

    const std::optional<uint32_t> ld = options.written_at ? options.written_at : log_time;
    if (!ld) return fail("no D or E log record to date the card: set written_at");

    std::map<int, Ds2Bookmark> bookmarks;
    for (int s = 1; s <= 4; ++s) {
        auto it = options.bookmarks.find(s);
        if (it != options.bookmarks.end()) {
            bookmarks[s] = it->second;
        } else {
            tree.warnings.push_back("no stream " + std::to_string(s) +
                                    " bookmark: LOG.SEQ and PROP.BIN carry zeros for it");
        }
    }

    tree.files[root + "LOG.SEQ"] = buildLogSeq(d_files, e_files, p_files, 0, bookmarks);

    std::map<std::string, std::string> prop;
    const std::set<std::string> from_records = {"SN", "MN", "SV", "F", "FV", "DFV", "LD", "FN",
                                                "PFN", "EFN", "DFN", "BK", "SK", "EK", "DK"};
    for (const auto& [k, v] : options.prop_constants) {
        if (!from_records.count(k)) prop[k] = v;
    }
    for (const auto& key : propKeyOrder()) {
        if (!from_records.count(key) && !prop.count(key))
            tree.warnings.push_back("PROP.BIN " + key + " not given: written empty");
    }
    prop["SN"] = identity.serial;
    prop["MN"] = identity.model;
    prop["SV"] = identity.firmware;
    prop["F"] = std::to_string(first_block->family);
    prop["FV"] = std::to_string(first_block->family_version);
    prop["DFV"] = std::to_string(first_block->data_format_version);
    prop["LD"] = std::to_string(*ld);
    prop["FN"] = "0";
    prop["PFN"] = std::to_string(p_files);
    prop["EFN"] = std::to_string(e_files);
    prop["DFN"] = std::to_string(d_files);
    const Ds2Bookmark zero{};
    auto bm = [&](int s) {
        auto it = bookmarks.find(s);
        return bookmarkToProp(it != bookmarks.end() ? it->second : zero);
    };
    prop["BK"] = bookmarkToProp(zero);
    prop["SK"] = bm(2);
    prop["DK"] = bm(3);
    prop["EK"] = bm(4);
    tree.files[root + "PROP.BIN"] = buildPropBin(prop);

    const std::string last = options.machine_folder;
    tree.files["P-SERIES/LAST.TXT"] = std::vector<uint8_t>(last.begin(), last.end());
    return tree;
}

bool Ds2CardWriter::write(const Ds2CardTree& tree, const std::string& card_root,
                          std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!tree.ok()) return fail(tree.error);
    for (const auto& [rel, bytes] : tree.files) {
        const fs::path path = fs::path(card_root) / rel;
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) return fail(path.parent_path().string() + ": " + ec.message());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) return fail(path.string() + ": write failed");
    }
    return true;
}

// ── Direct path ─────────────────────────────────────────────────────────────

std::vector<std::unique_ptr<ParsedSession>> parseDs2BleRecords(
    const std::vector<Ds2BleRecord>& records,
    const Ds2MachineIdentity& identity,
    const std::string& device_id,
    const std::string& device_name,
    PhilipsParseWarnings* warnings) {
    PhilipsParseWarnings local;
    PhilipsParseWarnings& w = warnings ? *warnings : local;

    // session -> (summary chunks, events chunks), a byte-identical repeat dropped
    // as parseSession does (doc 9.3).
    std::map<uint32_t, std::pair<std::vector<PhilipsChunk>, std::vector<PhilipsChunk>>> sessions;
    std::optional<PhilipsChunk> first_block;
    for (const auto& r : records) {
        if (r.stream != 1 && r.stream != 2) continue;
        std::vector<PhilipsChunk> chunks;
        if (!checkRecord(r, chunks).empty()) {
            ++w.unsupported_chunks;   // one per rejected record, whatever the reason
            continue;
        }
        if (!first_block) first_block = chunks.front();
        auto& slot = r.stream == 1 ? sessions[r.session].first : sessions[r.session].second;
        for (auto& c : chunks) {
            const bool duplicate = std::any_of(slot.begin(), slot.end(), [&](const PhilipsChunk& k) {
                return k.timestamp_raw == c.timestamp_raw && k.block_length == c.block_length &&
                       k.data == c.data;
            });
            if (!duplicate) slot.push_back(std::move(c));
        }
    }

    PhilipsDeviceInfo info;
    info.serial_number = identity.serial;
    info.model_number = identity.model;
    info.software_version = identity.firmware;
    if (first_block) {
        info.family = first_block->family;
        info.family_version = first_block->family_version;
        info.data_format_version = std::to_string(first_block->data_format_version);
    }
    info.patient_folder_num = 0;

    std::vector<std::unique_ptr<ParsedSession>> out;
    for (const auto& [number, kinds] : sessions) {
        PhilipsParseWarnings session_warnings;
        auto s = PhilipsParser::sessionFromChunks(kinds.first, kinds.second, {}, &info, device_id,
                                                  device_name, session_warnings);
        w.crc_failures += session_warnings.crc_failures;
        w.truncated_chunks += session_warnings.truncated_chunks;
        w.unknown_event_codes += session_warnings.unknown_event_codes;
        w.unknown_record_codes += session_warnings.unknown_record_codes;
        w.unsupported_chunks += session_warnings.unsupported_chunks;
        if (s) out.push_back(std::move(s));
    }
    return out;
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_PHILIPS
