// A ResMed night is several mask-on blocks, and each block writes its own EVE.
//
// parseSession used to hold `std::string eve_file` beside its BRP/PLD/SAD
// vectors, so the last EVE that directory_iterator happened to yield won and
// every other block's annotations were dropped. Directory order is unspecified,
// so which one survived was not even stable between runs.
//
// Real cost, from hms-cpap issue #22: a card whose first block is a seconds-long
// mask-fit check carrying the empty 832-byte EVE stub reported AHI 0.0 for the
// night, while OSCAR read 2.84 off the same bytes.
#include <gtest/gtest.h>

#include "cpapdash/parser/EDFParser.h"
#include "cpapdash/parser/EDFFile.h"
#include "cpapdash/parser/Models.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace cpapdash::parser;
namespace fs = std::filesystem;

namespace {

void writeField(std::vector<uint8_t>& buf, int off, int len, const std::string& v) {
    for (int i = 0; i < len; ++i)
        buf[off + i] = (i < static_cast<int>(v.size())) ? v[i] : ' ';
}

// A minimal BRP so parseSession has the checkpoint it requires.
std::vector<uint8_t> buildBRP(const std::string& ddmmyy, const std::string& hhmmss) {
    const int ns = 1, spr = 1500, records = 1;
    const int header_bytes = 256 + 256 * ns;
    std::vector<uint8_t> buf(header_bytes + records * spr * 2, ' ');

    writeField(buf, 0, 8, "0");
    writeField(buf, 8, 80, "TestPatient");
    writeField(buf, 88, 80, "Startdate");
    writeField(buf, 168, 8, ddmmyy);
    writeField(buf, 176, 8, hhmmss);
    writeField(buf, 184, 8, std::to_string(header_bytes));
    writeField(buf, 192, 44, "");
    writeField(buf, 236, 8, std::to_string(records));
    writeField(buf, 244, 8, "60");
    writeField(buf, 252, 4, std::to_string(ns));

    int base = 256;
    auto block = [&](int width, const std::string& v) {
        writeField(buf, base, width, v);
        base += ns * width;
    };
    block(16, "Flow.40ms");
    block(80, "");
    block(8, "L/s");
    block(8, "-10");
    block(8, "10");
    block(8, "-32768");
    block(8, "32767");
    block(80, "");
    block(8, std::to_string(spr));
    block(32, "");

    for (int i = 0; i < records * spr; ++i) {
        int16_t v = static_cast<int16_t>((i % 200) - 100);
        buf[header_bytes + i * 2]     = static_cast<uint8_t>(v & 0xFF);
        buf[header_bytes + i * 2 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }
    return buf;
}

// An EDF+ annotations file: one "EDF Annotations" signal, one record, TALs in
// the ResMed shape (+onset\x15duration\x14description\x14\0).
std::vector<uint8_t> buildEVE(const std::string& ddmmyy, const std::string& hhmmss,
                              const std::vector<std::pair<double, std::string>>& events) {
    const int ns = 1, spr = 512, records = 1;
    const int header_bytes = 256 + 256 * ns;
    const int annot_bytes = spr * 2;
    std::vector<uint8_t> buf(header_bytes + annot_bytes, ' ');

    writeField(buf, 0, 8, "0");
    writeField(buf, 8, 80, "TestPatient");
    writeField(buf, 88, 80, "Startdate");
    writeField(buf, 168, 8, ddmmyy);
    writeField(buf, 176, 8, hhmmss);
    writeField(buf, 184, 8, std::to_string(header_bytes));
    writeField(buf, 192, 44, "EDF+C");            // isEDFPlus() keys off this
    writeField(buf, 236, 8, std::to_string(records));
    writeField(buf, 244, 8, "60");
    writeField(buf, 252, 4, std::to_string(ns));

    int base = 256;
    auto block = [&](int width, const std::string& v) {
        writeField(buf, base, width, v);
        base += ns * width;
    };
    block(16, "EDF Annotations");
    block(80, "");
    block(8, "");
    block(8, "-32768");
    block(8, "32767");
    block(8, "-32768");
    block(8, "32767");
    block(80, "");
    block(8, std::to_string(spr));
    block(32, "");

    std::string tals;
    tals += "+0";
    tals += '\x14';
    tals += '\x14';
    tals += '\0';                                  // record timestamp, no description
    for (const auto& [onset, desc] : events) {
        tals += "+" + std::to_string(onset);
        tals += '\x15';
        tals += "10";                              // 10 s duration
        tals += '\x14';
        tals += desc;
        tals += '\x14';
        tals += '\0';
    }

    for (int i = 0; i < annot_bytes; ++i)
        buf[header_bytes + i] = (i < static_cast<int>(tals.size()))
                                    ? static_cast<uint8_t>(tals[i]) : 0;
    return buf;
}

void writeFile(const fs::path& p, const std::vector<uint8_t>& buf) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
}

class EveMultiBlock : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        dir_ = fs::temp_directory_path() / "cpapdash_eve_multiblock";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }
};

}  // namespace

// The reporter's card: the first block is a mask-fit check whose EVE is empty,
// the blocks that follow carry the night's real annotations.
TEST_F(EveMultiBlock, EveryBlocksEventsSurviveTheMerge) {
    writeFile(dir_ / "20250812_233427_BRP.edf", buildBRP("12.08.25", "23:34:27"));

    writeFile(dir_ / "20250812_233427_EVE.edf",
              buildEVE("12.08.25", "23:34:27", {}));                  // the 832 B stub
    writeFile(dir_ / "20250812_235319_EVE.edf",
              buildEVE("12.08.25", "23:53:19", {{10.0, "Obstructive Apnea"},
                                                {70.0, "Hypopnea"}}));
    writeFile(dir_ / "20250813_034616_EVE.edf",
              buildEVE("13.08.25", "03:46:16", {{5.0, "Central Apnea"}}));

    auto session = EDFParser::parseSession(dir_.string(), "dev", "ResMed");
    ASSERT_NE(session, nullptr);

    // Before the fix this was 0 or 1 depending on directory order.
    ASSERT_EQ(session->events.size(), 3u);
    EXPECT_TRUE(session->has_events);

    int obstructive = 0, hypopnea = 0, central = 0;
    for (const auto& e : session->events) {
        if (e.event_type == EventType::OBSTRUCTIVE) obstructive++;
        if (e.event_type == EventType::HYPOPNEA)    hypopnea++;
        if (e.event_type == EventType::CENTRAL)     central++;
    }
    EXPECT_EQ(obstructive, 1);
    EXPECT_EQ(hypopnea, 1);
    EXPECT_EQ(central, 1);
}

// Events are concatenated per file, so the night is only a timeline if the
// whole set is sorted afterwards.
TEST_F(EveMultiBlock, EventsAreOrderedAcrossFiles) {
    writeFile(dir_ / "20250812_233427_BRP.edf", buildBRP("12.08.25", "23:34:27"));
    // Deliberately written so the LATER file sorts FIRST by name, proving the
    // order comes from the timestamps and not from the filenames.
    writeFile(dir_ / "20250812_235319_EVE.edf",
              buildEVE("12.08.25", "23:53:19", {{0.0, "Hypopnea"}}));
    writeFile(dir_ / "20250813_034616_EVE.edf",
              buildEVE("13.08.25", "03:46:16", {{0.0, "Central Apnea"}}));

    auto session = EDFParser::parseSession(dir_.string(), "dev", "ResMed");
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(session->events.size(), 2u);
    EXPECT_LE(session->events[0].timestamp, session->events[1].timestamp);
}

// A night whose only EVE is the empty stub reports no events rather than failing.
TEST_F(EveMultiBlock, AnEmptyStubIsStillAParsedNight) {
    writeFile(dir_ / "20250812_233427_BRP.edf", buildBRP("12.08.25", "23:34:27"));
    writeFile(dir_ / "20250812_233427_EVE.edf", buildEVE("12.08.25", "23:34:27", {}));

    auto session = EDFParser::parseSession(dir_.string(), "dev", "ResMed");
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(session->events.empty());
}

// CSL presence is a flag, and several blocks must not turn it off.
TEST_F(EveMultiBlock, SeveralCslFilesStillMeanHasSummary) {
    writeFile(dir_ / "20250812_233427_BRP.edf", buildBRP("12.08.25", "23:34:27"));
    writeFile(dir_ / "20250812_233427_CSL.edf", buildEVE("12.08.25", "23:34:27", {}));
    writeFile(dir_ / "20250812_235319_CSL.edf", buildEVE("12.08.25", "23:53:19", {}));

    auto session = EDFParser::parseSession(dir_.string(), "dev", "ResMed");
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(session->has_summary);
}

// ── The buffer form takes every EVE too ─────────────────────────────────────
//
// A caller that reads files itself (hms-cpapdash-api does) never touches the
// directory form, so fixing that one alone left the cloud handing over a single
// EVE buffer and losing the rest of the night's events.
//
// This matters most when there is NO STR.edf: the machine's own daily record
// wins when present, and when it is absent these parsed events ARE the summary
// and the dashboard. Counting one file's worth understates the night.

TEST_F(EveMultiBlock, TheBufferFormUnionsEveryEve) {
    auto brp  = buildBRP("12.08.25", "23:34:27");
    auto eve1 = buildEVE("12.08.25", "23:34:27", {});                       // empty stub
    auto eve2 = buildEVE("12.08.25", "23:53:19", {{10.0, "Obstructive Apnea"},
                                                  {70.0, "Hypopnea"}});
    auto eve3 = buildEVE("13.08.25", "03:46:16", {{5.0, "Central Apnea"}});

    std::vector<EDFParser::ByteView> eves = {
        {eve1.data(), eve1.size()},
        {eve2.data(), eve2.size()},
        {eve3.data(), eve3.size()},
    };

    auto s = EDFParser::parseSessionFromBuffers(
        brp.data(), brp.size(), nullptr, 0, nullptr, 0, eves, "dev", "ResMed");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->events.size(), 3u)
        << "the buffer form kept one EVE and dropped the rest of the night";
    EXPECT_TRUE(s->has_events);
}

TEST_F(EveMultiBlock, TheBufferEventsAreOrderedAcrossBuffers) {
    auto brp  = buildBRP("12.08.25", "23:34:27");
    auto late = buildEVE("13.08.25", "03:46:16", {{0.0, "Central Apnea"}});
    auto early= buildEVE("12.08.25", "23:53:19", {{0.0, "Hypopnea"}});

    // Handed over out of order on purpose.
    std::vector<EDFParser::ByteView> eves = {
        {late.data(), late.size()}, {early.data(), early.size()}};

    auto s = EDFParser::parseSessionFromBuffers(
        brp.data(), brp.size(), nullptr, 0, nullptr, 0, eves, "dev", "ResMed");
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->events.size(), 2u);
    EXPECT_LE(s->events[0].timestamp, s->events[1].timestamp);
}

// The single-EVE overload still exists and still behaves, because callers that
// genuinely have one file should not have to build a vector.
TEST_F(EveMultiBlock, TheSingleBufferOverloadStillWorks) {
    auto brp = buildBRP("12.08.25", "23:34:27");
    auto eve = buildEVE("12.08.25", "23:53:19", {{10.0, "Obstructive Apnea"}});

    auto s = EDFParser::parseSessionFromBuffers(
        brp.data(), brp.size(), nullptr, 0, nullptr, 0,
        eve.data(), eve.size(), "dev", "ResMed");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->events.size(), 1u);
}

// No EVE at all is a valid night, not a parse failure.
TEST_F(EveMultiBlock, TheBufferFormAcceptsNoEveAtAll) {
    auto brp = buildBRP("12.08.25", "23:34:27");
    auto s = EDFParser::parseSessionFromBuffers(
        brp.data(), brp.size(), nullptr, 0, nullptr, 0,
        std::vector<EDFParser::ByteView>{}, "dev", "ResMed");
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->events.empty());
    EXPECT_FALSE(s->has_events);
}

// ── Every type, not just EVE, and nothing header-only ───────────────────────
//
// hms-cpapdash-api kept only the LARGEST file of each type, so a night lost
// every BRP checkpoint but one as well as every EVE but one. The directory form
// has always collected all of them; the buffer form now does too.

TEST_F(EveMultiBlock, TheBufferFormParsesEveryBrpNotJustOne) {
    auto brp1 = buildBRP("12.08.25", "23:34:27");   // 1 record each
    auto brp2 = buildBRP("12.08.25", "23:44:27");
    auto brp3 = buildBRP("12.08.25", "23:54:27");

    EDFParser::SessionBuffers one, all;
    one.brp = {{brp1.data(), brp1.size()}};
    all.brp = {{brp1.data(), brp1.size()},
               {brp2.data(), brp2.size()},
               {brp3.data(), brp3.size()}};

    auto s_one = EDFParser::parseSessionFromBuffers(one, "dev", "ResMed");
    auto s_all = EDFParser::parseSessionFromBuffers(all, "dev", "ResMed");
    ASSERT_NE(s_one, nullptr);
    ASSERT_NE(s_all, nullptr);

    EXPECT_GT(s_all->breathing_summary.size(), s_one->breathing_summary.size())
        << "keeping one BRP loses the rest of the night's flow";
}

// Header-only files are routine on a real card and must contribute nothing.
// Feeding one to the parsers adds no samples but does add its timestamp, which
// is how a night once got anchored to an empty checkpoint and came out short.
TEST_F(EveMultiBlock, HeaderOnlyFilesAreSkipped) {
    auto real  = buildBRP("12.08.25", "23:34:27");
    auto empty = buildBRP("12.08.25", "22:00:00");
    // Truncate to the header: 256 + 256*1 bytes, zero data records.
    empty.resize(256 + 256);

    EDFParser::SessionBuffers b;
    b.brp = {{empty.data(), empty.size()}, {real.data(), real.size()}};

    auto s = EDFParser::parseSessionFromBuffers(b, "dev", "ResMed");
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->breathing_summary.empty()) << "the real checkpoint was lost";

    // Same session parsed without the empty file: identical sample count.
    EDFParser::SessionBuffers only_real;
    only_real.brp = {{real.data(), real.size()}};
    auto s2 = EDFParser::parseSessionFromBuffers(only_real, "dev", "ResMed");
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s->breathing_summary.size(), s2->breathing_summary.size())
        << "the header-only file contributed something it should not have";
}

// An empty EVE stub is not "has events".
TEST_F(EveMultiBlock, AHeaderOnlyEveDoesNotCountAsHavingEvents) {
    auto brp = buildBRP("12.08.25", "23:34:27");
    auto stub = buildEVE("12.08.25", "23:34:27", {});
    stub.resize(256 + 256);                       // header only

    EDFParser::SessionBuffers b;
    b.brp = {{brp.data(), brp.size()}};
    b.eve = {{stub.data(), stub.size()}};

    auto s = EDFParser::parseSessionFromBuffers(b, "dev", "ResMed");
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->events.empty());
    EXPECT_FALSE(s->has_events);
}

// A null or zero-length buffer in the list is skipped, not a crash.
TEST_F(EveMultiBlock, NullAndEmptyBuffersAreIgnored) {
    auto brp = buildBRP("12.08.25", "23:34:27");
    EDFParser::SessionBuffers b;
    b.brp = {{nullptr, 0}, {brp.data(), brp.size()}, {brp.data(), 0}};
    b.eve = {{nullptr, 0}};
    b.pld = {{nullptr, 5}};                      // non-null, bogus length

    auto s = EDFParser::parseSessionFromBuffers(b, "dev", "ResMed");
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->breathing_summary.empty());
}

// CSL presence is a flag and several of them do not turn it off.
TEST_F(EveMultiBlock, SeveralCslBuffersStillMeanHasSummary) {
    auto brp = buildBRP("12.08.25", "23:34:27");
    auto csl = buildEVE("12.08.25", "23:34:27", {});
    EDFParser::SessionBuffers b;
    b.brp = {{brp.data(), brp.size()}};
    b.csl = {{csl.data(), csl.size()}, {csl.data(), csl.size()}};

    auto s = EDFParser::parseSessionFromBuffers(b, "dev", "ResMed");
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->has_summary);
}
