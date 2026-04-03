#include <gtest/gtest.h>

#include "sleeplink/parser/Models.h"
#include "sleeplink/parser/ISessionParser.h"

#ifdef SLEEPLINK_WITH_PHILIPS
#include "sleeplink/parser/PhilipsCrypto.h"
#include "sleeplink/parser/PhilipsChunk.h"
#include "sleeplink/parser/PhilipsParser.h"
#endif

using namespace sleeplink::parser;

// ── Model Extensions ─────────────────────────────────────────────────────────

TEST(ModelsExtensions, NewEventTypesHaveStrings) {
    EXPECT_EQ(eventTypeToString(EventType::FLOW_LIMITATION), "Flow Limitation");
    EXPECT_EQ(eventTypeToString(EventType::PERIODIC_BREATHING), "Periodic Breathing");
    EXPECT_EQ(eventTypeToString(EventType::LARGE_LEAK), "Large Leak");
    EXPECT_EQ(eventTypeToString(EventType::VIBRATORY_SNORE), "Vibratory Snore");
}

TEST(ModelsExtensions, SleepEventUsesEventTypeField) {
    auto now = std::chrono::system_clock::now();
    SleepEvent event(EventType::OBSTRUCTIVE, now, 10.5);
    EXPECT_EQ(event.event_type, EventType::OBSTRUCTIVE);
    EXPECT_DOUBLE_EQ(event.duration_seconds, 10.5);
}

TEST(ModelsExtensions, DeviceSettingsDefaults) {
    DeviceSettings settings;
    EXPECT_FALSE(settings.therapy_mode.has_value());
    EXPECT_FALSE(settings.set_pressure.has_value());
    EXPECT_FALSE(settings.flex_mode.has_value());
    EXPECT_FALSE(settings.humidifier_mode.has_value());
}

TEST(ModelsExtensions, ParsedSessionHasManufacturer) {
    ParsedSession session;
    EXPECT_EQ(session.manufacturer, DeviceManufacturer::UNKNOWN);

    session.manufacturer = DeviceManufacturer::PHILIPS;
    EXPECT_EQ(session.manufacturer, DeviceManufacturer::PHILIPS);
}

TEST(ModelsExtensions, ParsedSessionHasFilePaths) {
    ParsedSession session;
    EXPECT_FALSE(session.brp_file_path.has_value());
    session.brp_file_path = "/tmp/test.edf";
    EXPECT_EQ(session.brp_file_path.value(), "/tmp/test.edf");
}

TEST(ModelsExtensions, ParsedSessionHasSettings) {
    ParsedSession session;
    EXPECT_FALSE(session.settings.has_value());

    DeviceSettings settings;
    settings.therapy_mode = 1;
    settings.set_pressure = 10.0;
    session.settings = settings;

    EXPECT_TRUE(session.settings.has_value());
    EXPECT_EQ(session.settings->therapy_mode.value(), 1);
}

// ── ISessionParser Interface ─────────────────────────────────────────────────

TEST(ISessionParser, CreateResMedParser) {
    auto parser = createParser(DeviceManufacturer::RESMED);
    ASSERT_NE(parser, nullptr);
    EXPECT_EQ(parser->manufacturer(), DeviceManufacturer::RESMED);
}

TEST(ISessionParser, CreateUnknownReturnsNull) {
    auto parser = createParser(DeviceManufacturer::UNKNOWN);
    EXPECT_EQ(parser, nullptr);
}

#ifdef SLEEPLINK_WITH_PHILIPS

TEST(ISessionParser, CreatePhilipsParser) {
    auto parser = createParser(DeviceManufacturer::PHILIPS);
    ASSERT_NE(parser, nullptr);
    EXPECT_EQ(parser->manufacturer(), DeviceManufacturer::PHILIPS);
}

// ── PhilipsCrypto ────────────────────────────────────────────────────────────

TEST(PhilipsCrypto, ParseHeaderRejectsTooShort) {
    uint8_t data[100] = {};
    DS2Header header;
    EXPECT_FALSE(PhilipsCrypto::parseHeader(data, sizeof(data), header));
}

TEST(PhilipsCrypto, ParseHeaderRejectsBadMagic) {
    // 202 bytes with wrong magic
    std::vector<uint8_t> data(202, 0);
    data[0] = 0xFF;  // wrong magic
    data[1] = 0xFF;
    DS2Header header;
    EXPECT_FALSE(PhilipsCrypto::parseHeader(data.data(), data.size(), header));
}

TEST(PhilipsCrypto, ParseHeaderAcceptsValidStructure) {
    // Build a valid DS2 header (198 bytes structured + padding)
    std::vector<uint8_t> data(256, 0);  // extra room for padding
    size_t pos = 0;

    // Magic
    data[pos++] = 0x0D; data[pos++] = 0x01;
    // Version
    data[pos++] = 0x01; data[pos++] = 0x01;
    // GUID: length=36
    data[pos++] = 0x24; data[pos++] = 0x00;
    pos += 36;
    // IV: length=12
    data[pos++] = 0x0C; data[pos++] = 0x00;
    pos += 12;
    // Salt: length=16
    data[pos++] = 0x10; data[pos++] = 0x00;
    pos += 16;
    // Reserved
    data[pos++] = 0x00; data[pos++] = 0x01;
    // Import key: length=32
    data[pos++] = 0x20; data[pos++] = 0x00;
    pos += 32;
    // Import tag: length=16
    data[pos++] = 0x10; data[pos++] = 0x00;
    pos += 16;
    // Export key: length=32
    data[pos++] = 0x20; data[pos++] = 0x00;
    pos += 32;
    // Export tag: length=16
    data[pos++] = 0x10; data[pos++] = 0x00;
    pos += 16;
    // Payload tag: length=16
    data[pos++] = 0x10; data[pos++] = 0x00;
    pos += 16;

    ASSERT_EQ(pos, 198u);  // 198 bytes of structured header

    DS2Header header;
    EXPECT_TRUE(PhilipsCrypto::parseHeader(data.data(), data.size(), header));
    EXPECT_EQ(header.guid.size(), 36u);
    EXPECT_EQ(header.iv.size(), 12u);
    EXPECT_EQ(header.salt.size(), 16u);
    EXPECT_EQ(header.export_key.size(), 32u);
    EXPECT_EQ(header.payload_tag.size(), 16u);
}

// ── PhilipsChunk ─────────────────────────────────────────────────────────────

TEST(PhilipsChunk, Checksum) {
    uint8_t data[] = {0x03, 0x10, 0x00, 0x00, 0x00, 0x06, 0x02};
    // Sum = 0x03 + 0x10 + 0x00 + 0x00 + 0x00 + 0x06 + 0x02 = 0x1B
    EXPECT_EQ(PRS1ChunkReader::calcChecksum(data, sizeof(data)), 0x1B);
}

TEST(PhilipsChunk, CRC16KnownValue) {
    // "123456789" should produce CRC-16/Kermit = 0x2189
    const uint8_t data[] = "123456789";
    uint16_t crc = PRS1ChunkReader::calcCRC16(data, 9);
    EXPECT_EQ(crc, 0x2189);
}

TEST(PhilipsChunk, ParseChunksRejectsShortData) {
    uint8_t data[] = {0x03, 0x00};
    auto chunks = PRS1ChunkReader::parseChunks(data, sizeof(data));
    EXPECT_TRUE(chunks.empty());
}

// ── PhilipsParser Events ─────────────────────────────────────────────────────

TEST(PhilipsParser, ParseEventsExtractsOA) {
    // Build a minimal F0V6 event chunk with one Obstructive Apnea (code 0x06)
    PRS1ChunkHeader hdr;
    hdr.family = 0;
    hdr.family_version = 6;
    hdr.ext = 2;
    hdr.timestamp = 1700000000;  // Some unix timestamp
    hdr.hblock[0x06] = 3;  // code 0x06: 2-byte ts delta + 1-byte elapsed = 3

    // Event data: code=0x06, ts_delta=60 (LE16), elapsed=10
    std::vector<uint8_t> data = {
        0x06,             // event code
        0x3C, 0x00,       // ts_delta = 60
        0x0A              // elapsed = 10
    };

    std::vector<std::pair<PRS1ChunkHeader, std::vector<uint8_t>>> chunks;
    chunks.emplace_back(hdr, data);

    ParsedSession session;
    bool ok = PhilipsParser::parseEvents(chunks, session);
    EXPECT_TRUE(ok);
    ASSERT_EQ(session.events.size(), 1u);
    EXPECT_EQ(session.events[0].event_type, EventType::OBSTRUCTIVE);
}

TEST(PhilipsParser, ParseEventsExtractsMultipleTypes) {
    PRS1ChunkHeader hdr;
    hdr.family = 0;
    hdr.family_version = 6;
    hdr.ext = 2;
    hdr.timestamp = 1700000000;

    // Set up hblock sizes for all event types we'll use
    hdr.hblock[0x06] = 3;  // OA: 2 ts + 1 elapsed
    hdr.hblock[0x07] = 3;  // CA: 2 ts + 1 elapsed
    hdr.hblock[0x0a] = 3;  // HY: 2 ts + 1 elapsed
    hdr.hblock[0x0c] = 3;  // FL: 2 ts + 1 elapsed
    hdr.hblock[0x0d] = 2;  // VS: 2 ts only

    std::vector<uint8_t> data = {
        0x06, 0x3C, 0x00, 0x0A,    // OA at t+60, elapsed 10
        0x07, 0x78, 0x00, 0x05,    // CA at t+120+60=180, elapsed 5
        0x0a, 0x3C, 0x00, 0x08,    // HY at t+240, elapsed 8
        0x0c, 0x1E, 0x00, 0x03,    // FL at t+270, elapsed 3
        0x0d, 0x0A, 0x00,          // VS at t+280 (no data bytes)
    };

    std::vector<std::pair<PRS1ChunkHeader, std::vector<uint8_t>>> chunks;
    chunks.emplace_back(hdr, data);

    ParsedSession session;
    PhilipsParser::parseEvents(chunks, session);

    ASSERT_EQ(session.events.size(), 5u);
    EXPECT_EQ(session.events[0].event_type, EventType::OBSTRUCTIVE);
    EXPECT_EQ(session.events[1].event_type, EventType::CLEAR_AIRWAY);
    EXPECT_EQ(session.events[2].event_type, EventType::HYPOPNEA);
    EXPECT_EQ(session.events[3].event_type, EventType::FLOW_LIMITATION);
    EXPECT_EQ(session.events[4].event_type, EventType::VIBRATORY_SNORE);
}

TEST(PhilipsParser, MetricsCalculationAfterEvents) {
    ParsedSession session;
    session.manufacturer = DeviceManufacturer::PHILIPS;
    session.duration_seconds = 3600;  // 1 hour

    auto base = std::chrono::system_clock::from_time_t(1700000000);

    // Add 5 events for AHI = 5.0
    for (int i = 0; i < 3; ++i)
        session.events.emplace_back(EventType::OBSTRUCTIVE, base, 0);
    for (int i = 0; i < 2; ++i)
        session.events.emplace_back(EventType::HYPOPNEA, base, 0);

    session.calculateMetrics();

    ASSERT_TRUE(session.metrics.has_value());
    EXPECT_DOUBLE_EQ(session.metrics->ahi, 5.0);
    EXPECT_EQ(session.metrics->obstructive_apneas, 3);
    EXPECT_EQ(session.metrics->hypopneas, 2);
    EXPECT_EQ(session.metrics->total_events, 5);
}

#else // !SLEEPLINK_WITH_PHILIPS

TEST(ISessionParser, CreatePhilipsReturnsNullWithoutFlag) {
    auto parser = createParser(DeviceManufacturer::PHILIPS);
    EXPECT_EQ(parser, nullptr);
}

#endif // SLEEPLINK_WITH_PHILIPS
