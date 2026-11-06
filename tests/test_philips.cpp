#ifdef CPAPDASH_WITH_PHILIPS

#include <gtest/gtest.h>
#include "cpapdash/parser/PhilipsParser.h"
#include "cpapdash/parser/ISessionParser.h"
#include "cpapdash/parser/Models.h"

#include <cmath>
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

TEST(PhilipsParser, UnknownDataFormatVersionSkipped) {
    // Versions 2 and 3 are framed; anything else is counted, never guessed at.
    auto bytes = readBytes(fixturePath("unknownversion/0000000012.001"));
    ASSERT_FALSE(bytes.empty());

    PhilipsParseWarnings w;
    auto chunks = PhilipsParser::readChunks(bytes.data(), bytes.size(), w);

    EXPECT_TRUE(chunks.empty());
    EXPECT_EQ(w.unsupported_chunks, 1);
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

// SDD-009 D12: duration is the SUM of the mask-on periods, never the envelope,
// so a mask-off break is not therapy. v2 summary: 60 min, 30 min break, 20 min.
TEST(PhilipsParser, DurationIsSumOfMaskOnPeriodsV2) {
    PhilipsParser parser;
    auto session = parser.parseSession(fixturePath("ds1_twomask"), "dev1", "");

    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->session_start.value(), tp(kBaseTs + 60));
    EXPECT_EQ(session->session_end.value(), tp(kBaseTs + 6660));
    EXPECT_EQ(session->duration_seconds.value(), 3600 + 1200);
}

// ── DataFormatVersion 3: DreamStation 2 and DreamStation 1 on v3 firmware ─────
// docs/PHILIPS_FORMAT.md section 9, SDD-009. Fixtures from gen/make_fixtures.py.

TEST(PhilipsParser, V3ChunkFraming) {
    PhilipsParseWarnings w;
    auto summary = readBytes(fixturePath("ds2v3/P0/0000001E.B01"));
    auto chunks = PhilipsParser::readChunks(summary.data(), summary.size(), w);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(w.total(), 0);
    const auto& c = chunks[0];
    EXPECT_EQ(c.data_format_version, 3);
    EXPECT_EQ(c.family, 0);
    EXPECT_EQ(c.family_version, 6);
    EXPECT_EQ(c.file_extension, 1);
    EXPECT_EQ(c.session_number, 0x1Eu);
    EXPECT_EQ(c.record_size[0x08], 31);   // from the header block
    EXPECT_GT(c.record_size[0x01], 0);
    EXPECT_EQ(c.record_size[0x07], -1);   // absent from this chunk's table

    auto wave = readBytes(fixturePath("ds2v3/P0/0000001E.B05"));
    auto wchunks = PhilipsParser::readChunks(wave.data(), wave.size(), w);
    ASSERT_EQ(wchunks.size(), 2u);
    EXPECT_EQ(w.total(), 0);
    ASSERT_EQ(wchunks[0].waveforms.size(), 1u);   // 4-byte v3 signal entry
    EXPECT_EQ(wchunks[0].waveforms[0].interleave, 5);
    EXPECT_EQ(wchunks[0].waveforms[0].extra, 8);
    EXPECT_EQ(wchunks[0].durationSeconds(), 240);
    EXPECT_EQ(wchunks[0].data.size(), 1200u);     // 5 Hz * 240 s
}

TEST(PhilipsParser, V3CrcFailureSkipsChunk) {
    auto bytes = readBytes(fixturePath("ds2v3_badcrc/P0/00000032.B02"));
    PhilipsParseWarnings w;
    auto chunks = PhilipsParser::readChunks(bytes.data(), bytes.size(), w);
    EXPECT_TRUE(chunks.empty());
    EXPECT_EQ(w.crc_failures, 1);

    // The night still parses from its summary (SDD-009 D4).
    PhilipsParser parser;
    auto session = parser.parseSession(fixturePath("ds2v3_badcrc/P0"), "dev1", "");
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(session->has_summary);
    EXPECT_FALSE(session->has_events);
    EXPECT_GT(session->parse_warnings, 0);
}

TEST(PhilipsParser, ParsePropBinShortKeys) {
    auto info = PhilipsParser::parseProperties(fixturePath("ds2v3/PROP.BIN"));
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->serial_number, "PXTEST0DS2V3");
    EXPECT_EQ(info->model_number, "520X110C");
    EXPECT_EQ(info->family, 0);
    EXPECT_EQ(info->family_version, 6);
    EXPECT_EQ(info->data_format_version, "3");
    EXPECT_EQ(info->software_version, "V1.0.0.0000");
    EXPECT_EQ(info->patient_folder_num, 0);
    // Kept for a separate decryption step (SDD-009 D2).
    EXPECT_EQ(info->sid, "FAKE-SID");
    EXPECT_EQ(info->bk, "FAKE-BK");
    EXPECT_EQ(info->sk, "FAKE-SK");
    EXPECT_EQ(info->ek, "FAKE-EK");
    EXPECT_EQ(info->dk, "FAKE-DK");
}

TEST(PhilipsParser, DetectsPropBinAndPropTxt) {
    EXPECT_EQ(detectManufacturer(fixturePath("ds2v3")), DeviceManufacturer::PHILIPS);
    EXPECT_EQ(detectManufacturer(fixturePath("ds1v3")), DeviceManufacturer::PHILIPS);
    EXPECT_EQ(detectManufacturer(fixturePath("ds2_encrypted")), DeviceManufacturer::PHILIPS);
    EXPECT_EQ(detectManufacturer(std::vector<std::string>{"PROP.BIN", "P0/0000001E.B01"}),
              DeviceManufacturer::PHILIPS);
}

TEST(PhilipsParser, Ds2FullSession) {
    PhilipsParser parser;
    auto s = parser.parseSession(fixturePath("ds2v3/P0"), "dev1", "", tp(kBaseTs));
    ASSERT_NE(s, nullptr);

    EXPECT_EQ(s->manufacturer, DeviceManufacturer::PHILIPS);
    EXPECT_EQ(s->serial_number, "PXTEST0DS2V3");   // PROP.BIN in the parent folder
    EXPECT_EQ(s->product_name, "DreamStation 2 Auto CPAP");
    EXPECT_EQ(s->firmware, "V1.0.0.0000");
    EXPECT_TRUE(s->has_summary);
    EXPECT_TRUE(s->has_events);
    // Two records the doc does not describe, each skipped by its size and counted:
    // summary 0x09 and event 0x20.
    EXPECT_EQ(s->parse_warnings, 2);

    // Two mask-on periods: 60 min, a 10 min break, 30 min (D12).
    EXPECT_EQ(s->session_start.value(), tp(kBaseTs + 60));
    EXPECT_EQ(s->session_end.value(), tp(kBaseTs + 60 + 3600 + 600 + 1800));
    EXPECT_EQ(s->duration_seconds.value(), 3600 + 1800);

    // Settings (doc 9.7)
    ASSERT_TRUE(s->settings.has_value());
    const auto& st = *s->settings;
    EXPECT_EQ(st.therapy_mode, 1);                  // auto CPAP
    EXPECT_DOUBLE_EQ(st.min_pressure.value(), 5.0);
    EXPECT_DOUBLE_EQ(st.max_pressure.value(), 12.0);
    EXPECT_EQ(st.ramp_time, 20);
    EXPECT_DOUBLE_EQ(st.ramp_pressure.value(), 4.0);
    EXPECT_EQ(st.flex_mode, 2);                     // 0x90 on auto CPAP = A-Flex
    EXPECT_EQ(st.flex_level, 2);
    EXPECT_EQ(st.humidifier_mode, 3);               // heated tube
    EXPECT_EQ(st.humidifier_level, 3);
    EXPECT_EQ(st.tube_temp, 3);
    EXPECT_EQ(st.hose_diameter, 15);

    // Events (doc 9.8)
    int oa = 0, ca = 0, h = 0, rera = 0, fl = 0, other = 0, pb = 0, snore = 0;
    std::optional<SleepEvent> first_oa, first_h, ca_event, pb_event, other_event;
    for (const auto& e : s->events) {
        switch (e.event_type) {
            case EventType::OBSTRUCTIVE: if (!oa++) first_oa = e; break;
            case EventType::CLEAR_AIRWAY: ++ca; ca_event = e; break;
            case EventType::HYPOPNEA: if (!h++) first_h = e; break;
            case EventType::RERA: ++rera; break;
            case EventType::FLOW_LIMITATION: ++fl; break;
            case EventType::OTHER: ++other; other_event = e; break;
            case EventType::PERIODIC_BREATHING: ++pb; pb_event = e; break;
            case EventType::VIBRATORY_SNORE: ++snore; break;
            default: break;
        }
    }
    EXPECT_EQ(oa, 3);
    EXPECT_EQ(ca, 1);
    EXPECT_EQ(h, 2);
    EXPECT_EQ(rera, 1);
    EXPECT_EQ(fl, 1);
    EXPECT_EQ(other, 1);
    EXPECT_EQ(pb, 1);
    EXPECT_EQ(snore, 1);

    // Onsets: clock = header ts + summed deltas; OA ended 20 s before its record.
    EXPECT_EQ(first_oa->timestamp, tp(kBaseTs + 780 - 20));
    // 0x14: byte is its duration; placed back from the record.
    EXPECT_EQ(first_h->timestamp, tp(kBaseTs + 1380 - 12));
    EXPECT_DOUBLE_EQ(first_h->duration_seconds, 12.0);
    // 0x12 carries NO time field: the CA after it sits at 1470, not thousands of
    // seconds later.
    EXPECT_EQ(ca_event->timestamp, tp(kBaseTs + 1470 - 8));
    // 0x0f: 60 two-second units, ended 5 s before the record at 2150.
    EXPECT_EQ(pb_event->timestamp, tp(kBaseTs + 2150 - 5 - 120));
    EXPECT_DOUBLE_EQ(pb_event->duration_seconds, 120.0);
    // 0x0b: kept on the timeline as OTHER, out of the index (D10).
    EXPECT_EQ(other_event->timestamp, tp(kBaseTs + 1440 - 9));
    ASSERT_TRUE(other_event->details.has_value());
    EXPECT_NE(other_event->details->find("0x0b"), std::string::npos);

    // Our metrics come from the events; 0x0b is not a hypopnea for AHI.
    ASSERT_TRUE(s->metrics.has_value());
    EXPECT_EQ(s->metrics->hypopneas, 2);
    EXPECT_NEAR(s->metrics->ahi, (3 + 1 + 2) / 1.5, 1e-9);

    // The machine's own counts, summed over its two statistics records, beside ours.
    ASSERT_TRUE(s->machine_reported.has_value());
    const auto& m = *s->machine_reported;
    EXPECT_EQ(m.obstructive_apneas, 3);
    EXPECT_EQ(m.clear_airway_apneas, 1);
    EXPECT_EQ(m.hypopneas, 2);
    EXPECT_EQ(m.hypopneas_variant, 1);
    EXPECT_EQ(m.reras, 1);
    EXPECT_EQ(m.flow_limitations, 1);
    EXPECT_EQ(m.periodic_breathing_minutes, 2);

    EXPECT_FALSE(s->breathing_summary.empty());     // .B05 flow went through
}

TEST(PhilipsParser, KeyFieldsNeverReachTheSession) {
    PhilipsParser parser;
    auto s = parser.parseSession(fixturePath("ds2v3/P0"), "dev1", "", tp(kBaseTs));
    ASSERT_NE(s, nullptr);
    for (const std::string* f : {&s->serial_number, &s->device_name, &s->product_name,
                                 &s->firmware, &s->device_id}) {
        EXPECT_EQ(f->find("FAKE-"), std::string::npos) << *f;
    }
    for (const auto& e : s->events) {
        if (e.details) EXPECT_EQ(e.details->find("FAKE-"), std::string::npos);
    }
}

TEST(PhilipsParser, SessionSplitAcrossPatientFolders) {
    // Session 0x1F: summary in P1, events in P0 (doc 9.3).
    PhilipsParser parser;
    auto s = parser.parseSession(fixturePath("ds2v3/P1"), "dev1", "", tp(kBaseTs + 86400));
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->has_summary);
    EXPECT_TRUE(s->has_events);
    ASSERT_TRUE(s->machine_reported.has_value());
    ASSERT_TRUE(s->metrics.has_value());
    EXPECT_EQ(s->metrics->obstructive_apneas, s->machine_reported->obstructive_apneas);
}

TEST(PhilipsParser, Ds1OnV3Firmware) {
    PhilipsParser parser;
    auto s = parser.parseSession(fixturePath("ds1v3/P0"), "dev1", "");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->serial_number, "PXTEST0DS1V3");
    EXPECT_EQ(s->product_name, "DreamStation Auto CPAP");
    EXPECT_TRUE(s->has_summary);
    EXPECT_TRUE(s->has_events);
    EXPECT_FALSE(s->breathing_summary.empty());
    ASSERT_TRUE(s->machine_reported.has_value());
    EXPECT_EQ(s->metrics->obstructive_apneas, s->machine_reported->obstructive_apneas);
}

TEST(PhilipsParser, MultiSessionFileGroupsByHeader) {
    const auto dir = fixturePath("ds2v3_multisession/P0");
    auto groups = PhilipsParser::discoverSessions(dir);
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_EQ(groups[0].session_number, 0x3Cu);
    EXPECT_EQ(groups[1].session_number, 0x3Du);
    EXPECT_EQ(groups[0].summary_path, groups[1].summary_path);   // one file, two sessions

    // 0x3C's chunk appears twice, byte-identical: counted once.
    PhilipsParser parser;
    auto s = parser.parseSession(dir, "dev1", "", tp(kBaseTs));
    ASSERT_NE(s, nullptr);
    ASSERT_TRUE(s->machine_reported.has_value());
    EXPECT_EQ(s->machine_reported->obstructive_apneas, 3);
    EXPECT_EQ(s->duration_seconds.value(), 3600);
}

TEST(PhilipsParser, EncryptedCardIsReported) {
    // Noise PROP.BIN in the card root (SDD-009 D1).
    EXPECT_TRUE(PhilipsParser::isEncryptedCard(fixturePath("ds2_encrypted/P0")));
    PhilipsParser parser;
    EXPECT_EQ(parser.parseSession(fixturePath("ds2_encrypted/P0"), "dev1", ""), nullptr);

    // Headers in the clear, every data CRC failing.
    EXPECT_TRUE(PhilipsParser::isEncryptedCard(fixturePath("ds2_encrypted_data/P0")));

    // Decrypted and v2 cards are not.
    EXPECT_FALSE(PhilipsParser::isEncryptedCard(fixturePath("ds2v3/P0")));
    EXPECT_FALSE(PhilipsParser::isEncryptedCard(fixturePath("ds1v3/P0")));
    EXPECT_FALSE(PhilipsParser::isEncryptedCard(fixturePath("ds1")));
}

TEST(PhilipsParser, V3BufferPathParity) {
    std::map<std::string, std::vector<uint8_t>> storage;
    for (const auto& entry : fs::directory_iterator(fixturePath("ds2v3/P0")))
        storage[entry.path().filename().string()] = readBytes(entry.path().string());
    storage["PROP.BIN"] = readBytes(fixturePath("ds2v3/PROP.BIN"));
    std::map<std::string, std::pair<const uint8_t*, size_t>> buffers;
    for (const auto& [name, bytes] : storage) buffers[name] = {bytes.data(), bytes.size()};

    PhilipsParser parser;
    auto from_buffers = parser.parseSessionFromBuffers(buffers, "dev1", "");
    auto from_disk = parser.parseSession(fixturePath("ds2v3/P0"), "dev1", "");
    ASSERT_NE(from_buffers, nullptr);
    ASSERT_NE(from_disk, nullptr);
    EXPECT_EQ(from_buffers->session_start, from_disk->session_start);
    EXPECT_EQ(from_buffers->duration_seconds, from_disk->duration_seconds);
    EXPECT_EQ(from_buffers->events.size(), from_disk->events.size());
    EXPECT_EQ(from_buffers->serial_number, from_disk->serial_number);
}

// ── An open session: the waveform only, no summary yet (doc 9.9) ───────────

namespace {

// A card whose session 0x1E is still being recorded: its .B05 is there, its
// .B01 and .B02 are not (the machine writes them at mask-off). `tail` is
// appended to the waveform, to model a file caught mid-write.
fs::path stageOpenSession(const std::string& tag, const std::vector<uint8_t>& tail = {}) {
    auto root = fs::temp_directory_path() / ("philips_open_" + tag);
    fs::remove_all(root);
    fs::create_directories(root / "P0");
    fs::copy_file(fixturePath("ds2v3/PROP.BIN"), root / "PROP.BIN");
    auto wave = readBytes(fixturePath("ds2v3/P0/0000001E.B05"));
    wave.insert(wave.end(), tail.begin(), tail.end());
    std::ofstream(root / "P0" / "0000001E.B05", std::ios::binary)
        .write(reinterpret_cast<const char*>(wave.data()),
               static_cast<std::streamsize>(wave.size()));
    return root;
}

}  // namespace

TEST(PhilipsParser, OpenSessionFlowFromWaveformOnly) {
    const auto root = stageOpenSession("flow");
    PhilipsParseWarnings w;
    auto flow = PhilipsParser::sessionFlow((root / "P0").string(), 0x1E, w);
    ASSERT_TRUE(flow.has_value());
    EXPECT_EQ(flow->start, tp(kBaseTs + 60));      // the first chunk's header time
    EXPECT_DOUBLE_EQ(flow->sample_rate, 5.0);
    EXPECT_EQ(flow->recorded_seconds, 480);        // two 240 s chunks
    EXPECT_EQ(flow->flow.size(), 2400u);           // they abut: no gap
    EXPECT_EQ(flow->end(), tp(kBaseTs + 60 + 480));
    EXPECT_EQ(w.total(), 0);
    fs::remove_all(root);
}

TEST(PhilipsParser, OpenSessionGapIsNaNAndNotRecordedTime) {
    // The second chunk of a second file, 10 minutes after the first ends: the
    // mask was off in between, so that time is NaN and not therapy.
    const auto root = stageOpenSession("gap");
    auto wave = readBytes(fixturePath("ds2v3/P0/0000001E.B05"));
    PhilipsParseWarnings w;
    auto chunks = PhilipsParser::readChunks(wave.data(), wave.size(), w);
    ASSERT_EQ(chunks.size(), 2u);
    chunks[1].timestamp = tp(kBaseTs + 60 + 240 + 600);
    auto flow = PhilipsParser::flowSeries(chunks, w);
    ASSERT_TRUE(flow.has_value());
    EXPECT_EQ(flow->recorded_seconds, 480);
    EXPECT_EQ(flow->flow.size(), static_cast<size_t>((240 + 600 + 240) * 5));
    EXPECT_TRUE(std::isnan(flow->flow[240 * 5 + 10]));
    EXPECT_FALSE(std::isnan(flow->flow[10]));
    fs::remove_all(root);
}

TEST(PhilipsParser, OpenSessionTruncatedTailIsLeftOut) {
    // The tapper copies the file while the machine appends to it.
    auto wave = readBytes(fixturePath("ds2v3/P0/0000001E.B05"));
    const std::vector<uint8_t> half(wave.begin(), wave.begin() + wave.size() / 4);
    const auto root = stageOpenSession("tail", half);
    PhilipsParseWarnings w;
    auto flow = PhilipsParser::sessionFlow((root / "P0").string(), 0x1E, w);
    ASSERT_TRUE(flow.has_value());
    EXPECT_EQ(flow->recorded_seconds, 480);
    EXPECT_EQ(w.truncated_chunks, 1);
    fs::remove_all(root);
}

TEST(PhilipsParser, OpenSessionUnknownNumberHasNoFlow) {
    const auto root = stageOpenSession("none");
    PhilipsParseWarnings w;
    EXPECT_FALSE(PhilipsParser::sessionFlow((root / "P0").string(), 0x99, w).has_value());
    EXPECT_FALSE(PhilipsParser::sessionFlow((root / "nope").string(), 0x1E, w).has_value());
    fs::remove_all(root);
}

TEST(PhilipsParser, OpenSessionParsesWithWaveformTimeAsTherapy) {
    const auto root = stageOpenSession("parse");
    PhilipsParser parser;
    auto s = parser.parseSession((root / "P0").string(), "dev1", "", tp(kBaseTs + 60));
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->has_summary);
    EXPECT_FALSE(s->has_events);
    EXPECT_EQ(s->session_start, tp(kBaseTs + 60));
    EXPECT_EQ(s->session_end, tp(kBaseTs + 60 + 480));
    ASSERT_TRUE(s->duration_seconds.has_value());
    EXPECT_EQ(*s->duration_seconds, 480);          // therapy so far = waveform time
    EXPECT_FALSE(s->breathing_summary.empty());
    EXPECT_TRUE(s->events.empty());                // no machine events before mask-off
    fs::remove_all(root);
}

TEST(PhilipsParser, ClosedSessionKeepsSummaryDuration) {
    // With a summary, the duration stays the summed mask-on periods (D12).
    PhilipsParser parser;
    auto s = parser.parseSession(fixturePath("ds2v3/P0"), "dev1", "", tp(kBaseTs));
    ASSERT_NE(s, nullptr);
    ASSERT_TRUE(s->has_summary);
    ASSERT_TRUE(s->duration_seconds.has_value());
    PhilipsParseWarnings w;
    auto flow = PhilipsParser::sessionFlow(fixturePath("ds2v3/P0"), 0x1E, w);
    ASSERT_TRUE(flow.has_value());
    EXPECT_NE(*s->duration_seconds, flow->recorded_seconds);
}

#endif // CPAPDASH_WITH_PHILIPS
