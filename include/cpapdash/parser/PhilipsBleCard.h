#pragma once

#ifdef CPAPDASH_WITH_PHILIPS

// PhilipsBleCard - a DreamStation 2 card tree, or parsed sessions, from the
// records the machine returns over Bluetooth.
//
// After a session closes, a DreamStation 2 serves it over Bluetooth as one
// record per stream, and each record body is byte for byte the PLAINTEXT card
// file of that session (docs/PHILIPS_FORMAT.md 9.10):
//
//   stream 1  <session hex>.B01   summary
//   stream 2  <session hex>.B02   events
//   stream 3  D/NNN.B03           D log
//   stream 4  E/NNN.B04           E log
//
// There is no .B05 waveform over Bluetooth. This header takes those records
// (from whatever reads the machine; the parser does not depend on a Bluetooth
// library) and either writes the card tree a freshly inserted card would hold,
// which PhilipsParser::parseSession reads unchanged, or parses the records
// straight into sessions without touching the disk.
//
// PROVENANCE: docs/PHILIPS_FORMAT.md sections 9.2 (container, CRC) and 9.10
// (card tree, LOG.SEQ, PROP.BIN), measured on a real DreamStation 2 card [S].
// No OSCAR source.

#include "cpapdash/parser/Models.h"
#include "cpapdash/parser/PhilipsParser.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cpapdash::parser {

/** One record as the machine returned it over Bluetooth. */
struct Ds2BleRecord {
    uint8_t stream = 0;          ///< 1 summary, 2 events, 3 D log, 4 E log
    uint32_t session = 0;        ///< session number (streams 1 and 2; not used for 3 and 4)
    std::vector<uint8_t> body;   ///< the reassembled body: one or more v3 blocks
};

/** A stream's read cursor, the 10 bytes the machine keeps in LOG.SEQ (doc 9.10). */
using Ds2Bookmark = std::array<uint8_t, 10>;

/** What the machine says about itself over Bluetooth. */
struct Ds2MachineIdentity {
    std::string serial;      ///< PROP.BIN SN
    std::string model;       ///< PROP.BIN MN, e.g. "520X110C"
    std::string firmware;    ///< PROP.BIN SV, e.g. "V1.0.2.3590"
};

/** What a card tree needs that the records do not carry. */
struct Ds2CardOptions {
    /// The machine's folder under P-SERIES/ (8 characters on a real card).
    /// Required: it is in no record and is not derived from the serial.
    std::string machine_folder;

    /// Final cursor of each stream 1..4 (LOG.SEQ; SK, DK, EK of PROP.BIN).
    /// A stream left out is written as zeros, with a warning.
    std::map<int, Ds2Bookmark> bookmarks;

    /// PROP.BIN LD, the machine's clock when it wrote the card (unix seconds).
    /// Default: the header time of the first D or E log block, which the
    /// machine stamps at the read. With neither, build() fails.
    std::optional<uint32_t> written_at;

    /// PROP.BIN keys the records cannot supply (CF, PT, DF, FD, SID, TS, DC),
    /// copied from a card of the same machine. A key left out is written with
    /// an empty value, with a warning; the parser reads none of them.
    std::map<std::string, std::string> prop_constants;
};

/** A card tree in memory. */
struct Ds2CardTree {
    /// Path relative to the card root ("P-SERIES/<folder>/P0/00000779.B01")
    /// -> file bytes.
    std::map<std::string, std::vector<uint8_t>> files;
    std::vector<std::string> warnings;
    /// Non-empty when the records cannot make a tree; files is then empty.
    std::string error;

    bool ok() const { return error.empty(); }
    /// The session folder to hand to PhilipsParser ("P-SERIES/<folder>/P0").
    std::string session_dir;
};

class Ds2CardWriter {
public:
    /// The PROP.BIN keys in the order the machine writes them, before VC.
    static const std::vector<std::string>& propKeyOrder();

    /// More files than this in P0 is refused: the machine's rollover to P1 is
    /// not measured.
    static constexpr size_t kMaxFilesPerFolder = 500;

    /**
     * Check every record and lay out the tree. Every block must pass its
     * header sum and data CRC and the blocks must fill the body exactly; a
     * stream 1 or 2 block must carry the record's session number and its
     * stream as the file extension. Any failure sets error and writes nothing.
     */
    static Ds2CardTree build(const std::vector<Ds2BleRecord>& records,
                             const Ds2MachineIdentity& identity,
                             const Ds2CardOptions& options);

    /** Write a built tree under card_root. false (and *error) on failure. */
    static bool write(const Ds2CardTree& tree, const std::string& card_root,
                      std::string* error = nullptr);

    /**
     * LOG.SEQ, 74 bytes (doc 9.10): DFN, EFN, PFN, FN each as u32 LE + 0x5a,
     * the bookmarks of streams 0..4 (absent = zeros), then the v3 CRC of the
     * 70 bytes before it, u32 LE.
     */
    static std::vector<uint8_t> buildLogSeq(uint32_t d_files, uint32_t e_files,
                                            uint32_t p_files, uint32_t p_folder,
                                            const std::map<int, Ds2Bookmark>& bookmarks);

    /**
     * PROP.BIN (doc 9.10): "KEY=VALUE\r\n" for every key of propKeyOrder()
     * (absent = empty value), then "VC=0x%08X" with no line end, VC being the
     * v3 CRC of every byte before it, "VC=" included.
     */
    static std::vector<uint8_t> buildPropBin(const std::map<std::string, std::string>& values);

    /** A bookmark as PROP.BIN prints it: "0x" + offset, stream, counter, check. */
    static std::string bookmarkToProp(const Ds2Bookmark& bookmark);
};

/**
 * The direct path: the records parsed into sessions, no files. One session per
 * session number with a usable summary, ascending; each is the session
 * PhilipsParser::parseSession returns for the same night from the written tree.
 * A record that fails its checks is skipped and counted in *warnings.
 */
std::vector<std::unique_ptr<ParsedSession>> parseDs2BleRecords(
    const std::vector<Ds2BleRecord>& records,
    const Ds2MachineIdentity& identity,
    const std::string& device_id,
    const std::string& device_name,
    PhilipsParseWarnings* warnings = nullptr);

} // namespace cpapdash::parser

#endif // CPAPDASH_WITH_PHILIPS
