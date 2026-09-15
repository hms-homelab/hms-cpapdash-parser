#pragma once
//
// The one byte-level repair a ResMed signal file needs before a reader that
// trusts its header (OSCAR) sees it.
//
// ResMed writes num-data-records (EDF header offset 236, 8 ASCII bytes) as -1,
// or a count lower than the data, while a session is recording, and writes the
// true count only when the file is finalized. A pull taken mid-recording keeps
// that header. This library, CpapDash and SleepHQ compute the count from the
// file size, so they read such a file whole; OSCAR trusts the field and imports
// a night of a minute or two, or nothing.
//
// The rules are the ones cpapdash-ingest-lib's CardArchive.h has used for the
// OSCAR download and the SleepHQ archive since 2026-07-16; they live here so
// the public hms-cpap and the cloud share one copy.
//
// *** WHAT THIS DELIBERATELY DOES NOT DO. *** It never opens a file. The caller
// holds the bytes (a whole file, or its header) and decides where they go.
//
#include <cstddef>
#include <cstdint>
#include <string>

namespace cpapdash::parser {

/// A ResMed per-session SIGNAL file, by its name, case-insensitively:
/// *_BRP/_PLD/_SAD.edf, and on the 11 series *_SA2.edf (pulse and SpO2) and
/// *_TCV.edf (trigger/cycle events), written the same way. EVE/CSL
/// (annotations) and STR (summary) are not: they read fine and are left
/// untouched.
bool isResmedSignalEdf(const std::string& name);

/// Repair num-data-records IN [buf], which holds the whole file ([n] bytes).
/// Returns true when the field was rewritten.
///
/// The count is recomputed as (size - header bytes) / record bytes, the record
/// bytes being the sum of the samples per record times 2, and written ONLY when
/// the data divides evenly (the image is intact and the layout read is right)
/// and the stored value differs. Any doubt leaves the bytes as they are.
bool repairEdfDataRecords(char* buf, std::size_t n);

/// The same repair for a caller holding only the start of the file: [buf] has
/// [buf_len] bytes of it, at least the whole header, and [file_size] is the
/// size of the file on disk. Nothing past [buf_len] is read; a header longer
/// than the buffer is left alone. Rewrites the 8 bytes at offset 236 of [buf]
/// when it returns true; the caller writes them back.
bool repairEdfDataRecords(char* buf, std::size_t buf_len, std::uint64_t file_size);

/// The EDF header size a file declares (offset 184), or 0 when [buf] is too
/// short or the field is not a number: how much of a file to read before
/// calling the header-only form.
std::size_t edfHeaderBytes(const char* buf, std::size_t buf_len);

}  // namespace cpapdash::parser
