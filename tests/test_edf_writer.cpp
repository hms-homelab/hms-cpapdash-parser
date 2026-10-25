// A night that reaches us as a live stream lands as the files a card would
// have carried. These tests prove the writer's files are what this library's
// own readers take: a plain EDF round-trips through EDFFile, and a BRP and PLD
// laid out from an AirSense 11's stream (80 s with the blower running and
// nobody on the mask) parse into a session with the machine's serial, its
// breathing minute and its slow channels, exactly as a card's files would.
#include <gtest/gtest.h>

#include "cpapdash/parser/EDFFile.h"
#include "cpapdash/parser/EDFParser.h"
#include "cpapdash/parser/EDFWriter.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace cpapdash::parser;

namespace {

std::string fixturePath(const std::string& name) {
    return std::string(CPAPDASH_PARSER_SOURCE_DIR) + "/tests/fixtures/" + name;
}

// One CSV column set per line, '#' lines and the header skipped; "null" is NaN.
std::vector<std::vector<double>> readCsv(const std::string& path, size_t columns) {
    std::vector<std::vector<double>> cols(columns);
    std::ifstream in(path);
    std::string line;
    bool header_seen = false;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!header_seen) { header_seen = true; continue; }
        std::stringstream ss(line);
        std::string cell;
        size_t c = 0;
        while (std::getline(ss, cell, ',') && c < columns) {
            cols[c++].push_back(cell == "null" ? std::nan("") : std::stod(cell));
        }
    }
    return cols;
}

// A made-up serial in the machine's eleven-digit shape.
const std::string kSerial = "12345678901";

WallClock sampleStart() {
    // The machine's own clock, as the card would have named the file.
    return WallClock{2026, 10, 17, 6, 44, 52};
}

}  // namespace

TEST(EDFWriter, APlainEDFRoundTripsThroughTheReader) {
    EDFWriteSpec spec;
    spec.recording       = "Startdate 17-OCT-2026 X X X SRN=" + kSerial;
    spec.start           = sampleStart();
    spec.record_duration = 1.0;
    EDFWriteSignal s;
    s.label              = "Flow.40ms";
    s.phys_dim           = "L/min";
    s.phys_min           = -300;
    s.phys_max           = 300;
    s.samples_per_record = 25;
    for (int i = 0; i < 25 * 3; i++) s.samples.push_back(50.0 * std::sin(i * 0.2));
    spec.signals.push_back(s);

    const auto bytes = writeEDF(spec);
    ASSERT_EQ(bytes.size(), 256u + 256u + 3u * 25u * 2u);
    EDFFile edf;
    ASSERT_TRUE(edf.open(bytes.data(), bytes.size()));
    EXPECT_EQ(edf.num_signals, 1);
    EXPECT_EQ(edf.num_records_header, 3);
    EXPECT_DOUBLE_EQ(edf.record_duration, 1.0);
    EXPECT_EQ(edf.start_year, 2026);
    EXPECT_EQ(edf.start_month, 10);
    EXPECT_EQ(edf.start_day, 17);
    EXPECT_EQ(edf.start_hour, 6);
    EXPECT_EQ(edf.start_minute, 44);
    EXPECT_EQ(edf.start_second, 52);
    EXPECT_EQ(edf.recording, "Startdate 17-OCT-2026 X X X SRN=" + kSerial);
    EXPECT_TRUE(edf.complete);
    const int idx = edf.findSignal("Flow");
    ASSERT_GE(idx, 0);
    EXPECT_EQ(edf.signals[idx].samples_per_record, 25);
    EXPECT_EQ(edf.signals[idx].phys_dim, "L/min");
    std::vector<double> back;
    ASSERT_EQ(edf.readSignal(idx, back), 75);
    for (int i = 0; i < 75; i++) EXPECT_NEAR(back[i], s.samples[i], 0.01) << "sample " << i;
}

TEST(EDFWriter, TrailingSamplesThatDoNotFillARecordAreDropped) {
    EDFWriteSpec spec;
    spec.start = WallClock{2026, 1, 1, 0, 0, 0};
    EDFWriteSignal s;
    s.label = "Press.40ms"; s.phys_dim = "cmH2O"; s.phys_min = 0; s.phys_max = 40;
    s.samples_per_record = 25;
    s.samples.assign(25 * 3 + 7, 8.0);
    spec.signals.push_back(s);
    const auto bytes = writeEDF(spec);
    EDFFile edf;
    ASSERT_TRUE(edf.open(bytes.data(), bytes.size()));
    EXPECT_EQ(edf.num_records_header, 3);
    EXPECT_TRUE(writeEDF(EDFWriteSpec{}).empty()) << "no signal, no file";
}

TEST(EDFWriter, ValuesAreClampedToTheSignalsRange) {
    EDFWriteSpec spec;
    spec.start = WallClock{2026, 1, 1, 0, 0, 0};
    EDFWriteSignal s;
    s.label = "Press.40ms"; s.phys_dim = "cmH2O"; s.phys_min = 0; s.phys_max = 40;
    s.samples_per_record = 1;
    s.samples = {-5.0, 20.0, 99.0, std::nan("")};
    spec.signals.push_back(s);
    const auto bytes = writeEDF(spec);
    EDFFile edf;
    ASSERT_TRUE(edf.open(bytes.data(), bytes.size()));
    std::vector<double> back;
    ASSERT_EQ(edf.readSignal(0, back), 4);
    EXPECT_NEAR(back[0], 0.0, 0.001);
    EXPECT_NEAR(back[1], 20.0, 0.001);
    EXPECT_NEAR(back[2], 40.0, 0.001);
    EXPECT_NEAR(back[3], 0.0, 0.001) << "a NaN reads as zero, never as garbage";
}

TEST(EDFWriter, TheResMedNamesAndRecordingField) {
    EXPECT_EQ(resmedFileName(sampleStart(), "BRP"), "20261017_064452_BRP.edf");
    EXPECT_EQ(resmedFileName(WallClock{2026, 1, 2, 3, 4, 5}, "PLD"), "20260102_030405_PLD.edf");
    EXPECT_EQ(resmedRecordingField(sampleStart(), kSerial),
              "Startdate 17-OCT-2026 X X X SRN=" + kSerial);
    EXPECT_EQ(resmedRecordingField(sampleStart(), kSerial, 39523, 2),
              "Startdate 17-OCT-2026 X X X SRN=" + kSerial + " MID=39523 VID=2");
    EXPECT_EQ(resmedRecordingField(sampleStart(), ""), "Startdate 17-OCT-2026 X X X");
}

TEST(EDFWriter, AStreamLandsAsACardSession) {
    const auto fast = readCsv(fixturePath("as11_stream_sample_fast.csv"), 2);
    const auto slow = readCsv(fixturePath("as11_stream_sample_slow.csv"), 9);
    ASSERT_EQ(fast[0].size(), 2000u) << "80 s at 25 Hz";
    ASSERT_EQ(slow[0].size(), 40u) << "80 s per 2 s";

    StreamedBRP brp;
    brp.start  = sampleStart();
    brp.serial = kSerial;
    for (double v : fast[0]) brp.flow_lpm.push_back(v * 60.0);   // the stream's L/s
    brp.press_cmh2o = fast[1];
    const auto brp_bytes = writeResMedBRP(brp);
    ASSERT_FALSE(brp_bytes.empty());

    StreamedPLD pld;
    pld.start            = sampleStart();
    pld.serial           = kSerial;
    pld.mask_press_cmh2o = slow[0];
    pld.press_cmh2o      = slow[1];
    pld.epr_press_cmh2o  = slow[2];
    pld.leak_lps         = slow[3];
    pld.resp_rate        = slow[4];
    pld.tidal_volume_l   = slow[5];
    pld.minute_vent_lpm  = slow[6];
    pld.snore            = slow[7];
    pld.flow_limitation  = slow[8];
    const auto pld_bytes = writeResMedPLD(pld);
    ASSERT_FALSE(pld_bytes.empty());

    // The files as the readers see them.
    {
        EDFFile edf;
        ASSERT_TRUE(edf.open(brp_bytes.data(), brp_bytes.size()));
        EXPECT_EQ(edf.num_records_header, 80);
        EXPECT_GE(edf.findSignal("Flow"), 0);
        EXPECT_GE(edf.findSignalPrefix("Press"), 0);
    }
    {
        EDFFile edf;
        ASSERT_TRUE(edf.open(pld_bytes.data(), pld_bytes.size()));
        EXPECT_EQ(edf.num_records_header, 40);
        EXPECT_DOUBLE_EQ(edf.record_duration, 2.0);
        for (const char* label : {"MaskPress", "EprPress", "Leak", "RespRate", "TidVol", "MinVent",
                                  "Snore", "FlowLim"})
            EXPECT_GE(edf.findSignal(label), 0) << label;
        EXPECT_GE(edf.findSignalPrefix("Press"), 0);
    }

    // And as a session, the way a card's night is parsed.
    auto session = EDFParser::parseSessionFromBuffers(
        brp_bytes.data(), brp_bytes.size(), pld_bytes.data(), pld_bytes.size(), nullptr, 0,
        std::vector<EDFParser::ByteView>{}, "device-1", "sample", "20261017_064452");
    ASSERT_TRUE(session);
    EXPECT_EQ(session->serial_number, kSerial);
    ASSERT_TRUE(session->session_start.has_value());
    EXPECT_EQ(session->data_records, 80);
    EXPECT_TRUE(session->file_complete);
    ASSERT_GE(session->breathing_summary.size(), 1u) << "80 s holds one whole minute";
    const auto& minute = session->breathing_summary.front();
    EXPECT_GT(minute.max_flow_rate, 0.0) << "the blower's flow was there";
    EXPECT_GE(minute.max_pressure, 0.0);
}

TEST(EDFWriter, TheRingLandsAsASADFileBesideTheBreathing) {
    const auto fast = readCsv(fixturePath("as11_stream_sample_fast.csv"), 2);
    ASSERT_EQ(fast[0].size(), 2000u);
    StreamedBRP brp;
    brp.start  = sampleStart();
    brp.serial = kSerial;
    for (double v : fast[0]) brp.flow_lpm.push_back(v * 60.0);
    brp.press_cmh2o = fast[1];
    const auto brp_bytes = writeResMedBRP(brp);
    ASSERT_FALSE(brp_bytes.empty());

    // 80 s of a ring read every 2 s, each reading held for its two seconds;
    // seconds 10 and 11 had no reading (the ring off the finger).
    StreamedSAD sad;
    sad.start  = sampleStart();
    sad.serial = kSerial;
    for (int s = 0; s < 80; s++) {
        const bool none = (s == 10 || s == 11);
        sad.spo2_pct.push_back(none ? std::nan("") : 96.0 + (s / 2) % 3);
        sad.pulse_bpm.push_back(none ? std::nan("") : 60.0 + (s / 2) % 5);
    }
    const auto sad_bytes = writeResMedSAD(sad);
    ASSERT_FALSE(sad_bytes.empty());

    {
        EDFFile edf;
        ASSERT_TRUE(edf.open(sad_bytes.data(), sad_bytes.size()));
        EXPECT_EQ(edf.num_records_header, 80);
        EXPECT_DOUBLE_EQ(edf.record_duration, 1.0);
        EXPECT_EQ(edf.recording, "Startdate 17-OCT-2026 X X X SRN=" + kSerial);
        ASSERT_GE(edf.findSignal("SpO2"), 0);
        ASSERT_GE(edf.findSignal("Pulse"), 0);
        std::vector<double> spo2;
        ASSERT_EQ(edf.readSignal(edf.findSignal("SpO2"), spo2), 80);
        EXPECT_NEAR(spo2[0], 96.0, 0.01);
        EXPECT_NEAR(spo2[2], 97.0, 0.01);
        EXPECT_NEAR(spo2[10], 0.0, 0.01) << "no reading is written as 0";
    }

    // Parsed as a card's session: the vitals are per second, and the seconds
    // without a reading carry none (not a zero).
    auto session = EDFParser::parseSessionFromBuffers(
        brp_bytes.data(), brp_bytes.size(), nullptr, 0, sad_bytes.data(), sad_bytes.size(),
        std::vector<EDFParser::ByteView>{}, "device-1", "sample", "20261017_064452");
    ASSERT_TRUE(session);
    ASSERT_EQ(session->vitals.size(), 80u);
    ASSERT_TRUE(session->vitals[0].spo2.has_value());
    EXPECT_NEAR(*session->vitals[0].spo2, 96.0, 0.01);
    ASSERT_TRUE(session->vitals[0].heart_rate.has_value());
    EXPECT_EQ(*session->vitals[0].heart_rate, 60);
    EXPECT_FALSE(session->vitals[10].spo2.has_value());
    EXPECT_FALSE(session->vitals[11].heart_rate.has_value());
    EXPECT_TRUE(session->vitals[12].spo2.has_value());
}

TEST(EDFWriter, ASADWithNoReadingsIsNoFile) {
    StreamedSAD sad;
    sad.start  = sampleStart();
    sad.serial = kSerial;
    EXPECT_TRUE(writeResMedSAD(sad).empty());
    sad.pulse_bpm = {62.0, 63.0};
    const auto bytes = writeResMedSAD(sad);
    ASSERT_FALSE(bytes.empty()) << "one signal is enough; the other is padded with no-reading";
    EDFFile edf;
    ASSERT_TRUE(edf.open(bytes.data(), bytes.size()));
    EXPECT_EQ(edf.num_records_header, 2);
    std::vector<double> spo2;
    ASSERT_EQ(edf.readSignal(edf.findSignal("SpO2"), spo2), 2);
    EXPECT_NEAR(spo2[1], 0.0, 0.01);
}

// ── EVE / CSL: the machine's event files ─────────────────────────────────────

TEST(EDFWriter, AnEveIsTheMachinesLayoutRecordForRecord) {
    // Two records and their CRCs as an AirSense 11 wrote them on its own EVE
    // (the CRC is the only thing here not derived from the format itself).
    ResMedAnnotationFile f;
    f.start = WallClock{2026, 10, 25, 9, 35, 15};
    f.serial = kSerial;
    f.annotations.push_back({679, 21, "Obstructive Apnea"});
    const auto b = writeResMedAnnotations(f);
    ASSERT_EQ(b.size(), 768u + 2u * 64u) << "a 768-byte header, a 64-byte record each";
    const std::string hdr(b.begin(), b.begin() + 256);
    EXPECT_EQ(hdr.substr(192, 5), "EDF+D");
    EXPECT_EQ(hdr.substr(236, 8), "2       ") << "Recording starts, then the event";

    const std::string r0(b.begin() + 768, b.begin() + 768 + 64);
    const std::string want0 = std::string("+0\x14\x14\0+0\x15", 8) + "0\x14Recording starts\x14";
    EXPECT_EQ(r0.substr(0, want0.size()), want0);
    EXPECT_EQ(static_cast<uint8_t>(r0[62]), 0x94);
    EXPECT_EQ(static_cast<uint8_t>(r0[63]), 0xD7);

    const std::string r1(b.begin() + 768 + 64, b.begin() + 768 + 128);
    const std::string want1 = std::string("+0\x14\x14\0+679\x15", 10) + "21\x14Obstructive Apnea\x14";
    EXPECT_EQ(r1.substr(0, want1.size()), want1);
    EXPECT_EQ(static_cast<uint8_t>(r1[62]), 0xD6);
    EXPECT_EQ(static_cast<uint8_t>(r1[63]), 0x3F);
}

TEST(EDFWriter, AWrittenEveReadsBackAsTheMachinesAnnotationsOnItsClock) {
    ResMedAnnotationFile f;
    f.start = sampleStart();
    f.serial = kSerial;
    f.model_id = 46;
    f.annotations.push_back({120, 21, "Obstructive Apnea"});
    f.annotations.push_back({300, 11, "Central Apnea"});
    f.annotations.push_back({450, 0, "Hypopnea"});
    const auto b = writeResMedAnnotations(f);

    EDFFile edf;
    ASSERT_TRUE(edf.open(b.data(), b.size()));
    EXPECT_TRUE(edf.isEDFPlus());
    EXPECT_NE(edf.recording.find("SRN=" + kSerial + " MID=46"), std::string::npos);
    // The reader leaves the "Recording starts" marker out, as for a card's.
    const auto ann = edf.readAnnotations();
    ASSERT_EQ(ann.size(), 3u);
    EXPECT_EQ(ann[0].description, "Obstructive Apnea");
    EXPECT_DOUBLE_EQ(ann[0].onset_sec, 120.0) << "stamped at its end";
    EXPECT_DOUBLE_EQ(ann[0].duration_sec, 21.0);
    EXPECT_EQ(ann[1].description, "Central Apnea");
    EXPECT_EQ(ann[2].description, "Hypopnea");
    EXPECT_DOUBLE_EQ(ann[2].duration_sec, 0.0);
}

TEST(EDFWriter, ACslCarriesTheMachinesCsrMarkersAndAnEmptyOneOnlyItsStart) {
    ResMedAnnotationFile f;
    f.start = sampleStart();
    f.serial = kSerial;
    EXPECT_EQ(writeResMedAnnotations(f).size(), 768u + 64u) << "an event-free night: Recording starts";
    f.annotations.push_back({7687, 0, "CSR Start"});
    f.annotations.push_back({8400, 0, "CSR End"});
    const auto b = writeResMedAnnotations(f);
    EDFFile edf;
    ASSERT_TRUE(edf.open(b.data(), b.size()));
    const auto ann = edf.readAnnotations();
    ASSERT_EQ(ann.size(), 2u);
    EXPECT_EQ(ann[0].description, "CSR Start");
    EXPECT_DOUBLE_EQ(ann[0].onset_sec, 7687.0);
    EXPECT_EQ(ann[1].description, "CSR End");
}

TEST(EDFWriter, AnAnnotationTooLongForARecordIsCutToFitNotSpilled) {
    ResMedAnnotationFile f;
    f.start = sampleStart();
    f.annotations.push_back({100000, 120, std::string(80, 'x')});
    const auto b = writeResMedAnnotations(f);
    ASSERT_EQ(b.size(), 768u + 2u * 64u);
    const std::string r1(b.begin() + 768 + 64, b.begin() + 768 + 126);
    EXPECT_EQ(r1.back(), '\0') << "the record still closes inside its 62 bytes";
    EDFFile edf;
    ASSERT_TRUE(edf.open(b.data(), b.size()));
    EXPECT_EQ(edf.readAnnotations().size(), 1u);
}
