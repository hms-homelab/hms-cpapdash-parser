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
