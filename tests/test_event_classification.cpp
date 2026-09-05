// SDD-004: what each EVE annotation classifies as, and which of those counts.
//
// The chain in EDFParser_EVE.cpp used to end in a catch-all that swallowed every
// unrecognised annotation into RERA, so session_metrics.reras -- and any RIN derived
// from it -- counted things that are not respiratory effort at all. It now lands in
// EventType::OTHER, which is recorded and stays inside total_events but belongs to
// no index.
//
// The 181 'Recording starts' file markers named in SDD-004 1.1 do not actually reach
// that catch-all in this repo: EDFFile::readAnnotations() drops that exact string one
// layer earlier. Both layers are pinned below, because the catch-all is still wrong
// for every label the filter does not know.
//
// The label census these tests assert against was measured from a real ResMed card
// (~/cool_shit/cpap_card_backup_20260827, 181 EVE files); reproduce it with
// tools/eve_labels.py.
#include <gtest/gtest.h>

#include "cpapdash/parser/EDFParser.h"
#include "cpapdash/parser/Models.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace cpapdash::parser;
namespace fs = std::filesystem;

namespace {

void writeField(std::vector<uint8_t>& buf, int off, int len, const std::string& v) {
    for (int i = 0; i < len; ++i)
        buf[off + i] = (i < static_cast<int>(v.size())) ? v[i] : ' ';
}

// A minimal BRP so parseSession has the checkpoint it requires.
std::vector<uint8_t> buildBRP(const std::string& ddmmyy, const std::string& hhmmss) {
    const int ns = 1, spr = 1500, records = 1;
    const int header_bytes = 256 + 256 * ns;
    std::vector<uint8_t> buf(header_bytes + records * spr * 2, ' ');

    writeField(buf, 0, 8, "0");
    writeField(buf, 8, 80, "TestPatient");
    writeField(buf, 88, 80, "Startdate");
    writeField(buf, 168, 8, ddmmyy);
    writeField(buf, 176, 8, hhmmss);
    writeField(buf, 184, 8, std::to_string(header_bytes));
    writeField(buf, 192, 44, "");
    writeField(buf, 236, 8, std::to_string(records));
    writeField(buf, 244, 8, "60");
    writeField(buf, 252, 4, std::to_string(ns));

    int base = 256;
    auto block = [&](int width, const std::string& v) {
        writeField(buf, base, width, v);
        base += ns * width;
    };
    block(16, "Flow.40ms");
    block(80, "");
    block(8, "L/s");
    block(8, "-10");
    block(8, "10");
    block(8, "-32768");
    block(8, "32767");
    block(80, "");
    block(8, std::to_string(spr));
    block(32, "");

    for (int i = 0; i < records * spr; ++i) {
        int16_t v = static_cast<int16_t>((i % 200) - 100);
        buf[header_bytes + i * 2]     = static_cast<uint8_t>(v & 0xFF);
        buf[header_bytes + i * 2 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }
    return buf;
}

// An EDF+ annotations file: one "EDF Annotations" signal, one record, TALs in
// the ResMed shape (+onset\x15duration\x14description\x14\0).
std::vector<uint8_t> buildEVE(const std::string& ddmmyy, const std::string& hhmmss,
                              const std::vector<std::pair<double, std::string>>& events) {
    const int ns = 1, spr = 512, records = 1;
    const int header_bytes = 256 + 256 * ns;
    const int annot_bytes = spr * 2;
    std::vector<uint8_t> buf(header_bytes + annot_bytes, ' ');

    writeField(buf, 0, 8, "0");
    writeField(buf, 8, 80, "TestPatient");
    writeField(buf, 88, 80, "Startdate");
    writeField(buf, 168, 8, ddmmyy);
    writeField(buf, 176, 8, hhmmss);
    writeField(buf, 184, 8, std::to_string(header_bytes));
    writeField(buf, 192, 44, "EDF+C");            // isEDFPlus() keys off this
    writeField(buf, 236, 8, std::to_string(records));
    writeField(buf, 244, 8, "60");
    writeField(buf, 252, 4, std::to_string(ns));

    int base = 256;
    auto block = [&](int width, const std::string& v) {
        writeField(buf, base, width, v);
        base += ns * width;
    };
    block(16, "EDF Annotations");
    block(80, "");
    block(8, "");
    block(8, "-32768");
    block(8, "32767");
    block(8, "-32768");
    block(8, "32767");
    block(80, "");
    block(8, std::to_string(spr));
    block(32, "");

    std::string tals;
    tals += "+0";
    tals += '\x14';
    tals += '\x14';
    tals += '\0';                                  // record timestamp, no description
    for (const auto& [onset, desc] : events) {
        tals += "+" + std::to_string(onset);
        tals += '\x15';
        tals += "10";                              // 10 s duration
        tals += '\x14';
        tals += desc;
        tals += '\x14';
        tals += '\0';
    }

    for (int i = 0; i < annot_bytes; ++i)
        buf[header_bytes + i] = (i < static_cast<int>(tals.size()))
                                    ? static_cast<uint8_t>(tals[i]) : 0;
    return buf;
}

void writeFile(const fs::path& p, const std::vector<uint8_t>& buf) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
}

// Every label the reference card writes that becomes an event. Measured byte-exact
// off the card: 741 'Obstructive Apnea', 534 'Hypopnea', 396 'Arousal',
// 233 'Central Apnea', 28 'Apnea'.
const std::vector<std::pair<std::string, EventType>> kCensus = {
    {"Obstructive Apnea", EventType::OBSTRUCTIVE},
    {"Hypopnea",          EventType::HYPOPNEA},
    {"Central Apnea",     EventType::CENTRAL},
    {"Apnea",             EventType::APNEA},
    {"Arousal",           EventType::RERA},
};

// The card's sixth label, 181 of them, one per EVE file. It is the reason the
// catch-all was a defect, but it is filtered by EDFFile::readAnnotations() before
// classification ever sees it -- see RecordingStartsIsDroppedBeforeClassification.
const char* kFileMarker = "Recording starts";

class EventClassification : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        dir_ = fs::temp_directory_path() / "cpapdash_event_classification";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        writeFile(dir_ / "20250812_233427_BRP.edf", buildBRP("12.08.25", "23:34:27"));
    }
    void TearDown() override { fs::remove_all(dir_); }

    // Parses a single EVE holding `labels`, one annotation each, and returns the
    // session. Onsets are spread so nothing collides.
    std::unique_ptr<ParsedSession> parseLabels(const std::vector<std::string>& labels) {
        std::vector<std::pair<double, std::string>> annots;
        double onset = 1.0;
        for (const auto& l : labels) {
            annots.emplace_back(onset, l);
            onset += 1.0;
        }
        writeFile(dir_ / "20250812_233427_EVE.edf",
                  buildEVE("12.08.25", "23:34:27", annots));
        return EDFParser::parseSession(dir_.string(), "dev", "ResMed");
    }
};

}  // namespace

// Each label the card writes reaches its intended type. Matched on the annotation
// text the parser carries in `details`, not on position.
TEST_F(EventClassification, EveryCensusLabelClassifiesToItsOwnType) {
    std::vector<std::string> labels;
    for (const auto& [label, _] : kCensus) labels.push_back(label);

    auto session = parseLabels(labels);
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(session->events.size(), kCensus.size());

    std::map<std::string, EventType> got;
    for (const auto& e : session->events) {
        ASSERT_TRUE(e.details.has_value());
        got[e.details.value()] = e.event_type;
    }

    for (const auto& [label, expected] : kCensus) {
        ASSERT_EQ(got.count(label), 1u) << "missing annotation: " << label;
        EXPECT_EQ(got[label], expected)
            << label << " classified as " << eventTypeToString(got[label])
            << ", expected " << eventTypeToString(expected);
    }
}

// The card's 181 'Recording starts' markers cannot inflate any count, because
// EDFFile::readAnnotations() drops that exact description before parseEVEFile runs
// (src/EDFFile.cpp, "Skip 'Recording starts' / 'Recording ends' markers"). This is
// asserted so the two layers stay honest about each other: if that filter is ever
// removed, the marker lands in OTHER by the test below, not in RERA.
TEST_F(EventClassification, RecordingStartsIsDroppedBeforeClassification) {
    auto session = parseLabels({kFileMarker, "Recording ends", "Hypopnea"});
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(session->events.size(), 1u);
    EXPECT_EQ(session->events[0].event_type, EventType::HYPOPNEA);

    ASSERT_TRUE(session->metrics.has_value());
    EXPECT_EQ(session->metrics->reras, 0);
    EXPECT_EQ(session->metrics->other_events, 0);
    EXPECT_EQ(session->metrics->total_events, 1);
}

// A marker ResMed spells differently gets past that filter and reaches the chain.
// It must land in OTHER: this is the shape of the original defect, where anything
// unrecognised became a RERA and inflated RIN.
TEST_F(EventClassification, AMarkerVariantThatSurvivesTheFilterIsOther) {
    auto session = parseLabels({"Recording starts (resumed)"});
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(session->events.size(), 1u);

    EXPECT_EQ(session->events[0].event_type, EventType::OTHER);
    ASSERT_TRUE(session->metrics.has_value());
    EXPECT_EQ(session->metrics->reras, 0);
    EXPECT_EQ(session->metrics->other_events, 1);
}

// The regression guard for the original defect. A label the chain does not know
// must land in OTHER; before SDD-004 it became a RERA and inflated RIN.
TEST_F(EventClassification, AnUnrecognisedLabelIsOtherNotRera) {
    auto session = parseLabels({"Sternutation Burst"});
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(session->events.size(), 1u);

    EXPECT_EQ(session->events[0].event_type, EventType::OTHER);
    EXPECT_NE(session->events[0].event_type, EventType::RERA);

    ASSERT_TRUE(session->metrics.has_value());
    EXPECT_EQ(session->metrics->reras, 0);
    EXPECT_EQ(session->metrics->other_events, 1);
}

// 'Arousal' keeps its RERA mapping, and now says so in its own branch: RIN computed
// from arousals alone reconciles against ResMed's RIN channel on 172 of 175 nights.
TEST_F(EventClassification, ArousalIsStillARera) {
    auto session = parseLabels({"Arousal", "Arousal", "Flow Limitation"});
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->metrics.has_value());

    // Only the arousals. The flow limitation is recorded, and is not a RERA.
    EXPECT_EQ(session->metrics->reras, 2);
    EXPECT_EQ(session->metrics->other_events, 1);
}

// THE INVARIANT: reclassifying does not change how many events there are. Every
// annotation is still recorded and still counted, OTHER included, so no user's
// displayed event count drops at the release.
TEST_F(EventClassification, TotalEventsIsUnchangedByTheReclassification) {
    std::vector<std::string> labels;
    for (const auto& [label, _] : kCensus) labels.push_back(label);
    labels.push_back("Flow Limitation");      // recognised by nothing in the chain
    labels.push_back("Sternutation Burst");   // and something nobody has a rule for

    auto session = parseLabels(labels);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->metrics.has_value());
    const auto& m = session->metrics.value();

    // Same input, same total: 7 annotations in, 7 events out.
    EXPECT_EQ(m.total_events, static_cast<int>(labels.size()));
    EXPECT_EQ(static_cast<int>(session->events.size()), m.total_events);

    // The old catch-all called all three of these RERAs, and that is exactly the
    // count that moved. The sum across the two buckets is what it used to report,
    // which is why the total did not move.
    EXPECT_EQ(m.reras, 1);
    EXPECT_EQ(m.other_events, 2);
    EXPECT_EQ(m.reras + m.other_events, 3);

    // And the rest of the census is where it belongs.
    EXPECT_EQ(m.obstructive_apneas, 1);
    EXPECT_EQ(m.hypopneas, 1);
    EXPECT_EQ(m.central_apneas, 1);
    EXPECT_EQ(m.unclassified_apneas, 1);
}

// The bare 'Apnea' bucket was counted toward AHI but never exposed, so a consumer
// adding up the stored columns got a short numerator
// (docs/RESMED_CALCULATION_RULES.md section 5).
TEST(EventCounts, UnclassifiedApneasAreExposed) {
    ParsedSession session;
    session.duration_seconds = 3600;  // 1 hour
    auto now = std::chrono::system_clock::now();

    session.events.push_back(SleepEvent(EventType::APNEA, now, 12.0));
    session.events.push_back(SleepEvent(EventType::APNEA, now, 14.0));
    session.events.push_back(SleepEvent(EventType::OBSTRUCTIVE, now, 15.0));

    session.calculateMetrics();
    ASSERT_TRUE(session.metrics.has_value());
    const auto& m = session.metrics.value();

    EXPECT_EQ(m.unclassified_apneas, 2);
    EXPECT_EQ(m.total_events, 3);
    // 3 AHI-relevant events over 1 hour. Reconstructing from the stored columns
    // gives the same number, which is the point of exposing the bucket.
    EXPECT_NEAR(m.ahi, 3.0, 1e-9);
    EXPECT_EQ(m.obstructive_apneas + m.central_apneas + m.clear_airway_apneas
                  + m.unclassified_apneas + m.hypopneas,
              3);
}

// The AHI numerator is obstructive + central + clear-airway + unclassified +
// hypopnea. RERA, OTHER and CSR are recorded and excluded.
TEST(EventCounts, AhiNumeratorExcludesReraOtherAndCsr) {
    ParsedSession session;
    session.duration_seconds = 7200;  // 2 hours
    auto now = std::chrono::system_clock::now();

    // Counts: 2 OA, 1 CA, 1 clear airway, 1 unclassified, 3 hypopnea = 8.
    session.events.push_back(SleepEvent(EventType::OBSTRUCTIVE, now, 15.0));
    session.events.push_back(SleepEvent(EventType::OBSTRUCTIVE, now, 16.0));
    session.events.push_back(SleepEvent(EventType::CENTRAL, now, 11.0));
    session.events.push_back(SleepEvent(EventType::CLEAR_AIRWAY, now, 13.0));
    session.events.push_back(SleepEvent(EventType::APNEA, now, 12.0));
    session.events.push_back(SleepEvent(EventType::HYPOPNEA, now, 10.0));
    session.events.push_back(SleepEvent(EventType::HYPOPNEA, now, 10.0));
    session.events.push_back(SleepEvent(EventType::HYPOPNEA, now, 10.0));
    // Excluded: 5 RERA, 4 OTHER, 2 CSR, 1 flow limitation, 1 snore, 1 large leak.
    for (int i = 0; i < 5; ++i)
        session.events.push_back(SleepEvent(EventType::RERA, now, 5.0));
    for (int i = 0; i < 4; ++i)
        session.events.push_back(SleepEvent(EventType::OTHER, now, 0.0));
    for (int i = 0; i < 2; ++i)
        session.events.push_back(SleepEvent(EventType::CSR, now, 30.0));
    session.events.push_back(SleepEvent(EventType::FLOW_LIMITATION, now, 4.0));
    session.events.push_back(SleepEvent(EventType::VIBRATORY_SNORE, now, 3.0));
    session.events.push_back(SleepEvent(EventType::LARGE_LEAK, now, 60.0));

    session.calculateMetrics();
    ASSERT_TRUE(session.metrics.has_value());
    const auto& m = session.metrics.value();

    EXPECT_EQ(m.total_events, 22);
    EXPECT_EQ(m.reras, 5);
    EXPECT_EQ(m.other_events, 4);

    // 8 / 2 hours = 4.0. Had RERA, OTHER or CSR leaked in it would read 9.5,
    // and had total_events been the numerator, 11.0.
    EXPECT_NEAR(m.ahi, 4.0, 1e-9);
}

// OTHER round-trips through the string mapping rather than falling out as "Unknown".
TEST(EventCounts, OtherRoundTripsThroughEventTypeToString) {
    EXPECT_EQ(eventTypeToString(EventType::OTHER), "Other");
    EXPECT_NE(eventTypeToString(EventType::OTHER), "Unknown");
    EXPECT_NE(eventTypeToString(EventType::OTHER), eventTypeToString(EventType::RERA));
}
