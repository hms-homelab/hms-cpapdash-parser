// Reads a BMC / React Health Luna card folder and reports what BmcParser made of
// it: every session, its duration, its apneas and the index, and the card-level
// notes (refused packets, duplicate seconds, clock order, span types).
//
// A tool and not a test: it runs on a real person's card, which never enters this
// repository. SDD-007 gate 2 is this output on the donor card.
//
//   bmc_probe <card folder>

#include "cpapdash/parser/BmcParser.h"

#include <cstdio>
#include <ctime>

using namespace cpapdash::parser;

namespace {

std::string fmt(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm lt{};
    localtime_r(&t, &lt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <card folder>\n", argv[0]);
        return 2;
    }
    const auto card = BmcParser::readCard(argv[1]);
    if (!card) {
        std::fprintf(stderr, "no BMC card (serial-named raw files + .evt) in %s\n", argv[1]);
        return 1;
    }
    const auto& n = card->notes;
    std::printf("packets %d, refused %d, duplicate seconds %d, out of order %s\n", n.packets,
                n.refused_packets, n.duplicate_seconds, n.out_of_order ? "yes" : "no");
    std::printf("event records %zu, refused %d, apneas outside every session %d\n",
                card->events.size(), n.refused_event_records, n.events_outside_session);
    std::printf("sessions %d\n", n.sessions);

    int total_events = 0;
    BmcNotes all;
    for (const auto& span : card->sessions) {
        BmcNotes per;
        auto s = BmcParser::sessionFromCard(*card, span, "probe", "BMC", &per);
        const auto& m = *s->metrics;
        std::printf("  %s -> %s  %6d s  (#%d)  OA %d  CA %d  AHI %.2f  IPAP %.2f  EPAP %.2f%s\n",
                    fmt(*s->session_start).c_str(), fmt(*s->session_end).c_str(),
                    *s->duration_seconds, span.session_number, m.obstructive_apneas,
                    m.clear_airway_apneas, m.ahi, m.avg_therapy_pressure.value_or(-1),
                    m.avg_epr_pressure.value_or(-1),
                    m.index_kind == SessionMetrics::IndexKind::Ungraded ? " (ungraded)" : "");
        total_events += per.events_emitted;
        for (const auto& [type, count] : per.span_types) all.span_types[type] += count;
    }
    std::printf("apneas placed %d\n", total_events);
    std::printf("span types (not events):");
    for (const auto& [type, count] : all.span_types) std::printf(" %d:%d", type, count);
    std::printf("\n");
    return 0;
}
