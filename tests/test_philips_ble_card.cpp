#ifdef CPAPDASH_WITH_PHILIPS

// DreamStation 2 Bluetooth records -> card tree / sessions (PhilipsBleCard.h).
//
// Every record here is SYNTHETIC: made-up sessions, times and events, built in
// this file from the layouts of docs/PHILIPS_FORMAT.md 9.2 (container), 9.5
// (summary), 9.6 (statistics), 9.8 (events) and 9.10 (card tree). The expected
// values are worked out by hand from those layouts, not read from a parser run.
//
//   256  one long session, every event kind the v3 decoder handles
//   257  three mask-on periods; its events body is TWO blocks
//   258  a 4-second session whose events file holds no event
//   259  a summary with no mask-on (machine switched on and off): no session
//   D/E  one log block each

#include <gtest/gtest.h>
#include "cpapdash/parser/PhilipsBleCard.h"
#include "cpapdash/parser/PhilipsParser.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace cpapdash::parser;
namespace fs = std::filesystem;

namespace {

const Ds2MachineIdentity kIdentity{"TEST0000000001", "520X110C", "V1.0.2.3590"};

constexpr uint32_t T0 = 1893456000;           // 2030-01-01 00:00:00 UTC
constexpr uint32_t T1 = T0 + 86400;           // session 257
constexpr uint32_t T2 = T0 + 2 * 86400 + 3600;  // sessions 258, 259
constexpr uint32_t kLogTime = T2 + 7200;      // the D and E blocks' header time

// ── Building v3 blocks ──────────────────────────────────────────────────────

struct Rec { uint8_t code; std::vector<uint8_t> body; };

std::vector<uint8_t> le16(unsigned v) {
    return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
}

std::vector<uint8_t> cat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// A FileType 0 block (doc 9.2): 15-byte header, (code, size) table in order of
// first use, sum8, the records, CRC-32 of the records.
std::vector<uint8_t> v3Block(uint8_t ext, uint32_t session, uint32_t ts,
                             const std::vector<Rec>& recs) {
    std::vector<std::pair<uint8_t, uint8_t>> table;
    std::vector<uint8_t> data;
    for (const auto& r : recs) {
        auto it = std::find_if(table.begin(), table.end(),
                               [&](const auto& p) { return p.first == r.code; });
        if (it == table.end()) table.emplace_back(r.code, static_cast<uint8_t>(r.body.size()));
        data.push_back(r.code);
        data.insert(data.end(), r.body.begin(), r.body.end());
    }
    std::vector<uint8_t> b = {3, 0, 0, 0, 0, 6, ext};
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(session >> (8 * i)));
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(ts >> (8 * i)));
    b.push_back(static_cast<uint8_t>(table.size()));
    for (const auto& [code, size] : table) { b.push_back(code); b.push_back(size); }
    const size_t len = b.size() + 1 + data.size() + 4;
    b[1] = static_cast<uint8_t>(len);
    b[2] = static_cast<uint8_t>(len >> 8);
    uint8_t sum = 0;
    for (uint8_t c : b) sum = static_cast<uint8_t>(sum + c);
    b.push_back(sum);
    b.insert(b.end(), data.begin(), data.end());
    const uint32_t crc = PhilipsParser::crc32v3(data.data(), data.size());
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(crc >> (8 * i)));
    return b;
}

// Summary records (doc 9.5).
Rec equipmentOn() { return {0x00, {0x00}}; }
Rec settingsAuto() { return {0x01, {0x00, 0x01, 0x02, 0x0d, 0x02, 40, 200}}; }  // auto, 4-20
Rec maskOn(unsigned delta) { return {0x03, cat(le16(delta), {0x00, 0x00})}; }
Rec maskOff(unsigned delta) { return {0x04, le16(delta)}; }
Rec pressure() { return {0x05, {40, 80, 60, 50}}; }
Rec equipmentOff(unsigned delta) { return {0x02, cat(le16(delta), std::vector<uint8_t>(7, 0))}; }
// Statistics (doc 9.6): 15 u16 words + 1 byte. 1 OA, 3 CA, 8 H, 9 H 0x0b, 10 RERA, 11 FL.
Rec stats(int oa, int ca, int h, int h0b, int rera, int fl) {
    std::vector<uint16_t> w(15, 0);
    w[1] = oa; w[3] = ca; w[8] = h; w[9] = h0b; w[10] = rera; w[11] = fl;
    std::vector<uint8_t> b;
    for (auto v : w) b = cat(b, le16(v));
    b.push_back(0);
    return {0x08, b};
}

// Event records (doc 9.8): u16 seconds since the previous record, then fields.
Rec ev(uint8_t code, unsigned delta, uint8_t byte) { return {code, cat(le16(delta), {byte})}; }
Rec leakStat(unsigned delta) { return {0x11, cat(le16(delta), {20, 0, 50})}; }
Rec hypVariant(unsigned delta, uint8_t since) { return {0x0b, cat(le16(delta), {0x00, since})}; }
Rec snoreAtPressure() { return {0x12, {0x00, 50, 0x00, 0x00}}; }   // no time field
Rec pressureNow(unsigned delta) { return {0x01, cat(le16(delta), {40})}; }

std::vector<Ds2BleRecord> syntheticRecords() {
    std::vector<Ds2BleRecord> r;

    // 256: mask on T0+1 to T0+7201 (7,200 s).
    r.push_back({1, 256, v3Block(1, 256, T0, {
        equipmentOn(), settingsAuto(), maskOn(1), maskOff(7200), pressure(),
        stats(2, 1, 3, 1, 2, 1), equipmentOff(30)})});
    r.push_back({2, 256, v3Block(2, 256, T0, {
        leakStat(120),               // clock T0+120
        ev(0x06, 480, 10),           // T0+600   OA, onset T0+590
        ev(0x14, 300, 25),           // T0+900   H 25 s, onset T0+875
        ev(0x05, 600, 5),            // T0+1500  RERA, onset T0+1495
        ev(0x07, 900, 8),            // T0+2400  CA, onset T0+2392
        ev(0x14, 600, 18),           // T0+3000  H 18 s, onset T0+2982
        snoreAtPressure(),           // no time field: the clock must not move
        ev(0x06, 1200, 12),          // T0+4200  OA, onset T0+4188
        hypVariant(300, 20),         // T0+4500  0x0b, OTHER at T0+4480
        ev(0x05, 600, 3),            // T0+5100  RERA, onset T0+5097
        ev(0x0c, 600, 4),            // T0+5700  FL, onset T0+5696
        ev(0x14, 900, 30)})});       // T0+6600  H 30 s, onset T0+6570

    // 257: three periods T1+2..602, T1+722..1022, T1+1082..2282 (2,100 s).
    r.push_back({1, 257, v3Block(1, 257, T1, {
        equipmentOn(), settingsAuto(),
        maskOn(2), maskOff(600), pressure(), stats(1, 0, 0, 0, 0, 0),
        maskOn(120), maskOff(300), pressure(), stats(0, 0, 1, 0, 0, 0),
        maskOn(60), maskOff(1200), pressure(), stats(0, 0, 0, 0, 1, 0),
        equipmentOff(10)})});
    r.push_back({2, 257, cat(
        v3Block(2, 257, T1, {ev(0x06, 302, 0),        // T1+302 OA
                             ev(0x14, 598, 15)}),     // T1+900 H 15 s, onset T1+885
        v3Block(2, 257, T1 + 1800, {ev(0x05, 200, 2)}))});  // T1+2000 RERA, onset T1+1998

    // 258: mask on T2+1 to T2+5 (4 s); events file with a pressure record only.
    r.push_back({1, 258, v3Block(1, 258, T2, {
        equipmentOn(), settingsAuto(), maskOn(1), maskOff(4), pressure(),
        stats(0, 0, 0, 0, 0, 0), equipmentOff(3)})});
    r.push_back({2, 258, v3Block(2, 258, T2, {pressureNow(2)})});

    // 259: switched on and off, no mask-on: written, but no session.
    r.push_back({1, 259, v3Block(1, 259, T2 + 600, {
        equipmentOn(), settingsAuto(), equipmentOff(20)})});

    // The D and E logs, stamped at the read.
    r.push_back({3, 0, v3Block(3, 259, kLogTime, {{0x00, std::vector<uint8_t>(16, 0x11)}})});
    r.push_back({4, 0, v3Block(4, 259, kLogTime, {{0x01, std::vector<uint8_t>(25, 0x22)}})});
    return r;
}

// ── Helpers ─────────────────────────────────────────────────────────────────

std::string toHex(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}

int64_t secs(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

Ds2Bookmark bookmark(uint8_t first) {
    Ds2Bookmark b{};
    for (int i = 0; i < 10; ++i) b[i] = static_cast<uint8_t>(first + i);
    return b;
}

Ds2CardOptions options() {
    Ds2CardOptions o;
    o.machine_folder = "ABCD0000";
    o.bookmarks[1] = bookmark(0x01);
    o.bookmarks[2] = bookmark(0x11);
    return o;
}

fs::path writeTree(const Ds2CardTree& tree, const std::string& tag) {
    const fs::path root = fs::temp_directory_path() / ("ds2ble_test_" + tag);
    fs::remove_all(root);
    std::string err;
    EXPECT_TRUE(Ds2CardWriter::write(tree, root.string(), &err)) << err;
    return root;
}

// Every session of the written tree, through PhilipsParser::parseSession.
std::map<uint32_t, std::unique_ptr<ParsedSession>> parseTree(const fs::path& session_dir) {
    std::map<uint32_t, std::unique_ptr<ParsedSession>> out;
    PhilipsParser parser;
    for (const auto& g : PhilipsParser::discoverSessions(session_dir.string())) {
        std::ifstream f(g.summary_path, std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        PhilipsParseWarnings w;
        auto chunks = PhilipsParser::readChunks(b.data(), b.size(), w);
        if (chunks.empty()) continue;
        auto s = parser.parseSession(session_dir.string(), "dev", "", chunks.front().timestamp);
        if (s && s->session_start &&
            std::abs(secs(*s->session_start) - static_cast<int64_t>(chunks.front().timestamp_raw)) < 600)
            out[g.session_number] = std::move(s);
    }
    return out;
}

using Ev = std::tuple<EventType, int64_t, int>;   // type, onset, duration

std::vector<Ev> events(const ParsedSession& s) {
    std::vector<Ev> out;
    for (const auto& e : s.events)
        out.emplace_back(e.event_type, secs(e.timestamp), static_cast<int>(e.duration_seconds));
    std::sort(out.begin(), out.end(), [](const Ev& a, const Ev& b) {
        return std::get<1>(a) < std::get<1>(b);
    });
    return out;
}

} // namespace

// ── Card-level files ────────────────────────────────────────────────────────

// Expected bytes computed independently of this code from the layout (doc 9.10).
TEST(PhilipsBleCard, LogSeqLayoutAndCrc) {
    std::map<int, Ds2Bookmark> bm = {{1, bookmark(0x01)}, {2, bookmark(0x11)},
                                     {3, bookmark(0x21)}, {4, bookmark(0x31)}};
    auto b = Ds2CardWriter::buildLogSeq(1, 2, 38, 0, bm);
    ASSERT_EQ(b.size(), 74u);
    EXPECT_EQ(toHex(b),
              "010000005a020000005a260000005a000000005a00000000000000000000"
              "0102030405060708090a1112131415161718191a2122232425262728292a"
              "3132333435363738393afe1bc8d0");
}

TEST(PhilipsBleCard, BookmarkAsPropPrintsIt) {
    EXPECT_EQ(Ds2CardWriter::bookmarkToProp(bookmark(0x11)), "0x1413121115161a191817");
    EXPECT_EQ(Ds2CardWriter::bookmarkToProp(Ds2Bookmark{}), "0x00000000000000000000");
}

TEST(PhilipsBleCard, PropBinKeyOrderAndVc) {
    std::map<std::string, std::string> v = {{"SN", "X"}, {"MN", "Y"}};
    auto b = Ds2CardWriter::buildPropBin(v);
    const std::string s(b.begin(), b.end());
    EXPECT_EQ(s.rfind("CF=\r\nSN=X\r\nMN=Y\r\nPT=\r\n", 0), 0u);
    const auto vc = s.rfind("VC=0x");
    ASSERT_NE(vc, std::string::npos);
    EXPECT_EQ(s.size(), vc + 13);   // "VC=0x" + 8 hex digits, no line end
    char expect[16];
    std::snprintf(expect, sizeof(expect), "%08X", PhilipsParser::crc32v3(b.data(), vc + 3));
    EXPECT_EQ(s.substr(vc + 5), expect);
}

// ── The tree ────────────────────────────────────────────────────────────────

TEST(PhilipsBleCard, BuildsTheCardTree) {
    const auto recs = syntheticRecords();
    auto tree = Ds2CardWriter::build(recs, kIdentity, options());
    ASSERT_TRUE(tree.ok()) << tree.error;
    // 7 session files, D, E, LOG.SEQ, PROP.BIN, LAST.TXT
    EXPECT_EQ(tree.files.size(), 12u);
    EXPECT_EQ(tree.session_dir, "P-SERIES/ABCD0000/P0");
    EXPECT_EQ(tree.files.at("P-SERIES/ABCD0000/P0/00000100.B01"), recs[0].body);
    EXPECT_EQ(tree.files.at("P-SERIES/ABCD0000/P0/00000101.B02"), recs[3].body);
    EXPECT_TRUE(tree.files.count("P-SERIES/ABCD0000/P0/00000103.B01"));
    EXPECT_FALSE(tree.files.count("P-SERIES/ABCD0000/P0/00000103.B02"));
    EXPECT_EQ(tree.files.at("P-SERIES/ABCD0000/D/000.B03"), recs[7].body);
    EXPECT_EQ(tree.files.at("P-SERIES/ABCD0000/E/000.B04"), recs[8].body);
    const auto& last = tree.files.at("P-SERIES/LAST.TXT");
    EXPECT_EQ(std::string(last.begin(), last.end()), "ABCD0000");

    // LOG.SEQ: 1 D file, 1 E file, 7 in P0, folder 0; bookmarks 1 and 2 given.
    EXPECT_EQ(tree.files.at("P-SERIES/ABCD0000/LOG.SEQ"),
              Ds2CardWriter::buildLogSeq(1, 1, 7, 0, options().bookmarks));

    // PROP.BIN: LD from the log blocks. VC computed independently of this code
    // for the same values.
    const auto& prop = tree.files.at("P-SERIES/ABCD0000/PROP.BIN");
    const std::string p(prop.begin(), prop.end());
    EXPECT_EQ(p.rfind("CF=\r\nSN=TEST0000000001\r\nMN=520X110C\r\nPT=\r\nDF=\r\nDFV=3\r\n"
                      "F=0\r\nFV=6\r\nSV=V1.0.2.3590\r\nFD=\r\nLD=1893639600\r\n"
                      "FN=0\r\nPFN=7\r\nEFN=1\r\nDFN=1\r\nSID=\r\n"
                      "BK=0x00000000000000000000\r\nSK=0x1413121115161a191817\r\n"
                      "EK=0x00000000000000000000\r\nDK=0x00000000000000000000\r\n"
                      "TS=\r\nDC=\r\nVC=0x", 0), 0u);
    EXPECT_EQ(p.substr(p.size() - 13), "VC=0x14F3DAA6");

    // Warnings: streams 3 and 4 have no bookmark; seven constants not given.
    EXPECT_EQ(tree.warnings.size(), 2u + 7u);
}

TEST(PhilipsBleCard, WrittenTreeReadsBackThroughPhilipsParser) {
    auto tree = Ds2CardWriter::build(syntheticRecords(), kIdentity, options());
    ASSERT_TRUE(tree.ok()) << tree.error;
    const fs::path root = writeTree(tree, "readback");
    const fs::path p0 = root / tree.session_dir;

    auto info = PhilipsParser::parseProperties((p0.parent_path() / "PROP.BIN").string());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->serial_number, kIdentity.serial);
    EXPECT_EQ(info->model_number, "520X110C");
    EXPECT_EQ(info->software_version, "V1.0.2.3590");
    EXPECT_EQ(info->family, 0);
    EXPECT_EQ(info->family_version, 6);
    EXPECT_EQ(info->data_format_version, "3");
    EXPECT_EQ(info->patient_folder_num, 0);
    EXPECT_FALSE(PhilipsParser::isEncryptedCard(p0.string()));

    auto groups = PhilipsParser::discoverSessions(p0.string());
    ASSERT_EQ(groups.size(), 4u);
    EXPECT_EQ(groups.front().session_number, 256u);
    EXPECT_EQ(groups.back().session_number, 259u);
    EXPECT_TRUE(groups.back().events_path.empty());

    auto sessions = parseTree(p0);
    ASSERT_EQ(sessions.size(), 3u);   // 259 has no mask-on
    EXPECT_FALSE(sessions.count(259));
    for (const auto& [n, s] : sessions) {
        EXPECT_EQ(s->serial_number, kIdentity.serial) << n;
        EXPECT_EQ(s->product_name, "DreamStation 2 Auto CPAP") << n;
        EXPECT_EQ(s->firmware, "V1.0.2.3590") << n;
        EXPECT_EQ(s->parse_warnings, 0) << n;
        EXPECT_TRUE(s->has_summary) << n;
        ASSERT_TRUE(s->settings.has_value()) << n;
        EXPECT_EQ(s->settings->therapy_mode, 1) << n;   // auto CPAP
    }
    fs::remove_all(root);
}

TEST(PhilipsBleCard, LongSessionEveryEventKind) {
    auto tree = Ds2CardWriter::build(syntheticRecords(), kIdentity, options());
    ASSERT_TRUE(tree.ok()) << tree.error;
    const fs::path root = writeTree(tree, "long");
    auto sessions = parseTree(root / tree.session_dir);
    ASSERT_TRUE(sessions.count(256));
    const auto& s = *sessions.at(256);

    EXPECT_EQ(secs(*s.session_start), T0 + 1);
    EXPECT_EQ(secs(*s.session_end), T0 + 7201);
    EXPECT_EQ(s.duration_seconds, 7200);
    EXPECT_TRUE(s.has_events);

    const std::vector<Ev> expect = {
        {EventType::OBSTRUCTIVE, T0 + 590, 0},   {EventType::HYPOPNEA, T0 + 875, 25},
        {EventType::RERA, T0 + 1495, 0},         {EventType::CLEAR_AIRWAY, T0 + 2392, 0},
        {EventType::HYPOPNEA, T0 + 2982, 18},    {EventType::OBSTRUCTIVE, T0 + 4188, 0},
        {EventType::OTHER, T0 + 4480, 0},        {EventType::RERA, T0 + 5097, 0},
        {EventType::FLOW_LIMITATION, T0 + 5696, 0}, {EventType::HYPOPNEA, T0 + 6570, 30},
    };
    EXPECT_EQ(events(s), expect);

    // 2 OA + 1 CA + 3 H over 7,200 s = 3.0; the 0x0b variant is out of the AHI.
    const auto& m = *s.metrics;
    EXPECT_EQ(m.obstructive_apneas, 2);
    EXPECT_EQ(m.clear_airway_apneas, 1);
    EXPECT_EQ(m.hypopneas, 3);
    EXPECT_EQ(m.reras, 2);
    EXPECT_DOUBLE_EQ(m.ahi, 3.0);

    ASSERT_TRUE(s.machine_reported.has_value());
    EXPECT_EQ(s.machine_reported->obstructive_apneas, 2);
    EXPECT_EQ(s.machine_reported->clear_airway_apneas, 1);
    EXPECT_EQ(s.machine_reported->hypopneas, 3);
    EXPECT_EQ(s.machine_reported->hypopneas_variant, 1);
    EXPECT_EQ(s.machine_reported->reras, 2);
    EXPECT_EQ(s.machine_reported->flow_limitations, 1);
    fs::remove_all(root);
}

TEST(PhilipsBleCard, MultiPeriodAndShortSessions) {
    auto tree = Ds2CardWriter::build(syntheticRecords(), kIdentity, options());
    ASSERT_TRUE(tree.ok()) << tree.error;
    const fs::path root = writeTree(tree, "periods");
    auto sessions = parseTree(root / tree.session_dir);

    // 257: first mask-on to last mask-off; therapy = 600 + 300 + 1,200, the
    // two mask-off breaks not counted. Its events come from two blocks.
    ASSERT_TRUE(sessions.count(257));
    const auto& b = *sessions.at(257);
    EXPECT_EQ(secs(*b.session_start), T1 + 2);
    EXPECT_EQ(secs(*b.session_end), T1 + 2282);
    EXPECT_EQ(b.duration_seconds, 2100);
    // The three periods themselves, in order, as the summary lists them.
    ASSERT_EQ(b.mask_on_periods.size(), 3u);
    const std::vector<std::pair<int64_t, int64_t>> periods = {
        {T1 + 2, T1 + 602}, {T1 + 722, T1 + 1022}, {T1 + 1082, T1 + 2282}};
    for (size_t k = 0; k < periods.size(); ++k) {
        EXPECT_EQ(secs(b.mask_on_periods[k].first), periods[k].first) << k;
        EXPECT_EQ(secs(b.mask_on_periods[k].second), periods[k].second) << k;
    }
    const std::vector<Ev> expect = {
        {EventType::OBSTRUCTIVE, T1 + 302, 0},
        {EventType::HYPOPNEA, T1 + 885, 15},
        {EventType::RERA, T1 + 1998, 0},
    };
    EXPECT_EQ(events(b), expect);
    EXPECT_NEAR(b.metrics->ahi, 2 * 3600.0 / 2100, 1e-9);
    ASSERT_TRUE(b.machine_reported.has_value());   // summed over the three periods
    EXPECT_EQ(b.machine_reported->obstructive_apneas, 1);
    EXPECT_EQ(b.machine_reported->hypopneas, 1);
    EXPECT_EQ(b.machine_reported->reras, 1);

    // 258: 4 seconds, an events file with no event.
    ASSERT_TRUE(sessions.count(258));
    const auto& c = *sessions.at(258);
    EXPECT_EQ(secs(*c.session_start), T2 + 1);
    EXPECT_EQ(c.duration_seconds, 4);
    EXPECT_TRUE(c.events.empty());
    EXPECT_FALSE(c.has_events);
    EXPECT_EQ(c.metrics->ahi, 0.0);
    fs::remove_all(root);
}

// ── The direct path ─────────────────────────────────────────────────────────

TEST(PhilipsBleCard, DirectPathEqualsTheWrittenTree) {
    const auto recs = syntheticRecords();
    auto tree = Ds2CardWriter::build(recs, kIdentity, options());
    ASSERT_TRUE(tree.ok()) << tree.error;
    const fs::path root = writeTree(tree, "direct");
    auto from_tree = parseTree(root / tree.session_dir);

    PhilipsParseWarnings w;
    auto direct = parseDs2BleRecords(recs, kIdentity, "dev", "", &w);
    EXPECT_EQ(w.total(), 0);
    ASSERT_EQ(direct.size(), 3u);
    ASSERT_EQ(direct.size(), from_tree.size());
    auto it = from_tree.begin();   // both ascending by session number
    for (const auto& d : direct) {
        SCOPED_TRACE("session " + std::to_string(it->first));
        const auto& t = *(it++)->second;
        EXPECT_EQ(d->session_start, t.session_start);
        EXPECT_EQ(d->session_end, t.session_end);
        EXPECT_EQ(d->duration_seconds, t.duration_seconds);
        EXPECT_EQ(d->mask_on_periods, t.mask_on_periods);
        EXPECT_FALSE(d->mask_on_periods.empty());
        EXPECT_EQ(d->serial_number, t.serial_number);
        EXPECT_EQ(d->product_name, t.product_name);
        EXPECT_EQ(d->firmware, t.firmware);
        EXPECT_EQ(d->model_id, t.model_id);
        EXPECT_EQ(events(*d), events(t));
        EXPECT_DOUBLE_EQ(d->metrics->ahi, t.metrics->ahi);
        EXPECT_EQ(d->metrics->total_events, t.metrics->total_events);
    }
    fs::remove_all(root);
}

TEST(PhilipsBleCard, DirectPathSkipsABadRecord) {
    auto recs = syntheticRecords();
    recs[4].body[recs[4].body.size() - 6] ^= 0xff;   // 258's summary: data CRC fails
    PhilipsParseWarnings w;
    auto direct = parseDs2BleRecords(recs, kIdentity, "dev", "", &w);
    EXPECT_EQ(direct.size(), 2u);   // 258 has no summary left
    EXPECT_EQ(w.unsupported_chunks, 1);
}

// ── Refusals and options ────────────────────────────────────────────────────

TEST(PhilipsBleCard, RefusesWhatItCannotWriteFaithfully) {
    const auto good = syntheticRecords();
    auto expectError = [&](std::vector<Ds2BleRecord> recs, Ds2CardOptions o,
                           const std::string& needle) {
        auto t = Ds2CardWriter::build(recs, kIdentity, o);
        EXPECT_FALSE(t.ok());
        EXPECT_TRUE(t.files.empty());
        EXPECT_NE(t.error.find(needle), std::string::npos) << t.error;
    };

    auto bad_crc = good;
    bad_crc[1].body[bad_crc[1].body.size() - 6] ^= 0x01;
    expectError(bad_crc, options(), "fails its checks");

    auto wrong_session = good;
    wrong_session[0].session = 257;                   // body says 256
    expectError(wrong_session, options(), "block says session 256");

    auto wrong_stream = good;
    wrong_stream[0].stream = 2;                       // a .B01 body sent as stream 2
    expectError(wrong_stream, options(), "file extension 1");

    auto duplicate = good;
    duplicate.push_back(good[0]);
    expectError(duplicate, options(), "two records for");

    auto trailing = good;
    trailing[0].body.push_back(0);
    expectError(trailing, options(), "fill the body");

    auto unknown_stream = good;
    unknown_stream.push_back(Ds2BleRecord{5, 0, good[0].body});
    expectError(unknown_stream, options(), "no card file is known");

    auto o = options();
    o.machine_folder = "../x";
    expectError(good, o, "machine_folder");

    auto no_logs = good;                              // nothing dates the card
    no_logs.resize(7);
    expectError(no_logs, options(), "written_at");

    auto t = Ds2CardWriter::build(good, Ds2MachineIdentity{"", "520X110C", "V1"}, options());
    EXPECT_FALSE(t.ok());
}

TEST(PhilipsBleCard, WrittenAtAndPropConstantsAreTheCallers) {
    auto o = options();
    o.written_at = 1900000000;
    o.prop_constants = {{"CF", "c1"}, {"PT", "p1"}, {"DF", "0"}, {"FD", "f1"},
                        {"SID", "s1"}, {"TS", "t1"}, {"DC", "d1"},
                        {"SN", "IGNORED"}};       // a record-derived key cannot be overridden
    o.bookmarks[3] = bookmark(0x21);
    o.bookmarks[4] = bookmark(0x31);
    auto recs = syntheticRecords();
    recs.resize(7);                               // no logs: written_at dates the card
    auto tree = Ds2CardWriter::build(recs, kIdentity, o);
    ASSERT_TRUE(tree.ok()) << tree.error;
    EXPECT_TRUE(tree.warnings.empty());
    EXPECT_EQ(tree.files.size(), 10u);
    const auto& prop = tree.files.at("P-SERIES/ABCD0000/PROP.BIN");
    const std::string p(prop.begin(), prop.end());
    EXPECT_EQ(p.rfind("CF=c1\r\nSN=TEST0000000001\r\nMN=520X110C\r\nPT=p1\r\nDF=0\r\n", 0), 0u);
    EXPECT_NE(p.find("\r\nLD=1900000000\r\nFN=0\r\nPFN=7\r\nEFN=0\r\nDFN=0\r\n"), std::string::npos);
    EXPECT_NE(p.find("\r\nSID=s1\r\nBK=0x00000000000000000000\r\n"), std::string::npos);
    EXPECT_NE(p.find("\r\nEK=0x3433323135363a393837\r\nDK=0x2423222125262a292827\r\n"),
              std::string::npos);
    EXPECT_NE(p.find("\r\nTS=t1\r\nDC=d1\r\nVC=0x"), std::string::npos);
    EXPECT_EQ(tree.files.at("P-SERIES/ABCD0000/LOG.SEQ"),
              Ds2CardWriter::buildLogSeq(0, 0, 7, 0, o.bookmarks));
}

#endif // CPAPDASH_WITH_PHILIPS
