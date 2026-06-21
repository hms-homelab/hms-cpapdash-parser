#ifdef CPAPDASH_WITH_LOWENSTEIN

#include <gtest/gtest.h>
#include "cpapdash/parser/PrismaParser.h"
#include "cpapdash/parser/EDFFile.h"
#include "cpapdash/parser/ISessionParser.h"
#include <filesystem>
#include <fstream>

namespace cpapdash::parser {

namespace fs = std::filesystem;

static const std::string FIXTURES = "tests/fixtures/lowenstein";

static std::string fixturePath(const std::string& name) {
    // Try relative to build dir first, then source dir
    if (fs::exists(std::string("../") + FIXTURES + "/" + name))
        return std::string("../") + FIXTURES + "/" + name;
    if (fs::exists(FIXTURES + "/" + name))
        return FIXTURES + "/" + name;
    return "";
}

// ── device.xml ──────────────────────────────────────────────────────────────

TEST(PrismaParser, ParseDeviceXml) {
    auto path = fixturePath("device.xml");
    ASSERT_FALSE(path.empty()) << "device.xml fixture not found";

    auto info = PrismaParser::parseDeviceXml(path);
    EXPECT_EQ(info.serial_number, "TESTSN00");
    EXPECT_EQ(info.device_type, 27);
    EXPECT_EQ(info.fw_version, "5.05");
    EXPECT_EQ(info.hw_version, 11);
}

// ── RespEventID mapping ─────────────────────────────────────────────────────

TEST(PrismaParser, RespEventIdMapping) {
    EXPECT_EQ(PrismaParser::mapRespEventId(101), EventType::OBSTRUCTIVE);
    EXPECT_EQ(PrismaParser::mapRespEventId(102), EventType::CENTRAL);
    EXPECT_EQ(PrismaParser::mapRespEventId(103), EventType::APNEA);
    EXPECT_EQ(PrismaParser::mapRespEventId(105), EventType::APNEA);
    EXPECT_EQ(PrismaParser::mapRespEventId(106), EventType::APNEA);
    EXPECT_EQ(PrismaParser::mapRespEventId(111), EventType::HYPOPNEA);
    EXPECT_EQ(PrismaParser::mapRespEventId(112), EventType::HYPOPNEA);
    EXPECT_EQ(PrismaParser::mapRespEventId(113), EventType::HYPOPNEA);
    EXPECT_EQ(PrismaParser::mapRespEventId(121), EventType::RERA);
    EXPECT_EQ(PrismaParser::mapRespEventId(131), EventType::VIBRATORY_SNORE);
    EXPECT_EQ(PrismaParser::mapRespEventId(151), EventType::FLOW_LIMITATION);
    EXPECT_EQ(PrismaParser::mapRespEventId(161), EventType::LARGE_LEAK);
    EXPECT_EQ(PrismaParser::mapRespEventId(181), EventType::CSR);
    EXPECT_FALSE(PrismaParser::mapRespEventId(999).has_value());
    EXPECT_FALSE(PrismaParser::mapRespEventId(1111).has_value());
}

// ── Therapy mode mapping ────────────────────────────────────────────────────

TEST(PrismaParser, TherapyModeMapping) {
    EXPECT_EQ(PrismaParser::mapTherapyMode(1), 0);   // CPAP
    EXPECT_EQ(PrismaParser::mapTherapyMode(2), 1);   // APAP
    EXPECT_EQ(PrismaParser::mapTherapyMode(3), 7);   // AcSV -> ASV
    EXPECT_EQ(PrismaParser::mapTherapyMode(9), 2);   // Auto-S -> BiPAP
    EXPECT_EQ(PrismaParser::mapTherapyMode(10), 2);  // Auto-S/T -> BiPAP
    EXPECT_FALSE(PrismaParser::mapTherapyMode(99).has_value());
}

// ── WMEDF signal file ───────────────────────────────────────────────────────

TEST(PrismaParser, WmedfFileOpens) {
    auto path = fixturePath("signal_000373.wmedf");
    ASSERT_FALSE(path.empty()) << "signal_000373.wmedf fixture not found";

    EDFFile edf;
    ASSERT_TRUE(edf.open(path));

    EXPECT_EQ(edf.num_signals, 18);
    EXPECT_GT(edf.actual_records, 0);
    EXPECT_DOUBLE_EQ(edf.record_duration, 1.0);

    // Check known channel names
    EXPECT_GE(edf.findSignalExact("Pressure"), 0);
    EXPECT_GE(edf.findSignalExact("RespFlow"), 0);
    EXPECT_GE(edf.findSignalExact("LeakFlowBreath"), 0);
    EXPECT_GE(edf.findSignalExact("SpO2"), 0);
    EXPECT_GE(edf.findSignalExact("HeartFrequency"), 0);
    EXPECT_GE(edf.findSignalExact("ObstructLevel"), 0);
    EXPECT_GE(edf.findSignalExact("BreathFrequency"), 0);
    EXPECT_GE(edf.findSignalExact("BreathVolume"), 0);
}

TEST(PrismaParser, Wmedf8BitDetection) {
    auto path = fixturePath("signal_000373.wmedf");
    ASSERT_FALSE(path.empty());

    EDFFile edf;
    ASSERT_TRUE(edf.open(path));

    // Pressure should be 16-bit (#2), most others 8-bit (#1)
    int pressure_idx = edf.findSignalExact("Pressure");
    ASSERT_GE(pressure_idx, 0);
    EXPECT_EQ(edf.signals[pressure_idx].bytes_per_sample, 2);

    int breath_freq_idx = edf.findSignalExact("BreathFrequency");
    ASSERT_GE(breath_freq_idx, 0);
    EXPECT_EQ(edf.signals[breath_freq_idx].bytes_per_sample, 1);
}

TEST(PrismaParser, WmedfReadSignals) {
    auto path = fixturePath("signal_000373.wmedf");
    ASSERT_FALSE(path.empty());

    EDFFile edf;
    ASSERT_TRUE(edf.open(path));

    // Read pressure (16-bit)
    std::vector<double> pressure;
    int pressure_idx = edf.findSignalExact("Pressure");
    int count = edf.readSignal(pressure_idx, pressure);
    EXPECT_GT(count, 0);

    // Read breath frequency (8-bit)
    std::vector<double> breath_freq;
    int bf_idx = edf.findSignalExact("BreathFrequency");
    count = edf.readSignal(bf_idx, breath_freq);
    EXPECT_GT(count, 0);

    // Read SpO2 (8-bit)
    std::vector<double> spo2;
    int spo2_idx = edf.findSignalExact("SpO2");
    count = edf.readSignal(spo2_idx, spo2);
    EXPECT_GT(count, 0);
}

// ── Event XML parsing ───────────────────────────────────────────────────────

TEST(PrismaParser, ParseEventXml) {
    auto path = fixturePath("event_000373.xml");
    ASSERT_FALSE(path.empty()) << "event_000373.xml fixture not found";

    ParsedSession session;
    auto now = std::chrono::system_clock::now();
    ASSERT_TRUE(PrismaParser::parseEventXml(path, session, now));

    EXPECT_GT(session.events.size(), 0u);

    // Should have device settings from DeviceEvent elements
    ASSERT_TRUE(session.settings.has_value());
    EXPECT_TRUE(session.settings->therapy_mode.has_value());
}

TEST(PrismaParser, ParseEventXmlSmall) {
    auto path = fixturePath("event_000351.xml");
    ASSERT_FALSE(path.empty());

    ParsedSession session;
    auto now = std::chrono::system_clock::now();
    ASSERT_TRUE(PrismaParser::parseEventXml(path, session, now));

    EXPECT_GT(session.events.size(), 0u);
}

// SMART max writes RespEvent attributes spaced out (RespEventID = "101"); the
// extractor must handle both that and the tight DeviceEvent form in one file.
TEST(PrismaParser, ParseEventXmlSpacedAttributes) {
    auto tmp = fs::temp_directory_path() / "prisma_spaced_event.xml";
    {
        std::ofstream out(tmp);
        out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<desc>\n"
            << "<DeviceEvent  DeviceEventID=\"0\" Time=\"0\" ParameterID=\"1003\" NewValue=\"2\"/>\n"
            << "<RespEvent RespEventID = \"101\" EndTime = \"120\" Duration = \"15\"/>\n"
            << "<RespEvent RespEventID = \"102\" EndTime = \"240\" Duration = \"12\"/>\n"
            << "<RespEvent RespEventID = \"111\" EndTime = \"360\" Duration = \"20\"/>\n"
            << "<RespEvent RespEventID = \"121\" EndTime = \"480\" Duration = \"8\"/>\n"
            << "<RespEvent RespEventID = \"1230\" EndTime = \"1\" Duration = \"10\"/>\n"
            << "</desc>\n";
    }
    ParsedSession session;
    auto now = std::chrono::system_clock::now();
    ASSERT_TRUE(PrismaParser::parseEventXml(tmp.string(), session, now));

    // 4 clinical events parsed (OA, CA, hypopnea, RERA); 1230 is unmapped → skipped.
    EXPECT_EQ(session.events.size(), 4u);
    // DeviceEvent (tight form) still parsed in the same file.
    ASSERT_TRUE(session.settings.has_value());
    EXPECT_TRUE(session.settings->therapy_mode.has_value());

    fs::remove(tmp);
}

// ── Full session parse ──────────────────────────────────────────────────────

TEST(PrismaParser, FullSessionParse) {
    auto event_path = fixturePath("event_000373.xml");
    auto signal_path = fixturePath("signal_000373.wmedf");
    auto device_path = fixturePath("device.xml");
    ASSERT_FALSE(event_path.empty());
    ASSERT_FALSE(signal_path.empty());

    // Create a temp directory with the session files
    auto tmp = fs::temp_directory_path() / "prisma_test_session";
    fs::create_directories(tmp);
    fs::copy_file(event_path, tmp / "event_000373.xml", fs::copy_options::overwrite_existing);
    fs::copy_file(signal_path, tmp / "signal_000373.wmedf", fs::copy_options::overwrite_existing);
    if (!device_path.empty())
        fs::copy_file(device_path, tmp / "device.xml", fs::copy_options::overwrite_existing);

    PrismaParser parser;
    auto session = parser.parseSession(tmp.string(), "prisma_test", "Prisma 20A");

    fs::remove_all(tmp);

    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->manufacturer, DeviceManufacturer::LOWENSTEIN);
    EXPECT_EQ(session->device_id, "prisma_test");
    EXPECT_EQ(session->serial_number, "TESTSN00");
    EXPECT_TRUE(session->session_start.has_value());
    EXPECT_TRUE(session->session_end.has_value());
    EXPECT_TRUE(session->duration_seconds.has_value());
    EXPECT_GT(*session->duration_seconds, 0);
    EXPECT_GT(session->events.size(), 0u);
    EXPECT_GT(session->breathing_summary.size(), 0u);
    EXPECT_TRUE(session->has_events);
    EXPECT_EQ(session->status, ParsedSession::Status::COMPLETED);

    ASSERT_TRUE(session->metrics.has_value());
    EXPECT_GE(session->metrics->ahi, 0.0);
    EXPECT_GT(session->metrics->total_events, 0);
}

// ── Factory detection ───────────────────────────────────────────────────────

TEST(PrismaParser, FactoryCreatesLowenstein) {
    auto parser = createParser(DeviceManufacturer::LOWENSTEIN);
    ASSERT_NE(parser, nullptr);
    EXPECT_EQ(parser->manufacturer(), DeviceManufacturer::LOWENSTEIN);
}

TEST(PrismaParser, FactoryAutoDetectsFromDir) {
    auto tmp = fs::temp_directory_path() / "prisma_detect_test";
    fs::create_directories(tmp);

    // Create dummy files to trigger detection
    std::ofstream(tmp / "event_000001.xml") << "<desc></desc>";
    std::ofstream(tmp / "signal_000001.wmedf") << "dummy";

    auto parser = createParser(tmp.string());
    fs::remove_all(tmp);

    ASSERT_NE(parser, nullptr);
    EXPECT_EQ(parser->manufacturer(), DeviceManufacturer::LOWENSTEIN);
}

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_LOWENSTEIN
