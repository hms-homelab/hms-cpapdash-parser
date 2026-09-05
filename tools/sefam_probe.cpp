// sefam_probe -- read a Sefam card with the library and report what it found.
//
// SDD-005 section 8 calls for running the parser over a donor card and checking
// what comes back. This is that: point it at a card root, a serial folder or a
// single DATA_<n> folder and it parses every session under it and prints one
// line each, plus the block-checksum tally that says whether the framing was
// read correctly.
//
// Build:
//   cmake -S . -B build -DCPAPDASH_PARSER_WITH_SEFAM=ON -DCPAPDASH_PARSER_BUILD_TOOLS=ON
//   ./build/sefam_probe <path>

#include "cpapdash/parser/SefamParser.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace cpapdash::parser;
namespace fs = std::filesystem;

namespace {

// A folder holding at least one session manifest. Under the DATA_<n> layout that
// is one session; under the older <YYMMDD> layout it is every recording that
// started that day.
void collect(const fs::path& root, std::vector<std::pair<fs::path, std::string>>& out,
             int depth = 5) {
    std::error_code ec;
    if (depth < 0 || !fs::is_directory(root, ec)) return;

    for (const auto& stem : SefamParser::listSessionStems(root.string()))
        out.emplace_back(root, stem);

    for (const auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory(ec)) collect(e.path(), out, depth - 1);
}

std::string when(const std::optional<std::chrono::system_clock::time_point>& t) {
    if (!t) return "-";
    const std::time_t e = std::chrono::system_clock::to_time_t(*t);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &e);
#else
    localtime_r(&e, &local);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &local);
    return buf;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: sefam_probe <card root | serial folder | DATA_<n>>\n";
        return 2;
    }

    std::vector<std::pair<fs::path, std::string>> sessions;
    collect(argv[1], sessions);

    if (sessions.empty()) {
        std::cerr << "no Sefam session manifests under " << argv[1] << "\n";
        return 1;
    }

    std::printf("%-14s %-16s %6s %6s %6s %6s %5s %7s  %s\n",
                "session", "start", "hours", "press", "leak", "flow", "blk", "blocks",
                "notes");

    size_t parsed = 0, refused = 0, blocks = 0, bad = 0;
    std::map<int, double> bit_share_total;
    size_t bit_share_n = 0;

    for (const auto& [dir, stem] : sessions) {
        SefamParser parser;
        auto s = parser.parseSessionNamed(dir.string(), stem, "probe", "");
        const auto& notes = parser.lastNotes();

        if (!s) {
            ++refused;
            std::printf("%-14s %-16s %6s %6s %6s %6s %5s %7s  REFUSED\n",
                        stem.c_str(), "-", "-", "-", "-", "-", "-", "-");
            continue;
        }
        ++parsed;

        double press = 0, leak = 0, flow = 0;
        int np = 0, nl = 0;
        for (const auto& r : s->breathing_summary) {
            if (r.therapy_pressure) { press += *r.therapy_pressure; ++np; }
            if (r.leak_rate) { leak += *r.leak_rate; ++nl; }
            flow += r.avg_flow_rate;
        }
        if (np) press /= np;
        if (nl) leak /= nl;
        if (!s->breathing_summary.empty()) flow /= s->breathing_summary.size();

        size_t sb = 0, sbad = 0;
        for (const auto& [name, f] : notes.framing) { sb += f.blocks; sbad += f.checksum_failures; }
        blocks += sb;
        bad += sbad;

        for (const auto& [bit, share] : notes.det_bit_share) bit_share_total[bit] += share;
        if (!notes.det_bit_share.empty()) ++bit_share_n;

        std::string note;
        if (!s->breaths.empty()) {
            double rr = 0; int n = 0;
            for (const auto& b : s->breathing_summary)
                if (b.respiratory_rate) { rr += *b.respiratory_rate; ++n; }
            char buf[48];
            std::snprintf(buf, sizeof buf, "breaths:%zu rr:%.1f ", s->breaths.size(),
                          n ? rr / n : 0.0);
            note += buf;
        }
        if (!notes.stub_channels.empty()) note += "stubs:" + std::to_string(notes.stub_channels.size()) + " ";
        if (!notes.undeclared_files.empty()) note += "undeclared:" + std::to_string(notes.undeclared_files.size()) + " ";
        if (!s->vitals.empty()) note += "vitals:" + std::to_string(s->vitals.size()) + " ";
        for (const auto& [name, f] : notes.framing)
            if (f.truncated_tail) { note += "truncated:" + name + " "; }

        char framing[16];
        std::snprintf(framing, sizeof framing, "%ds/%dB",
                      notes.layout.seconds, notes.layout.trailer_bytes);

        std::printf("%-14s %-16s %6.2f %6.2f %6.2f %6.2f %5s %7zu  %s\n",
                    stem.c_str(), when(s->session_start).c_str(),
                    s->duration_seconds.value_or(0) / 3600.0, press, leak, flow,
                    framing, sb, note.c_str());
    }

    std::printf("\n%zu sessions: %zu parsed, %zu refused\n",
                sessions.size(), parsed, refused);
    std::printf("%zu blocks, %zu failing checksums\n", blocks, bad);

    if (bit_share_n) {
        std::printf("\nDET bits, mean share of a recording (meanings unknown):\n");
        for (const auto& [bit, total] : bit_share_total)
            std::printf("  bit %d (0x%02x): %5.2f%%\n", bit, 1 << bit,
                        100.0 * total / bit_share_n);
    }
    return 0;
}
