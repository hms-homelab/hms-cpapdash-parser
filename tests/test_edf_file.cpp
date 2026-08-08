#include <gtest/gtest.h>
#include "cpapdash/parser/EDFFile.h"
#include "cpapdash/parser/EDFParser.h"
#include <cstring>
#include <memory>
#include <vector>
#include <cstdint>

using namespace cpapdash::parser;

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

// Multi-signal EDF carrying the given labels in the given order, one sample per
// signal per record. Needed to reproduce label sets where one label is a suffix of
// another, which is the ResMed PLD case.
std::vector<uint8_t> buildLabelledEDF(const std::vector<std::string>& labels) {
    int ns = static_cast<int>(labels.size());
    int header_bytes = 256 + 256 * ns;
    int total_size = header_bytes + ns * 2;   // 1 record, 1 sample per signal

    std::vector<uint8_t> buf(total_size, ' ');
    auto writeField = [&](int offset, int len, const std::string& val) {
        for (int i = 0; i < len; i++)
            buf[offset + i] = (i < static_cast<int>(val.size())) ? val[i] : ' ';
    };

    writeField(0, 8, "0");
    writeField(8, 80, "TestPatient");
    writeField(88, 80, "Startdate 01-MAR-2026");
    writeField(168, 8, "01.03.26");
    writeField(176, 8, "23.30.00");
    writeField(184, 8, std::to_string(header_bytes));
    writeField(192, 44, "");
    writeField(236, 8, "1");
    writeField(244, 8, "1");
    writeField(252, 4, std::to_string(ns));

    // Signal-header fields are stored field-major: all labels, then all transducers...
    int base = 256;
    auto block = [&](int width, const std::string& val) {
        for (int i = 0; i < ns; ++i) writeField(base + i * width, width, val);
        base += ns * width;
    };
    for (int i = 0; i < ns; ++i) writeField(base + i * 16, 16, labels[i]);
    base += ns * 16;
    block(80, "");         // transducer
    block(8, "cmH2O");     // physical dimension
    block(8, "0");         // physical min
    block(8, "100");       // physical max
    block(8, "0");         // digital min
    block(8, "32767");     // digital max
    block(80, "");         // prefiltering
    block(8, "1");         // samples per record
    block(32, "");         // reserved

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

TEST(EDFFileTest, FindSignalPrefixAnchorsAtStart) {
    auto buf = buildLabelledEDF({"TestSig"});

    EDFFile edf;
    ASSERT_TRUE(edf.open(buf.data(), buf.size()));

    EXPECT_EQ(edf.findSignalPrefix("Test"), 0);
    EXPECT_EQ(edf.findSignalPrefix("TestSig"), 0);
    EXPECT_EQ(edf.findSignalPrefix("Sig"), -1);      // substring, not a prefix
    EXPECT_EQ(edf.findSignalPrefix("TestSignal"), -1);  // longer than the label
    EXPECT_EQ(edf.findSignalPrefix("NotHere"), -1);
}

// A ResMed PLD carries three pressure channels and the therapy one is a SUFFIX of
// the mask one, so a substring search silently returns mask pressure where therapy
// pressure was asked for. That mismatch shipped: CpapDash reported mask pressure as
// "Pressure" and read ~0.8 cmH2O below OSCAR and SleepHQ. Pin the distinction.
TEST(EDFFileTest, PressPrefixPicksTherapyNotMaskPressure) {
    auto buf = buildLabelledEDF({"MaskPress.2s", "Press.2s", "EprPress.2s", "Leak.2s"});

    EDFFile edf;
    ASSERT_TRUE(edf.open(buf.data(), buf.size()));

    // The trap, documented: substring search hands back the WRONG channel.
    EXPECT_EQ(edf.findSignal("Press"), 0);
    EXPECT_EQ(edf.signals[0].label, "MaskPress.2s");

    // Prefix search is anchored, so it can only match the therapy channel.
    ASSERT_EQ(edf.findSignalPrefix("Press"), 1);
    EXPECT_EQ(edf.signals[1].label, "Press.2s");

    // The other two stay reachable by their own distinct prefixes.
    EXPECT_EQ(edf.findSignalPrefix("MaskPress"), 0);
    EXPECT_EQ(edf.findSignalPrefix("EprPress"), 2);
    EXPECT_EQ(edf.findSignalPrefix("Leak"), 3);
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

// ── Malformed-input hardening ───────────────────────────────────────────────
//
// Both cases below reach a throwing std::stoi from data an uploader controls.
// In cpapdash-api this parse runs on a detached background thread, where an
// escaping exception is a std::terminate of the entire service rather than one
// failed session — so malformed input must degrade, never throw.

// buildMinimalEDF labels its one signal "TestSig"; the BRP parser requires a
// signal named "Flow" before it will return a session. Signal labels live at
// offset 256, 16 bytes each.
void labelFirstSignalFlow(std::vector<uint8_t>& buf) {
    const std::string label = "Flow";
    for (int i = 0; i < 16; i++)
        buf[256 + i] = (i < static_cast<int>(label.size())) ? label[i] : ' ';
}

// session_start_str comes from an uploaded filename ("YYYYMMDD_HHMMSS"). The
// only guard is a length check (>= 15), which says nothing about the characters
// being digits.
TEST(EDFParserHardening, NonNumericSessionStartDoesNotThrow) {
    auto brp = buildMinimalEDF(1, 4, {100, 200, 300, 400});
    labelFirstSignalFlow(brp);

    std::unique_ptr<ParsedSession> session;
    EXPECT_NO_THROW({
        session = EDFParser::parseSessionFromBuffers(
            brp.data(), brp.size(),
            nullptr, 0, nullptr, 0, nullptr, 0,
            "dev", "Test Device",
            "abcdefgh_ijklmn");  // 15 chars: passes the length guard, not numeric
    });

    // The session still parses. The unusable filename timestamp is skipped, and
    // the BRP header's own start date (01-MAR-2026 23:30, from buildMinimalEDF)
    // supplies session_start instead — so no garbage date is recorded.
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->session_start.has_value());
    const auto tt = std::chrono::system_clock::to_time_t(*session->session_start);
    std::tm tm_start{};
    localtime_r(&tt, &tm_start);
    EXPECT_EQ(tm_start.tm_year + 1900, 2026);
    EXPECT_EQ(tm_start.tm_mon + 1, 3);
    EXPECT_EQ(tm_start.tm_mday, 1);
}

// A digit run long enough to overflow int ("MID=99999999999") satisfies the
// \d+ regex in parseDeviceInfo but overflows std::stoi -> std::out_of_range.
TEST(EDFParserHardening, OverlongModelIdDoesNotThrow) {
    auto brp = buildMinimalEDF(1, 4, {1, 2, 3, 4});
    labelFirstSignalFlow(brp);
    // EDF recording field: 80 bytes at offset 88.
    const std::string rec =
        "Startdate 01-MAR-2026 SRN=23243570851 MID=99999999999 VID=99999999999";
    for (int i = 0; i < 80; i++)
        brp[88 + i] = (i < static_cast<int>(rec.size())) ? rec[i] : ' ';

    std::unique_ptr<ParsedSession> session;
    EXPECT_NO_THROW({
        session = EDFParser::parseSessionFromBuffers(
            brp.data(), brp.size(),
            nullptr, 0, nullptr, 0, nullptr, 0,
            "dev", "Test Device", "");
    });

    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->serial_number, "23243570851");  // a string — unaffected
}
