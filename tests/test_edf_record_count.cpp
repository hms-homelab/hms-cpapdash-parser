#include <gtest/gtest.h>
#include "cpapdash/parser/EdfRecordCount.h"

#include <algorithm>
#include <cstring>
#include <string>

using namespace cpapdash::parser;

namespace {

/// An EDF with two signals ([spr_a] and [spr_b] samples per record) and
/// [records] data records, whose header states [stored] as num-data-records,
/// the way ResMed leaves it while a file is recording ("-1", or a low count).
std::string makeEdf(int records, int spr_a, int spr_b, const std::string& stored) {
    const int ns = 2;
    const int header_bytes = 256 + 256 * ns;
    std::string h(header_bytes, ' ');
    auto field = [&](std::size_t off, std::size_t len, const std::string& v) {
        std::memcpy(&h[off], v.data(), std::min(v.size(), len));
    };
    field(0, 8, "0");
    field(168, 8, "13.09.26");
    field(176, 8, "23.32.06");
    field(184, 8, std::to_string(header_bytes));
    field(236, 8, stored);
    field(244, 8, "60");
    field(252, 4, std::to_string(ns));
    const std::size_t spr_off = 256 + ns * 216;
    field(spr_off, 8, std::to_string(spr_a));
    field(spr_off + 8, 8, std::to_string(spr_b));
    const std::size_t data = static_cast<std::size_t>(records) * (spr_a + spr_b) * 2;
    return h + std::string(data, '\x01');
}

std::string countOf(const std::string& edf) { return edf.substr(236, 8); }

}  // namespace

TEST(EdfRecordCount, AnUnfinalizedMinusOneBecomesTheRecordCount) {
    std::string f = makeEdf(245, 1500, 25, "-1");
    EXPECT_TRUE(repairEdfDataRecords(&f[0], f.size()));
    EXPECT_EQ(countOf(f), "245     ") << "left-justified, space-padded, as ResMed writes it";
}

TEST(EdfRecordCount, ACountLowerThanTheDataIsRaised) {
    // Seen on a real archive: 187 stored for 245 records of data.
    std::string f = makeEdf(245, 1500, 25, "187");
    EXPECT_TRUE(repairEdfDataRecords(&f[0], f.size()));
    EXPECT_EQ(countOf(f), "245     ");
}

TEST(EdfRecordCount, ACorrectCountIsLeftAlone) {
    std::string f = makeEdf(47, 1500, 25, "47");
    const std::string before = f;
    EXPECT_FALSE(repairEdfDataRecords(&f[0], f.size()));
    EXPECT_EQ(f, before);
}

TEST(EdfRecordCount, DataThatDoesNotDivideEvenlyIsLeftAlone) {
    // Pulled mid-record: the image is not whole, so no count is trusted.
    std::string f = makeEdf(10, 1500, 25, "-1") + "xyz";
    const std::string before = f;
    EXPECT_FALSE(repairEdfDataRecords(&f[0], f.size()));
    EXPECT_EQ(f, before);
}

TEST(EdfRecordCount, AHeaderItCannotReadIsLeftAlone) {
    std::string short_buf(200, ' ');
    EXPECT_FALSE(repairEdfDataRecords(&short_buf[0], short_buf.size()));

    std::string bad_ns = makeEdf(3, 10, 10, "-1");
    std::memcpy(&bad_ns[252], "abcd", 4);
    EXPECT_FALSE(repairEdfDataRecords(&bad_ns[0], bad_ns.size()));

    std::string bad_hdr = makeEdf(3, 10, 10, "-1");
    std::memcpy(&bad_hdr[184], "100     ", 8);   // smaller than the fixed 256
    EXPECT_FALSE(repairEdfDataRecords(&bad_hdr[0], bad_hdr.size()));

    std::string no_samples = makeEdf(3, 0, 0, "-1");
    EXPECT_FALSE(repairEdfDataRecords(&no_samples[0], no_samples.size()));

    EXPECT_FALSE(repairEdfDataRecords(nullptr, 1000));
}

TEST(EdfRecordCount, TheHeaderOnlyFormAgreesAndReadsNothingPastItsBuffer) {
    const std::string whole = makeEdf(245, 1500, 25, "-1");
    const std::size_t hb = edfHeaderBytes(whole.data(), whole.size());
    ASSERT_EQ(hb, 768u);

    // Only the header, exactly: the caller read that much of the file.
    std::string header = whole.substr(0, hb);
    EXPECT_TRUE(repairEdfDataRecords(&header[0], header.size(), whole.size()));
    std::string whole_copy = whole;
    ASSERT_TRUE(repairEdfDataRecords(&whole_copy[0], whole_copy.size()));
    EXPECT_EQ(header, whole_copy.substr(0, hb)) << "the same bytes either way";

    // A buffer shorter than the header it declares is refused, not overrun.
    std::string part = whole.substr(0, hb - 1);
    EXPECT_FALSE(repairEdfDataRecords(&part[0], part.size(), whole.size()));

    EXPECT_EQ(edfHeaderBytes(whole.data(), 100), 0u);
}

TEST(EdfRecordCount, OnlyResMedSignalFilesAreNamed) {
    EXPECT_TRUE(isResmedSignalEdf("20260913_233206_BRP.edf"));
    EXPECT_TRUE(isResmedSignalEdf("20260913_233206_PLD.EDF"));
    EXPECT_TRUE(isResmedSignalEdf("DATALOG/20260913/20260913_233206_SAD.edf"));
    // The 11 series (an AirCurve 11 VAuto card, hms-cpap #33).
    EXPECT_TRUE(isResmedSignalEdf("20260911_225616_SA2.edf"));
    EXPECT_TRUE(isResmedSignalEdf("20260911_225616_TCV.edf"));
    EXPECT_FALSE(isResmedSignalEdf("20260913_233200_EVE.edf"));
    EXPECT_FALSE(isResmedSignalEdf("20260913_233200_CSL.edf"));
    EXPECT_FALSE(isResmedSignalEdf("STR.edf"));
    EXPECT_FALSE(isResmedSignalEdf("20260913_233206_BRP.crc"));
}
