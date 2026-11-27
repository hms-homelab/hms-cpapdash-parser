// One read-only AirSense 11 request and its answer, the AR01 stream (platform
// SDD-067).
//
// The AR01 streams here are built with the writer around the stored pages
// captured from the bench AirSense 11 (tests/fixtures/airsense11_pages, the
// same fixtures the page decoder was proven on), so a page that crosses the
// relay decodes as the page itself did. No serial is inside any of them.

#include <gtest/gtest.h>
#include "cpapdash/parser/AirSense11Request.h"
#include "cpapdash/parser/AirSense11Pages.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace cpapdash::parser {

namespace fs = std::filesystem;

#ifndef CPAPDASH_PARSER_SOURCE_DIR
#define CPAPDASH_PARSER_SOURCE_DIR "."
#endif

static std::string readFile(const std::string& rel) {
    for (const std::string& base : {std::string(CPAPDASH_PARSER_SOURCE_DIR) + "/",
                                    std::string(""), std::string("../"), std::string("../../")}) {
        std::error_code ec;
        if (fs::exists(base + rel, ec)) {
            std::ifstream f(base + rel, std::ios::binary);
            return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        }
    }
    return {};
}

static std::vector<uint8_t> page(const std::string& name) {
    const std::string s = readFile("tests/fixtures/airsense11_pages/" + name);
    return {s.begin(), s.end()};
}

static const char* kSent =
    R"({"id":7,"method":"StartSpool","params":{"spoolAddress":{"RespiratoryFlow6p25Hz":{"fromDateTime":"2026-11-25T06:57:42.000Z"}}}})";
static const char* kReply = R"({"id":7,"result":{"spoolId":3}})";
static const char* kEndMore =
    R"({"status":"SPOOL_COMPLETE_MORE_DATA_PENDING","seq":4,"nextSpoolAddress":{"RespiratoryFlow6p25Hz":{"fromDateTime":"2026-11-25T07:03:22.160Z"}}})";

// A spool answer: SENT, REPLY, the page as DATA records of at most `frag`
// bytes (as the machine's fragments come), END, OUTCOME.
static std::vector<uint8_t> spoolStream(const std::vector<uint8_t>& pg, size_t frag,
                                        const std::string& end_json, uint8_t end = 0) {
    AirSense11RequestWriter w;
    w.record(AirSense11RecordType::Sent, kSent);
    w.record(AirSense11RecordType::Reply, kReply);
    size_t frags = 0;
    for (size_t i = 0; i < pg.size(); i += frag, frags++)
        w.record(AirSense11RecordType::Data, pg.data() + i, std::min(frag, pg.size() - i));
    if (!end_json.empty()) w.record(AirSense11RecordType::End, end_json);
    AirSense11RequestOutcome o;
    o.end = end;
    o.fragments = static_cast<uint16_t>(frags);
    o.bytes = static_cast<uint32_t>(pg.size());
    if (end != 0) o.last_error = "machine disconnected mid-spool";
    w.outcome(o);
    return w.bytes();
}

TEST(AirSense11Request, RoundTripKeepsEveryRecordAndThePageDecodesAsItself) {
    const auto pg = page("RespiratoryFlow6p25Hz_first4.bin");
    ASSERT_FALSE(pg.empty());
    const auto r = decodeAirSense11Request(spoolStream(pg, 120, kEndMore));

    EXPECT_TRUE(r.broken.empty()) << r.broken;
    EXPECT_TRUE(r.complete());
    ASSERT_EQ(r.sent.size(), 1u);
    EXPECT_EQ(r.sent[0], kSent);
    ASSERT_EQ(r.replies.size(), 1u);
    EXPECT_EQ(r.replies[0], kReply);
    EXPECT_EQ(r.page, pg);
    EXPECT_EQ(r.data_records, (pg.size() + 119) / 120);
    ASSERT_TRUE(r.outcome);
    EXPECT_EQ(r.outcome->bytes, pg.size());

    // The relayed page decodes exactly as the page did.
    const auto direct  = AirSense11PagesParser::parseSignalPage(pg.data(), pg.size());
    const auto relayed = AirSense11PagesParser::parseSignalPage(r.page.data(), r.page.size());
    ASSERT_EQ(relayed.chunks.size(), direct.chunks.size());
    ASSERT_FALSE(relayed.chunks.empty());
    for (size_t i = 0; i < direct.chunks.size(); i++) {
        EXPECT_EQ(relayed.chunks[i].start_ms, direct.chunks[i].start_ms);
        EXPECT_EQ(relayed.chunks[i].raw, direct.chunks[i].raw);
    }
}

TEST(AirSense11Request, EventsPageCrossesTheRelay) {
    const auto pg = page("TherapyEvents-RespiratoryEvents.bin");
    ASSERT_FALSE(pg.empty());
    const auto r = decodeAirSense11Request(
        spoolStream(pg, 2808, R"({"status":"SPOOL_COMPLETE","seq":1})"));
    ASSERT_TRUE(r.complete()) << r.broken;
    const auto direct  = AirSense11PagesParser::parseEventsPage(pg.data(), pg.size());
    const auto relayed = AirSense11PagesParser::parseEventsPage(r.page.data(), r.page.size());
    ASSERT_FALSE(direct.empty());
    ASSERT_EQ(relayed.size(), direct.size());
    for (size_t i = 0; i < direct.size(); i++) {
        EXPECT_EQ(relayed[i].type, direct[i].type);
        EXPECT_EQ(relayed[i].onset_ms, direct[i].onset_ms);
        EXPECT_EQ(relayed[i].duration_ms, direct[i].duration_ms);
    }
    ASSERT_TRUE(r.end);
    EXPECT_TRUE(r.end->finished());
    EXPECT_FALSE(r.end->morePending());
}

TEST(AirSense11Request, ChunkBoundariesDoNotMatter) {
    const auto pg = page("Leak0p5Hz.bin");
    ASSERT_FALSE(pg.empty());
    const auto stream = spoolStream(pg, 2808, kEndMore);
    const auto whole = decodeAirSense11Request(stream);

    for (size_t piece : {size_t(1), size_t(2), size_t(3), size_t(7), size_t(4096)}) {
        AirSense11RequestReader rd;
        for (size_t i = 0; i < stream.size(); i += piece)
            ASSERT_TRUE(rd.feed(stream.data() + i, std::min(piece, stream.size() - i))) << piece;
        const auto r = rd.finish();
        EXPECT_TRUE(r.complete()) << piece << ": " << r.broken;
        EXPECT_EQ(r.page, whole.page) << piece;
        EXPECT_EQ(r.sent, whole.sent) << piece;
        EXPECT_EQ(r.replies, whole.replies) << piece;
        ASSERT_TRUE(r.end);
        EXPECT_EQ(r.end->next_from, whole.end->next_from) << piece;
    }
}

TEST(AirSense11Request, EndRecordGivesStatusSeqAndTheNextCursor) {
    const auto r = decodeAirSense11Request(spoolStream(page("Leak0p5Hz.bin"), 2808, kEndMore));
    ASSERT_TRUE(r.end);
    EXPECT_EQ(r.end->json, kEndMore);
    EXPECT_EQ(r.end->status, "SPOOL_COMPLETE_MORE_DATA_PENDING");
    EXPECT_TRUE(r.end->morePending());
    EXPECT_FALSE(r.end->finished());
    EXPECT_EQ(r.end->seq, 4);
    EXPECT_EQ(r.end->next_spool, "RespiratoryFlow6p25Hz");
    EXPECT_EQ(r.end->next_from, "2026-11-25T07:03:22.160Z");
}

TEST(AirSense11Request, EndWithoutACursorAndWithSpacing) {
    const auto r = decodeAirSense11Request(spoolStream(
        page("Leak0p5Hz.bin"), 2808, R"({ "status" : "ERROR_DATA_UNAVAILABLE" , "seq" : 0 })"));
    ASSERT_TRUE(r.end);
    EXPECT_EQ(r.end->status, "ERROR_DATA_UNAVAILABLE");
    EXPECT_TRUE(r.end->finished());
    EXPECT_EQ(r.end->seq, 0);
    EXPECT_TRUE(r.end->next_spool.empty());
    EXPECT_TRUE(r.end->next_from.empty());
}

// SDD-067 D5: a page that broke part way keeps what arrived.
TEST(AirSense11Request, BrokenPageKeepsWhatArrivedAndIsNotComplete) {
    const auto pg = page("InspiratoryPressure0p5Hz.bin");
    const std::vector<uint8_t> half(pg.begin(), pg.begin() + pg.size() / 2);
    const auto r = decodeAirSense11Request(spoolStream(half, 2808, "", /*disconnected*/ 3));
    EXPECT_TRUE(r.broken.empty());            // the stream itself is whole...
    EXPECT_FALSE(r.complete());               // ...but the request did not end as meant
    EXPECT_EQ(r.page, half);
    ASSERT_TRUE(r.outcome);
    EXPECT_EQ(std::string(airSense11RequestEndName(r.outcome->end)), "disconnected");
    EXPECT_EQ(r.outcome->last_error, "machine disconnected mid-spool");
    EXPECT_FALSE(r.end);
}

TEST(AirSense11Request, StreamCutBeforeOutcomeIsBrokenAndKeepsWholeRecords) {
    const auto pg = page("Leak0p5Hz.bin");
    // A stream that stops before OUTCOME (the link went with no last record).
    AirSense11RequestWriter w;
    w.record(AirSense11RecordType::Sent, kSent);
    w.record(AirSense11RecordType::Data, pg.data(), 1000);
    const auto r = decodeAirSense11Request(w.bytes());
    EXPECT_EQ(r.broken, "cut before OUTCOME");
    EXPECT_FALSE(r.complete());
    EXPECT_EQ(r.page.size(), 1000u);
    EXPECT_EQ(r.sent.size(), 1u);
}

TEST(AirSense11Request, StreamCutInsideARecordIsBroken) {
    const auto s = spoolStream(page("Leak0p5Hz.bin"), 2808, kEndMore);
    const auto r = decodeAirSense11Request(s.data(), s.size() - 5);   // inside OUTCOME
    EXPECT_EQ(r.broken, "cut inside a record");
    EXPECT_FALSE(r.complete());
    EXPECT_FALSE(r.outcome);
}

TEST(AirSense11Request, RefusesWhatIsNotAnAR01Stream) {
    const std::vector<uint8_t> junk = {'C', 'D', 'S', '1', 1, 0, 0};
    const auto r = decodeAirSense11Request(junk);
    EXPECT_EQ(r.broken, "not an AR01 stream");
    EXPECT_FALSE(decodeAirSense11Request(junk.data(), 2).broken.empty());   // too short even for the magic
    EXPECT_EQ(decodeAirSense11Request(nullptr, 0).broken, "not an AR01 stream");
}

TEST(AirSense11Request, RecordAfterOutcomeUnknownTypeAndShortOutcomeAreBroken) {
    {
        AirSense11RequestWriter w;
        w.outcome({});
        w.record(AirSense11RecordType::Data, std::string("x"));
        EXPECT_EQ(decodeAirSense11Request(w.bytes()).broken, "a record after OUTCOME");
    }
    {
        AirSense11RequestWriter w;
        const uint8_t b = 0;
        w.record(static_cast<AirSense11RecordType>(9), &b, 1);
        EXPECT_EQ(decodeAirSense11Request(w.bytes()).broken, "unknown record type 9");
    }
    {
        AirSense11RequestWriter w;
        w.record(AirSense11RecordType::Outcome, std::string("short"));
        EXPECT_EQ(decodeAirSense11Request(w.bytes()).broken, "OUTCOME too short");
    }
}

TEST(AirSense11Request, OutcomeFieldsAreLittleEndianAndSigned) {
    AirSense11RequestOutcome o;
    o.result = 4; o.end = 1; o.hci_reason = 0x0213; o.rpc_error = -11002;
    o.fragments = 300; o.bytes = 70000; o.last_error = "TooManySignalsRequested";
    AirSense11RequestWriter w;
    w.outcome(o);
    const auto r = decodeAirSense11Request(w.bytes());
    ASSERT_TRUE(r.outcome);
    EXPECT_EQ(r.outcome->result, 4);
    EXPECT_EQ(std::string(airSense11RequestEndName(r.outcome->end)), "rpc_error");
    EXPECT_EQ(r.outcome->hci_reason, 0x0213);
    EXPECT_EQ(r.outcome->rpc_error, -11002);
    EXPECT_EQ(r.outcome->fragments, 300);
    EXPECT_EQ(r.outcome->bytes, 70000u);
    EXPECT_EQ(r.outcome->last_error, "TooManySignalsRequested");
    EXPECT_STREQ(airSense11RequestEndName(200), "unknown");
}

TEST(AirSense11Request, WriterRefusesARecordLongerThanU16) {
    AirSense11RequestWriter w;
    const std::vector<uint8_t> big(0x10000, 0xAB);
    const size_t before = w.bytes().size();
    EXPECT_FALSE(w.record(AirSense11RecordType::Data, big.data(), big.size()));
    EXPECT_EQ(w.bytes().size(), before);
}

// ── the selector list (SDD-067 D1, D6) ──

TEST(AirSense11Selectors, TheRepoListIsTheFiveStoredPageSpoolsAndNoGet) {
    const std::string text = readFile("data/as11_selectors.json");
    ASSERT_FALSE(text.empty());
    const auto sel = AirSense11Selectors::parse(text);
    ASSERT_TRUE(sel);
    EXPECT_EQ(sel->spool, (std::vector<std::string>{
                              "TherapyEvents-RespiratoryEvents", "RespiratoryFlow6p25Hz",
                              "MaskPressure6p25Hz", "Leak0p5Hz", "InspiratoryPressure0p5Hz"}));
    EXPECT_TRUE(sel->get.empty());   // D6: spools only in the first release
    EXPECT_TRUE(sel->allowsSpool("Leak0p5Hz"));
    EXPECT_FALSE(sel->allowsSpool("Settings"));
    EXPECT_FALSE(sel->allowsGet("SerialNumber"));
}

TEST(AirSense11Selectors, RefusesNamesTheFirmwareWouldRefuse) {
    EXPECT_TRUE(AirSense11Selectors::parse(R"({"spool":["A-b_9"],"get":["FGState"]})"));
    EXPECT_FALSE(AirSense11Selectors::parse(R"({"spool":["bad name"],"get":[]})"));
    EXPECT_FALSE(AirSense11Selectors::parse(R"({"spool":["x\"}],\"y"],"get":[]})"));
    EXPECT_FALSE(AirSense11Selectors::parse(R"({"spool":[""],"get":[]})"));
    EXPECT_FALSE(AirSense11Selectors::parse(R"({"spool":["Leak0p5Hz"]})"));   // no get
    EXPECT_FALSE(AirSense11Selectors::parse(R"({"spool":"Leak0p5Hz","get":[]})"));
    EXPECT_FALSE(AirSense11Selectors::parse(R"({"spool":["Leak0p5Hz",],"get":[]})"));
    EXPECT_FALSE(AirSense11Selectors::parse("not json"));
    EXPECT_TRUE(AirSense11Selectors::parse(R"({ "spool" : [ ] , "get" : [ ] })"));
}

TEST(AirSense11Selectors, NameLengthAndCharacters) {
    EXPECT_TRUE(isAirSense11SelectorName(std::string(47, 'a')));
    EXPECT_FALSE(isAirSense11SelectorName(std::string(48, 'a')));
    EXPECT_TRUE(isAirSense11SelectorName("TherapyEvents-RespiratoryEvents"));
    for (const char* bad : {"", "a b", "a.b", "a/b", "a\"b", "a{b", "a\\b", "é"})
        EXPECT_FALSE(isAirSense11SelectorName(bad)) << bad;
}

}  // namespace cpapdash::parser
