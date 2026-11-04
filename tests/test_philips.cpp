#ifdef CPAPDASH_WITH_PHILIPS

#include <gtest/gtest.h>
#include "cpapdash/parser/PhilipsParser.h"
#include "cpapdash/parser/ISessionParser.h"
#include "cpapdash/parser/Models.h"

#include <filesystem>
#include <fstream>
#include <vector>

using namespace cpapdash::parser;
namespace fs = std::filesystem;

namespace {

const std::string FIXTURES = "tests/fixtures/philips";

// Fixture epoch used by gen/make_fixtures.py
constexpr int64_t kBaseTs = 1750000000;
constexpr int64_t kMaskOn = kBaseTs + 60;

#ifndef CPAPDASH_PARSER_SOURCE_DIR
#define CPAPDASH_PARSER_SOURCE_DIR "."
#endif

// The source dir first, so ctest (which runs from the build dir, however deep)
// and a run from the repository root both find the fixtures.
std::string fixturePath(const std::string& name) {
    const std::string rel = FIXTURES + "/" + name;
    for (const std::string& base : {std::string(CPAPDASH_PARSER_SOURCE_DIR) + "/",
                                    std::string(""),
                                    std::string("../"),
                                    std::string("../../")}) {
        std::error_code ec;
        if (fs::exists(base + rel, ec)) return base + rel;
    }
    return "";
}

std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

std::chrono::system_clock::time_point tp(int64_t unix_secs) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(unix_secs));
}

// Copy a fixture dir's contents into a fresh temp dir (Prisma convention).
fs::path stageDir(const std::string& fixture_dir, const std::string& tag) {
    auto tmp = fs::temp_directory_path() / ("philips_test_" + tag);
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    for (const auto& entry : fs::directory_iterator(fixturePath(fixture_dir))) {
        if (entry.is_regular_file()) {
            fs::copy_file(entry.path(), tmp / entry.path().filename());
        }
    }
    return tmp;
}

} // namespace

// ── Phase 1: identity + container ───────────────────────────────────────────

TEST(PhilipsParser, ParseProperties) {
    auto path = fixturePath("ds1/Properties.txt");
    ASSERT_FALSE(path.empty());

    auto info = PhilipsParser::parseProperties(path);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->serial_number, "PXTEST0000DS1");
    EXPECT_EQ(info->model_number, "500X110");
    EXPECT_EQ(info->family, 0);
    EXPECT_EQ(info->family_version, 4);
    EXPECT_EQ(info->data_format_version, "2");
    EXPECT_EQ(info->patient_folder_num, 0);
}

TEST(PhilipsParser, ModelNameLookup) {
    EXPECT_EQ(PhilipsParser::modelName("560P").value(),
              "REMstar Auto (System One 60 Series)");
    EXPECT_EQ(PhilipsParser::modelName("500X110").value(), "DreamStation Auto CPAP");
    EXPECT_FALSE(PhilipsParser::modelName("999ZZZ").has_value());
}

TEST(PhilipsParser, ChunkFraming) {
    auto bytes = readBytes(fixturePath("ds1/0000000001.005"));
    ASSERT_FALSE(bytes.empty());

    PhilipsParseWarnings w;
    auto chunks = PhilipsParser::readChunks(bytes.data(), bytes.size(), w);

    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(w.total(), 0);

    const auto& c = chunks[0];
    EXPECT_EQ(c.data_format_version, 2);
    EXPECT_EQ(c.file_type, 1);
    EXPECT_EQ(c.family, 0);
    EXPECT_EQ(c.family_version, 4);
    EXPECT_EQ(c.file_extension, 5);
    EXPECT_EQ(c.session_number, 1u);
    EXPECT_EQ(c.timestamp, tp(kMaskOn));
    EXPECT_EQ(c.interval_records, 30);
    EXPECT_EQ(c.seconds_per_interval, 10);
    ASSERT_EQ(c.waveforms.size(), 1u);
    EXPECT_EQ(c.durationSeconds(), 300);
    EXPECT_EQ(c.data.size(), 1500u);  // 5 Hz * 300 s

    EXPECT_EQ(chunks[1].timestamp, tp(kMaskOn + 310));
}

TEST(PhilipsParser, ChunkCrcFailureSkipsChunk) {
    auto bytes = readBytes(fixturePath("badcrc/0000000009.001"));
    ASSERT_FALSE(bytes.empty());

    PhilipsParseWarnings w;
    auto chunks = PhilipsParser::readChunks(bytes.data(), bytes.size(), w);

    EXPECT_TRUE(chunks.empty());
    EXPECT_EQ(w.crc_failures, 1);
}

TEST(PhilipsParser, ChunkTruncatedTolerated) {
    auto bytes = readBytes(fixturePath("truncated/0000000010.005"));
    ASSERT_FALSE(bytes.empty());

    PhilipsParseWarnings w;
    auto chunks = PhilipsParser::readChunks(bytes.data(), bytes.size(), w);

    EXPECT_EQ(chunks.size(), 1u);  // the good chunk survives
    EXPECT_EQ(w.truncated_chunks, 1);
}

TEST(PhilipsParser, DataFormatVersion3Rejected) {
    auto bytes = readBytes(fixturePath("ds2/0000000012.001"));
    ASSERT_FALSE(bytes.empty());

    PhilipsParseWarnings w;
    auto chunks = PhilipsParser::readChunks(bytes.data(), bytes.size(), w);

    EXPECT_TRUE(chunks.empty());
    EXPECT_GE(w.unsupported_chunks, 1);
}

TEST(PhilipsParser, SessionDiscoveryGrouping) {
    auto groups = PhilipsParser::discoverSessions(fixturePath("ds1"));

    ASSERT_EQ(groups.size(), 2u);
    EXPECT_EQ(groups[0].session_id, "0000000000");
    EXPECT_TRUE(groups[0].summary_path.empty());
    EXPECT_FALSE(groups[0].waveform_path.empty());
    EXPECT_EQ(groups[1].session_id, "0000000001");
    EXPECT_FALSE(groups[1].summary_path.empty());
    EXPECT_FALSE(groups[1].events_path.empty());
    EXPECT_FALSE(groups[1].waveform_path.empty());
}

// ── Phase 2: summary + events ───────────────────────────────────────────────

TEST(PhilipsParser, TherapyModeMapping) {
    EXPECT_EQ(PhilipsParser::mapTherapyMode(0x00), 0);
    EXPECT_EQ(PhilipsParser::mapTherapyMode(0xA0), 0);
    EXPECT_EQ(PhilipsParser::mapTherapyMode(0x40), 1);
    EXPECT_EQ(PhilipsParser::mapTherapyMode(0x80), 1);
    EXPECT_EQ(PhilipsParser::mapTherapyMode(0x20), 2);
    EXPECT_EQ(PhilipsParser::mapTherapyMode(0x60), 2);
    EXPECT_FALSE(PhilipsParser::mapTherapyMode(0x55).has_value());
}

TEST(PhilipsParser, EventCodeMapping) {
    EXPECT_EQ(PhilipsParser::mapFamily0EventCode(0x05), EventType::RERA);
    EXPECT_EQ(PhilipsParser::mapFamily0EventCode(0x06), EventType::OBSTRUCTIVE);
    EXPECT_EQ(PhilipsParser::mapFamily0EventCode(0x07), EventType::CLEAR_AIRWAY);
    EXPECT_EQ(PhilipsParser::mapFamily0EventCode(0x0A), EventType::HYPOPNEA);
    EXPECT_EQ(PhilipsParser::mapFamily0EventCode(0x0B), EventType::HYPOPNEA);
    EXPECT_EQ(PhilipsParser::mapFamily0EventCode(0x0C), EventType::FLOW_LIMITATION);
    EXPECT_EQ(PhilipsParser::mapFamily0EventCode(0x0D), EventType::VIBRATORY_SNORE);
    EXPECT_EQ(PhilipsParser::mapFamily0EventCode(0x0F), EventType::PERIODIC_BREATHING);
    EXPECT_EQ(PhilipsParser::mapFamily0EventCode(0x10), EventType::LARGE_LEAK);
    EXPECT_FALSE(PhilipsParser::mapFamily0EventCode(0x01).has_value());  // stat, not event
    EXPECT_FALSE(PhilipsParser::mapFamily0EventCode(0x7F).has_value());
}

TEST(PhilipsParser, SummaryFields) {
    auto bytes = readBytes(fixturePath("ds1/0000000001.001"));
    PhilipsParseWarnings w;
    auto chunks = PhilipsParser::readChunks(bytes.data(), bytes.size(), w);
    ASSERT_EQ(chunks.size(), 1u);

    ParsedSession session;
    ASSERT_TRUE(PhilipsParser::parseSummary(chunks, session, w));
    EXPECT_EQ(w.total(), 0);

    // mask on at base+60, mask off 6h later (generator values)
    EXPECT_EQ(session.session_start.value(), tp(kMaskOn));
    EXPECT_EQ(session.session_end.value(), tp(kMaskOn + 6 * 3600));
    EXPECT_EQ(session.duration_seconds.value(), 6 * 3600);

    ASSERT_TRUE(session.settings.has_value());
    const auto& s = session.settings.value();
    EXPECT_EQ(s.therapy_mode, 1);  // APAP
    EXPECT_NEAR(s.min_pressure.value(), 4.0, 1e-9);   // tick conversion (0.1 cmH2O)
    EXPECT_NEAR(s.max_pressure.value(), 12.0, 1e-9);
    EXPECT_EQ(s.ramp_time, 15);
    EXPECT_NEAR(s.ramp_pressure.value(), 4.0, 1e-9);
    EXPECT_EQ(s.flex_mode, 2);   // A-Flex (plus mode on APAP)
    EXPECT_EQ(s.flex_level, 2);
    EXPECT_EQ(s.humidifier_mode, 1);  // fixed
    EXPECT_EQ(s.humidifier_level, 3);
    EXPECT_EQ(s.hose_diameter, 22);
}

TEST(PhilipsParser, EventOnsetConversion) {
    auto bytes = readBytes(fixturePath("ds1/0000000001.002"));
    PhilipsParseWarnings w;
    auto chunks = PhilipsParser::readChunks(bytes.data(), bytes.size(), w);
    ASSERT_EQ(chunks.size(), 1u);

    ParsedSession session;
    std::vector<PhilipsParser::EventStat> stats;
    ASSERT_TRUE(PhilipsParser::parseEvents(chunks, session, stats, w));
    EXPECT_EQ(w.total(), 0);

    // Generator: OA at delta 3600 after 0x01 stat at delta 0, elapsed 20.
    // Onset = maskOn + 3600 - 20; duration stays 0 (elapsed is back-distance).
    ASSERT_FALSE(session.events.empty());
    const auto& oa = session.events[0];
    EXPECT_EQ(oa.event_type, EventType::OBSTRUCTIVE);
    EXPECT_EQ(oa.timestamp, tp(kMaskOn + 3600 - 20));
    EXPECT_EQ(oa.duration_seconds, 0.0);

    // Hypopnea type 2 (code 0x0B): duration 18, elapsed 10
    const auto& h2 = session.events[2];
    EXPECT_EQ(h2.event_type, EventType::HYPOPNEA);
    EXPECT_EQ(h2.duration_seconds, 18.0);
    EXPECT_EQ(h2.timestamp, tp(kMaskOn + 3600 + 600 + 300 - 10));

    // Periodic breathing (0x0F): stored duration u16=30 doubles to 60 s,
    // onset = clock - elapsed - duration
    const auto& pb = session.events[3];
    EXPECT_EQ(pb.event_type, EventType::PERIODIC_BREATHING);
    EXPECT_EQ(pb.duration_seconds, 60.0);
    EXPECT_EQ(pb.timestamp, tp(kMaskOn + 3600 + 600 + 300 + 600 - 5 - 60));

    // Stats records (0x11) captured with tick-converted EPAP
    ASSERT_EQ(stats.size(), 2u);
    EXPECT_EQ(stats[0].timestamp, tp(kMaskOn + 120));
    EXPECT_EQ(stats[0].total_leak, 30.0);
    EXPECT_EQ(stats[0].snore, 2.0);
    EXPECT_NEAR(stats[0].epap, 4.8, 1e-9);
    EXPECT_EQ(stats[1].total_leak, 25.0);

    // Full census: OA, H1, H2, PB, LL, RERA, CA, FL, VS = 9 events
    EXPECT_EQ(session.events.size(), 9u);
}

TEST(PhilipsParser, UnknownEventCodeTolerated) {
    auto bytes = readBytes(fixturePath("unknowncode/0000000011.002"));
    PhilipsParseWarnings w;
    auto chunks = PhilipsParser::readChunks(bytes.data(), bytes.size(), w);
    ASSERT_EQ(chunks.size(), 1u);

    ParsedSession session;
    std::vector<PhilipsParser::EventStat> stats;
    ASSERT_TRUE(PhilipsParser::parseEvents(chunks, session, stats, w));

    // OA before the unknown code is kept; the block is abandoned after it
    EXPECT_EQ(session.events.size(), 1u);
    EXPECT_EQ(session.events[0].event_type, EventType::OBSTRUCTIVE);
    EXPECT_EQ(w.unknown_event_codes, 1);
}

TEST(PhilipsParser, SubV4SchemasSkipped) {
    // System One (FamilyVersion 3): .001/.002 gated off pending real-card
    // confirmation (format doc Q2/Q3)
    auto sum = readBytes(fixturePath("systemone/0000000002.001"));
    PhilipsParseWarnings w;
    auto chunks = PhilipsParser::readChunks(sum.data(), sum.size(), w);
    ASSERT_EQ(chunks.size(), 1u);

    ParsedSession session;
    EXPECT_FALSE(PhilipsParser::parseSummary(chunks, session, w));
    EXPECT_GE(w.unsupported_chunks, 1);
    EXPECT_FALSE(session.session_start.has_value());
}

// ── Phase 3: waveform ───────────────────────────────────────────────────────

TEST(PhilipsParser, WaveformFlowExtraction) {
    auto bytes = readBytes(fixturePath("ds1/0000000001.005"));
    PhilipsParseWarnings w;
    auto chunks = PhilipsParser::readChunks(bytes.data(), bytes.size(), w);
    ASSERT_EQ(chunks.size(), 2u);

    ParsedSession session;
    ASSERT_TRUE(PhilipsParser::parseWaveform(chunks, session, w));

    // 2 x 300 s chunks with a 10 s gap: ~10 minutes of summaries
    EXPECT_GE(session.breathing_summary.size(), 9u);
    EXPECT_LE(session.breathing_summary.size(), 10u);
    EXPECT_EQ(session.session_start.value(), tp(kMaskOn));

    // 15 bpm sine, amplitude 40: check a summary carries plausible values
    const auto& first = session.breathing_summary.front();
    EXPECT_NEAR(first.max_flow_rate, 40.0, 2.0);
    EXPECT_NEAR(first.min_flow_rate, -40.0, 2.0);
    ASSERT_TRUE(first.respiratory_rate.has_value());
    EXPECT_NEAR(first.respiratory_rate.value(), 15.0, 2.0);

    // Breath-by-breath detail present
    EXPECT_GT(session.breaths.size(), 100u);
}

TEST(PhilipsParser, BreathingSummaryFromFlowAt5Hz) {
    auto bytes = readBytes(fixturePath("systemone/0000000002.005"));
    PhilipsParseWarnings w;
    auto chunks = PhilipsParser::readChunks(bytes.data(), bytes.size(), w);
    ASSERT_EQ(chunks.size(), 1u);

    ParsedSession session;
    ASSERT_TRUE(PhilipsParser::parseWaveform(chunks, session, w));

    ASSERT_FALSE(session.breathing_summary.empty());
    const auto& s = session.breathing_summary.front();
    ASSERT_TRUE(s.tidal_volume.has_value());
    EXPECT_GT(s.tidal_volume.value(), 300.0);
    EXPECT_LT(s.tidal_volume.value(), 1200.0);
    // Flow-only path: no pressure percentile
    EXPECT_FALSE(s.pressure_p95.has_value());
}

// ── Assembly: parseSession / buffers / factory ─────────────────────────────

TEST(PhilipsParser, FullSessionParse) {
    auto tmp = stageDir("ds1", "full");

    PhilipsParser parser;
    auto session = parser.parseSession(tmp.string(), "dev1", "");

    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->manufacturer, DeviceManufacturer::PHILIPS);
    EXPECT_EQ(session->serial_number, "PXTEST0000DS1");
    EXPECT_EQ(session->model_id, 500);
    EXPECT_EQ(session->device_name, "DreamStation Auto CPAP");  // filled from model
    EXPECT_TRUE(session->has_summary);
    EXPECT_TRUE(session->has_events);
    EXPECT_EQ(session->parse_warnings, 0);
    EXPECT_EQ(session->status, ParsedSession::Status::COMPLETED);

    // Latest-with-summary group selected (0000000001, not the older .005-only)
    EXPECT_EQ(session->session_start.value(), tp(kMaskOn));
    EXPECT_EQ(session->duration_seconds.value(), 6 * 3600);
    EXPECT_EQ(session->events.size(), 9u);
    EXPECT_FALSE(session->breathing_summary.empty());
    ASSERT_TRUE(session->metrics.has_value());
    EXPECT_GT(session->metrics->total_events, 0);

    // Stats folding: minute 60 (stat at maskOn+4560... within summaries?) at
    // least one summary carries machine leak/snore
    bool any_stat = false;
    for (const auto& s : session->breathing_summary) {
        if (s.snore_index.has_value()) { any_stat = true; break; }
    }
    EXPECT_TRUE(any_stat);

    fs::remove_all(tmp);
}

TEST(PhilipsParser, PropertiesInParentDir) {
    // Card layout: Properties.txt in <serial>/, session files in <serial>/P0/
    auto tmp = stageDir("ds1", "parent");
    fs::create_directories(tmp / "P0");
    for (const auto& entry : fs::directory_iterator(tmp)) {
        if (entry.is_regular_file() && entry.path().filename() != "Properties.txt") {
            fs::rename(entry.path(), tmp / "P0" / entry.path().filename());
        }
    }

    PhilipsParser parser;
    auto session = parser.parseSession((tmp / "P0").string(), "dev1", "");

    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->serial_number, "PXTEST0000DS1");  // found in parent

    fs::remove_all(tmp);
}

TEST(PhilipsParser, BufferPathParity) {
    auto dir = fixturePath("ds1");
    std::map<std::string, std::vector<uint8_t>> storage;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            storage[entry.path().filename().string()] = readBytes(entry.path().string());
        }
    }
    std::map<std::string, std::pair<const uint8_t*, size_t>> buffers;
    for (const auto& [name, bytes] : storage) {
        buffers[name] = { bytes.data(), bytes.size() };
    }

    PhilipsParser parser;
    auto from_buffers = parser.parseSessionFromBuffers(buffers, "dev1", "");
    auto tmp = stageDir("ds1", "parity");
    auto from_disk = parser.parseSession(tmp.string(), "dev1", "");
    fs::remove_all(tmp);

    ASSERT_NE(from_buffers, nullptr);
    ASSERT_NE(from_disk, nullptr);
    EXPECT_EQ(from_buffers->session_start, from_disk->session_start);
    EXPECT_EQ(from_buffers->duration_seconds, from_disk->duration_seconds);
    EXPECT_EQ(from_buffers->events.size(), from_disk->events.size());
    EXPECT_EQ(from_buffers->breathing_summary.size(), from_disk->breathing_summary.size());
    EXPECT_EQ(from_buffers->serial_number, from_disk->serial_number);
}

TEST(PhilipsParser, CrcFailureSessionSurvives) {
    auto tmp = stageDir("badcrc", "crc");
    fs::copy_file(fixturePath("ds1/Properties.txt"), tmp / "Properties.txt");

    PhilipsParser parser;
    auto session = parser.parseSession(tmp.string(), "dev1", "");

    // .001 chunk was corrupt (skipped) but the .005 still yields a session
    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(session->has_summary);
    EXPECT_GT(session->parse_warnings, 0);
    EXPECT_FALSE(session->breathing_summary.empty());

    fs::remove_all(tmp);
}

TEST(PhilipsParser, SessionStartHintSelectsGroup) {
    auto tmp = stageDir("ds1", "hint");

    PhilipsParser parser;
    // Hint near the OLDER waveform-only session (base - 86400)
    auto session = parser.parseSession(tmp.string(), "dev1", "",
                                       tp(kBaseTs - 86400 + 30));

    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(session->has_summary);  // older group has no .001
    EXPECT_EQ(session->session_start.value(), tp(kBaseTs - 86400));

    fs::remove_all(tmp);
}

TEST(PhilipsParser, CreateParserFactory) {
    auto parser = createParser(DeviceManufacturer::PHILIPS);
    ASSERT_NE(parser, nullptr);
    EXPECT_EQ(parser->manufacturer(), DeviceManufacturer::PHILIPS);
}

TEST(PhilipsParser, FactoryAutoDetectsFromDir) {
    auto tmp = stageDir("ds1", "detect");

    auto parser = createParser(tmp.string());
    ASSERT_NE(parser, nullptr);
    EXPECT_EQ(parser->manufacturer(), DeviceManufacturer::PHILIPS);

    fs::remove_all(tmp);
}

#endif // CPAPDASH_WITH_PHILIPS
