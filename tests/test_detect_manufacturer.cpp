// SDD-022 slice 3: detectManufacturer() -- broad, detect-only-capable brand
// identification. Pure filename/path pattern matching, no file content reads, so
// these tests don't need CPAPDASH_WITH_LOWENSTEIN (they check the returned enum
// value, not that a parser instance can be constructed for it).
#include <gtest/gtest.h>
#include "cpapdash/parser/ISessionParser.h"

#include <filesystem>
#include <fstream>

using namespace cpapdash::parser;
namespace fs = std::filesystem;

// ── filenames overload ───────────────────────────────────────────────────────

TEST(DetectManufacturerTest, Wmedf_IsLowenstein) {
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"signal_000001.wmedf"}),
              DeviceManufacturer::LOWENSTEIN);
}

TEST(DetectManufacturerTest, Wmedf_CaseInsensitive) {
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"SIGNAL_1.WMEDF"}),
              DeviceManufacturer::LOWENSTEIN);
}

TEST(DetectManufacturerTest, BmcUsrPattern_IsBmc) {
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"16C01034.usr", "16C01034.evt"}),
              DeviceManufacturer::BMC);
}

TEST(DetectManufacturerTest, BmcUsrPattern_WrongShape_NotBmc) {
    // Not the \d\dC\d{5}.usr shape (extra digit, wrong letter position).
    EXPECT_NE(detectManufacturer(std::vector<std::string>{"160C01034.usr"}),
              DeviceManufacturer::BMC);
    EXPECT_NE(detectManufacturer(std::vector<std::string>{"AB16C0103.usr"}),
              DeviceManufacturer::BMC);
}

TEST(DetectManufacturerTest, PropertiesTxt_IsPhilips) {
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"Properties.txt"}),
              DeviceManufacturer::PHILIPS);
}

TEST(DetectManufacturerTest, PropertiesTxt_PathPrefixIgnored) {
    // Basename match only -- directory components (P-SERIES/<serial>/) don't matter.
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"P-SERIES/P1192913945CE/properties.txt"}),
              DeviceManufacturer::PHILIPS);
}

TEST(DetectManufacturerTest, EdfFiles_IsResMed) {
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"20260601_120000_BRP.edf", "STR.edf"}),
              DeviceManufacturer::RESMED);
}

TEST(DetectManufacturerTest, UnknownFiles_ReturnsUnknown) {
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"readme.txt", "config.ini"}),
              DeviceManufacturer::UNKNOWN);
}

TEST(DetectManufacturerTest, EmptyFilenames_ReturnsUnknown) {
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{}), DeviceManufacturer::UNKNOWN);
}

// Precedence: most-specific-first, first match wins. A (contrived) fileset that
// could match more than one signature must resolve to the FIRST checked brand.
TEST(DetectManufacturerTest, Precedence_WmedfBeatsEdf) {
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"signal_1.wmedf", "STR.edf"}),
              DeviceManufacturer::LOWENSTEIN);
}

TEST(DetectManufacturerTest, Precedence_BmcBeatsEdf) {
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"16C01034.usr", "16C01034.001"}),
              DeviceManufacturer::BMC);
}

TEST(DetectManufacturerTest, Precedence_PropertiesBeatsEdf) {
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"Properties.txt", "somefile.edf"}),
              DeviceManufacturer::PHILIPS);
}

// ── directory overload ───────────────────────────────────────────────────────

class DetectManufacturerDirTest : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("detectmfr_" + std::to_string(::getpid()));
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
};

TEST_F(DetectManufacturerDirTest, LowensteinDir) {
    std::ofstream(dir_ / "signal_1.wmedf") << "x";
    EXPECT_EQ(detectManufacturer(dir_.string()), DeviceManufacturer::LOWENSTEIN);
}

TEST_F(DetectManufacturerDirTest, BmcDir) {
    std::ofstream(dir_ / "16C01034.usr") << "x";
    EXPECT_EQ(detectManufacturer(dir_.string()), DeviceManufacturer::BMC);
}

TEST_F(DetectManufacturerDirTest, PhilipsDir) {
    std::ofstream(dir_ / "Properties.txt") << "x";
    EXPECT_EQ(detectManufacturer(dir_.string()), DeviceManufacturer::PHILIPS);
}

TEST_F(DetectManufacturerDirTest, ResMedDir_ByEdfFile) {
    std::ofstream(dir_ / "STR.edf") << "x";
    EXPECT_EQ(detectManufacturer(dir_.string()), DeviceManufacturer::RESMED);
}

TEST_F(DetectManufacturerDirTest, ResMedDir_ByDatalogSubdir) {
    fs::create_directories(dir_ / "DATALOG");
    EXPECT_EQ(detectManufacturer(dir_.string()), DeviceManufacturer::RESMED);
}

TEST_F(DetectManufacturerDirTest, EmptyDir_ReturnsUnknown) {
    EXPECT_EQ(detectManufacturer(dir_.string()), DeviceManufacturer::UNKNOWN);
}

TEST(DetectManufacturerTest, NonexistentDir_ReturnsUnknown) {
    EXPECT_EQ(detectManufacturer(std::string("/no/such/dir/at/all")), DeviceManufacturer::UNKNOWN);
}
