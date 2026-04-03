#include <gtest/gtest.h>
#include "sleeplink/parser/Models.h"
#include <cmath>

using namespace sleeplink::parser;

// ============================================================================
//  EventType string conversion tests
// ============================================================================

TEST(ModelsTest, EventTypeToStringAllTypes) {
    EXPECT_EQ(eventTypeToString(EventType::APNEA), "Apnea");
    EXPECT_EQ(eventTypeToString(EventType::HYPOPNEA), "Hypopnea");
    EXPECT_EQ(eventTypeToString(EventType::RERA), "RERA");
    EXPECT_EQ(eventTypeToString(EventType::CSR), "CSR");
    EXPECT_EQ(eventTypeToString(EventType::OBSTRUCTIVE), "Obstructive");
    EXPECT_EQ(eventTypeToString(EventType::CENTRAL), "Central");
    EXPECT_EQ(eventTypeToString(EventType::CLEAR_AIRWAY), "Clear Airway");
}

// ============================================================================
//  SleepEvent construction tests
// ============================================================================

TEST(ModelsTest, SleepEventConstruction) {
    auto now = std::chrono::system_clock::now();
    SleepEvent event(EventType::OBSTRUCTIVE, now, 12.5);

    EXPECT_EQ(event.event_type, EventType::OBSTRUCTIVE);
    EXPECT_EQ(event.timestamp, now);
    EXPECT_DOUBLE_EQ(event.duration_seconds, 12.5);
    EXPECT_FALSE(event.details.has_value());
}

TEST(ModelsTest, SleepEventWithDetails) {
    auto now = std::chrono::system_clock::now();
    SleepEvent event(EventType::HYPOPNEA, now, 8.0);
    event.details = "Obstructive Hypopnea";

    EXPECT_TRUE(event.details.has_value());
    EXPECT_EQ(event.details.value(), "Obstructive Hypopnea");
}

// ============================================================================
//  VitalSample tests
// ============================================================================

TEST(ModelsTest, VitalSampleDefaultsEmpty) {
    VitalSample v;
    EXPECT_FALSE(v.spo2.has_value());
    EXPECT_FALSE(v.heart_rate.has_value());
}

TEST(ModelsTest, VitalSampleWithValues) {
    auto now = std::chrono::system_clock::now();
    VitalSample v(now);
    v.spo2 = 96.5;
    v.heart_rate = 72;

    EXPECT_EQ(v.timestamp, now);
    EXPECT_DOUBLE_EQ(v.spo2.value(), 96.5);
    EXPECT_EQ(v.heart_rate.value(), 72);
}

// ============================================================================
//  BreathingSummary tests
// ============================================================================

TEST(ModelsTest, BreathingSummaryDefaultsZero) {
    BreathingSummary bs;
    EXPECT_DOUBLE_EQ(bs.avg_flow_rate, 0.0);
    EXPECT_DOUBLE_EQ(bs.avg_pressure, 0.0);
    EXPECT_FALSE(bs.respiratory_rate.has_value());
    EXPECT_FALSE(bs.tidal_volume.has_value());
    EXPECT_FALSE(bs.mask_pressure.has_value());
}

// ============================================================================
//  ParsedSession::calculateMetrics tests
// ============================================================================

TEST(ModelsTest, CalculateMetricsWithEvents) {
    ParsedSession session;
    session.duration_seconds = 7200;  // 2 hours

    auto now = std::chrono::system_clock::now();

    // Add 6 events: 2 obstructive, 2 hypopnea, 1 central, 1 RERA
    session.events.push_back(SleepEvent(EventType::OBSTRUCTIVE, now, 15.0));
    session.events.push_back(SleepEvent(EventType::OBSTRUCTIVE, now, 20.0));
    session.events.push_back(SleepEvent(EventType::HYPOPNEA, now, 10.0));
    session.events.push_back(SleepEvent(EventType::HYPOPNEA, now, 12.0));
    session.events.push_back(SleepEvent(EventType::CENTRAL, now, 8.0));
    session.events.push_back(SleepEvent(EventType::RERA, now, 5.0));

    session.calculateMetrics();
    ASSERT_TRUE(session.metrics.has_value());

    auto& m = session.metrics.value();

    // Event counts
    EXPECT_EQ(m.total_events, 6);
    EXPECT_EQ(m.obstructive_apneas, 2);
    EXPECT_EQ(m.hypopneas, 2);
    EXPECT_EQ(m.central_apneas, 1);
    EXPECT_EQ(m.reras, 1);
    EXPECT_EQ(m.clear_airway_apneas, 0);

    // AHI = 6 events / 2 hours = 3.0
    EXPECT_NEAR(m.ahi, 3.0, 0.01);

    // Event duration stats
    EXPECT_TRUE(m.avg_event_duration.has_value());
    EXPECT_NEAR(m.avg_event_duration.value(), (15 + 20 + 10 + 12 + 8 + 5) / 6.0, 0.01);
    EXPECT_NEAR(m.max_event_duration.value(), 20.0, 0.01);

    // Usage
    EXPECT_TRUE(m.usage_hours.has_value());
    EXPECT_NEAR(m.usage_hours.value(), 2.0, 0.01);
    EXPECT_NEAR(m.usage_percent.value(), 25.0, 0.01);  // 2/8 * 100
}

TEST(ModelsTest, CalculateMetricsWithBreathingSummaries) {
    ParsedSession session;
    session.duration_seconds = 300;  // 5 minutes

    auto now = std::chrono::system_clock::now();

    // Create 3 breathing summaries with known values
    for (int i = 0; i < 3; ++i) {
        BreathingSummary bs(now + std::chrono::minutes(i));
        bs.avg_pressure = 10.0 + i;      // 10, 11, 12
        bs.avg_flow_rate = 5.0 + i;      // 5, 6, 7
        bs.respiratory_rate = 14.0 + i;  // 14, 15, 16
        bs.tidal_volume = 400.0 + i * 50; // 400, 450, 500
        bs.leak_rate = 2.0 + i;          // 2, 3, 4
        session.breathing_summary.push_back(bs);
    }

    session.calculateMetrics();
    ASSERT_TRUE(session.metrics.has_value());

    auto& m = session.metrics.value();

    // Pressure: avg of 10, 11, 12 = 11.0
    EXPECT_TRUE(m.avg_pressure.has_value());
    EXPECT_NEAR(m.avg_pressure.value(), 11.0, 0.01);
    EXPECT_NEAR(m.min_pressure.value(), 10.0, 0.01);
    EXPECT_NEAR(m.max_pressure.value(), 12.0, 0.01);

    // Flow: avg of 5, 6, 7 = 6.0
    EXPECT_TRUE(m.avg_flow_rate.has_value());
    EXPECT_NEAR(m.avg_flow_rate.value(), 6.0, 0.01);

    // Respiratory rate: avg of 14, 15, 16 = 15.0
    EXPECT_TRUE(m.avg_respiratory_rate.has_value());
    EXPECT_NEAR(m.avg_respiratory_rate.value(), 15.0, 0.01);

    // Tidal volume: avg of 400, 450, 500 = 450.0
    EXPECT_TRUE(m.avg_tidal_volume.has_value());
    EXPECT_NEAR(m.avg_tidal_volume.value(), 450.0, 0.01);

    // Leak: avg of 2, 3, 4 = 3.0
    EXPECT_TRUE(m.avg_leak_rate.has_value());
    EXPECT_NEAR(m.avg_leak_rate.value(), 3.0, 0.01);
}

TEST(ModelsTest, CalculateMetricsWithVitals) {
    ParsedSession session;
    session.duration_seconds = 60;

    auto now = std::chrono::system_clock::now();

    // Add vitals: SpO2 and HR
    for (int i = 0; i < 5; ++i) {
        VitalSample v(now + std::chrono::seconds(i));
        v.spo2 = 94.0 + i;      // 94, 95, 96, 97, 98
        v.heart_rate = 70 + i;   // 70, 71, 72, 73, 74
        session.vitals.push_back(v);
    }

    session.calculateMetrics();
    ASSERT_TRUE(session.metrics.has_value());

    auto& m = session.metrics.value();

    // SpO2: avg=96, min=94, max=98
    EXPECT_TRUE(m.avg_spo2.has_value());
    EXPECT_NEAR(m.avg_spo2.value(), 96.0, 0.01);
    EXPECT_NEAR(m.min_spo2.value(), 94.0, 0.01);
    EXPECT_NEAR(m.max_spo2.value(), 98.0, 0.01);

    // HR: avg=72, min=70, max=74
    EXPECT_TRUE(m.avg_heart_rate.has_value());
    EXPECT_EQ(m.avg_heart_rate.value(), 72);
    EXPECT_EQ(m.min_heart_rate.value(), 70);
    EXPECT_EQ(m.max_heart_rate.value(), 74);
}

TEST(ModelsTest, CalculateMetricsWithDesaturationDrop) {
    ParsedSession session;
    session.duration_seconds = 10;

    auto now = std::chrono::system_clock::now();

    // Simulate a desaturation: 97, 97, 92 (drop of 5%)
    session.vitals.push_back(VitalSample(now));
    session.vitals.back().spo2 = 97.0;
    session.vitals.push_back(VitalSample(now + std::chrono::seconds(1)));
    session.vitals.back().spo2 = 97.0;
    session.vitals.push_back(VitalSample(now + std::chrono::seconds(2)));
    session.vitals.back().spo2 = 92.0;  // 5% drop from previous
    session.vitals.push_back(VitalSample(now + std::chrono::seconds(3)));
    session.vitals.back().spo2 = 96.0;

    session.calculateMetrics();
    ASSERT_TRUE(session.metrics.has_value());

    // Should detect 1 desaturation (97 -> 92 = 5% drop >= 4% threshold)
    EXPECT_TRUE(session.metrics->spo2_drops.has_value());
    EXPECT_EQ(session.metrics->spo2_drops.value(), 1);
}

TEST(ModelsTest, CalculateMetricsEmptySession) {
    ParsedSession session;
    session.calculateMetrics();

    ASSERT_TRUE(session.metrics.has_value());

    auto& m = session.metrics.value();
    EXPECT_EQ(m.total_events, 0);
    EXPECT_DOUBLE_EQ(m.ahi, 0.0);
    EXPECT_FALSE(m.avg_pressure.has_value());
    EXPECT_FALSE(m.avg_spo2.has_value());
    EXPECT_FALSE(m.avg_heart_rate.has_value());
    EXPECT_FALSE(m.usage_hours.has_value());
}

TEST(ModelsTest, ParsedSessionToString) {
    ParsedSession session;
    session.device_name = "AirSense 11";
    session.serial_number = "23243570851";
    session.duration_seconds = 25200;  // 7 hours
    session.session_start = std::chrono::system_clock::now();

    session.calculateMetrics();

    std::string str = session.toString();
    EXPECT_NE(str.find("AirSense 11"), std::string::npos);
    EXPECT_NE(str.find("23243570851"), std::string::npos);
    EXPECT_NE(str.find("7h"), std::string::npos);
}

TEST(ModelsTest, STRDailyRecordHasTherapy) {
    STRDailyRecord r;
    EXPECT_FALSE(r.hasTherapy());

    r.duration_minutes = 420.0;
    EXPECT_TRUE(r.hasTherapy());
}
