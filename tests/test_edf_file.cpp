#include <gtest/gtest.h>
#include "sleeplink/parser/EDFFile.h"
#include <cstring>
#include <vector>
#include <cstdint>

using namespace sleeplink::parser;

namespace {

/**
 * Build a minimal valid EDF buffer in memory.
 *
 * Layout (1 signal, 1 data record):
 *   Main header:   256 bytes
 *   Signal header: 256 bytes (1 signal)
 *   Data:          samples_per_record * 2 bytes
 *
 * The signal "TestSig" has:
 *   phys_min=-100, phys_max=100, dig_min=-32768, dig_max=32767
 *   samples_per_record = samples_count
 *   record_duration = 1.0 s
 */
std::vector<uint8_t> buildMinimalEDF(
    int num_records,
    int samples_per_record,
    const std::vector<int16_t>& sample_data,
    const std::string& date_str = "01.03.26",   // dd.mm.yy
    const std::string& time_str = "23.30.00"     // hh.mm.ss
) {
    int num_signals = 1;
    int header_bytes = 256 + 256 * num_signals;
    int record_size = samples_per_record * 2;
    int data_size = num_records * record_size;
    int total_size = header_bytes + data_size;

    std::vector<uint8_t> buf(total_size, ' ');  // Fill with spaces (EDF convention)

    auto writeField = [&](int offset, int len, const std::string& val) {
        // Left-align, pad with spaces
        for (int i = 0; i < len; i++) {
            buf[offset + i] = (i < static_cast<int>(val.size())) ? val[i] : ' ';
        }
    };

    // --- Main header (256 bytes) ---
    writeField(0, 8, "0");                                      // version
    writeField(8, 80, "TestPatient");                            // patient
    writeField(88, 80, "Startdate 01-MAR-2026 SRN=12345 MID=99 VID=3"); // recording
    writeField(168, 8, date_str);                                // start date
    writeField(176, 8, time_str);                                // start time
    writeField(184, 8, std::to_string(header_bytes));            // header bytes
    writeField(192, 44, "");                                     // reserved (plain EDF)
    writeField(236, 8, std::to_string(num_records));             // num records
    writeField(244, 8, "1");                                     // record duration (1 second)
    writeField(252, 4, std::to_string(num_signals));             // num signals

    // --- Signal header (256 bytes per signal) ---
    int sig_base = 256;
    int pos = 0;

    // label (16 x ns)
    writeField(sig_base + pos, 16, "TestSig");
    pos += 16;

    // transducer (80 x ns)
    writeField(sig_base + pos, 80, "");
    pos += 80;

    // physical dimension (8 x ns)
    writeField(sig_base + pos, 8, "uV");
    pos += 8;

    // physical minimum (8 x ns)
    writeField(sig_base + pos, 8, "-100");
    pos += 8;

    // physical maximum (8 x ns)
    writeField(sig_base + pos, 8, "100");
    pos += 8;

    // digital minimum (8 x ns)
    writeField(sig_base + pos, 8, "-32768");
    pos += 8;

    // digital maximum (8 x ns)
    writeField(sig_base + pos, 8, "32767");
    pos += 8;

    // prefiltering (80 x ns)
    writeField(sig_base + pos, 80, "");
    pos += 80;

    // samples per data record (8 x ns)
    writeField(sig_base + pos, 8, std::to_string(samples_per_record));
    pos += 8;

    // reserved (32 x ns)
    writeField(sig_base + pos, 32, "");

    // --- Data records ---
    int data_offset = header_bytes;
    for (size_t i = 0; i < sample_data.size() && i < static_cast<size_t>(num_records * samples_per_record); ++i) {
        int16_t val = sample_data[i];
        buf[data_offset + i * 2]     = static_cast<uint8_t>(val & 0xFF);
        buf[data_offset + i * 2 + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    }

    return buf;
}

} // anonymous namespace

// ============================================================================
//  Tests
// ============================================================================

TEST(EDFFileTest, OpenFromBufferParsesHeader) {
    std::vector<int16_t> samples = {0, 100, -100, 32767, -32768};
    auto buf = buildMinimalEDF(1, 5, samples);

    EDFFile edf;
    ASSERT_TRUE(edf.open(buf.data(), buf.size()));

    EXPECT_EQ(edf.num_signals, 1);
    EXPECT_EQ(edf.actual_records, 1);
    EXPECT_EQ(edf.record_duration, 1.0);
    EXPECT_TRUE(edf.complete);
    EXPECT_FALSE(edf.growing);
    EXPECT_EQ(edf.extra_records, 0);
    EXPECT_EQ(edf.patient, "TestPatient");
    EXPECT_EQ(edf.signals[0].label, "TestSig");
    EXPECT_EQ(edf.signals[0].phys_dim, "uV");
}

TEST(EDFFileTest, OpenFromBufferParsesDate) {
    auto buf = buildMinimalEDF(1, 1, {0}, "15.06.26", "02.30.45");

    EDFFile edf;
    ASSERT_TRUE(edf.open(buf.data(), buf.size()));

    EXPECT_EQ(edf.start_day, 15);
    EXPECT_EQ(edf.start_month, 6);
    EXPECT_EQ(edf.start_year, 2026);
    EXPECT_EQ(edf.start_hour, 2);
    EXPECT_EQ(edf.start_minute, 30);
    EXPECT_EQ(edf.start_second, 45);
}

TEST(EDFFileTest, ReadSignalConvertsToPhysical) {
    // With phys_min=-100, phys_max=100, dig_min=-32768, dig_max=32767:
    // scale = 200 / 65535 ~= 0.003051804
    // offset = -100 - (-32768 * scale) = -100 + 100.0015 ~= 0.0015
    // physical = digital * scale + offset
    //
    // digital=0     -> physical ~= 0.0015    (near zero)
    // digital=32767 -> physical ~= 100.0     (max)
    // digital=-32768-> physical ~= -100.0    (min)

    std::vector<int16_t> samples = {0, 32767, -32768};
    auto buf = buildMinimalEDF(1, 3, samples);

    EDFFile edf;
    ASSERT_TRUE(edf.open(buf.data(), buf.size()));

    std::vector<double> out;
    int count = edf.readSignal(0, out);

    ASSERT_EQ(count, 3);
    EXPECT_NEAR(out[0], 0.0, 0.1);      // digital 0 -> near zero
    EXPECT_NEAR(out[1], 100.0, 0.1);    // digital max -> phys max
    EXPECT_NEAR(out[2], -100.0, 0.1);   // digital min -> phys min
}

TEST(EDFFileTest, FindSignalByPartialLabel) {
    auto buf = buildMinimalEDF(1, 1, {0});

    EDFFile edf;
    ASSERT_TRUE(edf.open(buf.data(), buf.size()));

    EXPECT_EQ(edf.findSignal("Test"), 0);
    EXPECT_EQ(edf.findSignal("Sig"), 0);
    EXPECT_EQ(edf.findSignal("NotHere"), -1);
}

TEST(EDFFileTest, FindSignalExact) {
    auto buf = buildMinimalEDF(1, 1, {0});

    EDFFile edf;
    ASSERT_TRUE(edf.open(buf.data(), buf.size()));

    EXPECT_EQ(edf.findSignalExact("TestSig"), 0);
    EXPECT_EQ(edf.findSignalExact("Test"), -1);     // partial match should fail
    EXPECT_EQ(edf.findSignalExact("testsig"), -1);   // case-sensitive
}

TEST(EDFFileTest, DetectsGrowingFile) {
    // Create EDF with header declaring 1 record but data for 3 records
    std::vector<int16_t> samples = {100, 200, 300};
    int samples_per_record = 1;
    int num_records_header = 1;
    int actual_data_records = 3;

    // Build buffer manually with header saying 1 record but 3 records of data
    auto buf = buildMinimalEDF(actual_data_records, samples_per_record, samples);
    // Patch the num_records_header field to say "1" instead of "3"
    // Field is at offset 236, length 8
    std::string one_str = "1       ";
    std::memcpy(buf.data() + 236, one_str.data(), 8);

    EDFFile edf;
    ASSERT_TRUE(edf.open(buf.data(), buf.size()));

    EXPECT_EQ(edf.num_records_header, 1);
    EXPECT_EQ(edf.actual_records, 3);
    EXPECT_FALSE(edf.complete);
    EXPECT_TRUE(edf.growing);
    EXPECT_EQ(edf.extra_records, 2);
}

TEST(EDFFileTest, MultipleRecordsReadCorrectly) {
    // 3 records, 2 samples each
    std::vector<int16_t> samples = {0, 16383, -16384, 32767, -32768, 100};
    auto buf = buildMinimalEDF(3, 2, samples);

    EDFFile edf;
    ASSERT_TRUE(edf.open(buf.data(), buf.size()));

    std::vector<double> out;
    int count = edf.readSignal(0, out);

    ASSERT_EQ(count, 6);
    ASSERT_EQ(edf.actual_records, 3);
}

TEST(EDFFileTest, RejectsTooSmallBuffer) {
    std::vector<uint8_t> tiny(100, 0);

    EDFFile edf;
    EXPECT_FALSE(edf.open(tiny.data(), tiny.size()));
}

TEST(EDFFileTest, GetStartTimeReturnsCorrectTimePoint) {
    auto buf = buildMinimalEDF(1, 1, {0}, "15.06.26", "02.30.45");

    EDFFile edf;
    ASSERT_TRUE(edf.open(buf.data(), buf.size()));

    auto tp = edf.getStartTime();
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm* tm = std::localtime(&t);

    EXPECT_EQ(tm->tm_year + 1900, 2026);
    EXPECT_EQ(tm->tm_mon + 1, 6);
    EXPECT_EQ(tm->tm_mday, 15);
    EXPECT_EQ(tm->tm_hour, 2);
    EXPECT_EQ(tm->tm_min, 30);
    EXPECT_EQ(tm->tm_sec, 45);
}

TEST(EDFFileTest, IsEDFPlusDetectsCorrectly) {
    auto buf = buildMinimalEDF(1, 1, {0});

    // Plain EDF (empty reserved)
    EDFFile edf_plain;
    ASSERT_TRUE(edf_plain.open(buf.data(), buf.size()));
    EXPECT_FALSE(edf_plain.isEDFPlus());

    // Patch reserved field to "EDF+C"
    std::string edfplus = "EDF+C";
    for (int i = 0; i < 5; i++) {
        buf[192 + i] = edfplus[i];
    }

    EDFFile edf_plus;
    ASSERT_TRUE(edf_plus.open(buf.data(), buf.size()));
    EXPECT_TRUE(edf_plus.isEDFPlus());
}
