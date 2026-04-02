#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>

namespace sleeplink::parser {

/**
 * Raw EDF signal descriptor (parsed from header)
 */
struct EDFSignal {
    std::string label;
    std::string transducer;
    std::string phys_dim;
    double phys_min = 0;
    double phys_max = 0;
    int dig_min = 0;
    int dig_max = 0;
    int samples_per_record = 0;
    // Computed scaling: physical = digital * scale + offset
    double scale = 1.0;
    double offset = 0.0;
};

/**
 * EDF+ annotation entry
 */
struct EDFAnnotation {
    double onset_sec;       // Seconds from recording start
    double duration_sec;    // Duration in seconds (-1 if unused)
    std::string description;
};

/**
 * Raw EDF file reader -- tolerant of ResMed non-standard files.
 *
 * edflib 1.27 rejects ResMed CPAP files (empty phys_dim on Crc16 signal,
 * EDF+D with 0-second datarecords). This reader handles those quirks and
 * also handles incomplete/growing files (partial last data record).
 *
 * Supports both file-based and memory-buffer-based opening.
 */
class EDFFile {
public:
    // Header fields
    std::string patient;
    std::string recording;
    int start_year = 0, start_month = 0, start_day = 0;
    int start_hour = 0, start_minute = 0, start_second = 0;
    int header_bytes = 0;
    std::string reserved;       // "EDF+C", "EDF+D", or empty for plain EDF
    int num_records_header = 0; // From header (-1 means unknown/recording)
    double record_duration = 0; // Seconds per data record
    int num_signals = 0;
    std::vector<EDFSignal> signals;

    // Computed
    int record_size_bytes = 0;  // Bytes per data record (all signals)
    int actual_records = 0;     // Records actually present in file
    int extra_records = 0;      // Records beyond header declaration
    bool complete = false;      // true if actual_records == num_records_header
    bool growing = false;       // true if actual_records > num_records_header

    /** Open from file path. */
    bool open(const std::string& filepath);

    /** Open from memory buffer. Copies data internally. */
    bool open(const uint8_t* data, size_t len);

    /** Read all physical samples for one signal. Returns sample count. */
    int readSignal(int signal_idx, std::vector<double>& out);

    /** Read EDF+ annotations from annotation signal(s). */
    std::vector<EDFAnnotation> readAnnotations();

    /** Get recording start as system_clock time_point. */
    std::chrono::system_clock::time_point getStartTime() const;

    /** Find signal index whose label contains partial_label (-1 if not found). */
    int findSignal(const std::string& partial_label) const;

    /** Find signal index whose label matches exactly (after trimming). -1 if not found. */
    int findSignalExact(const std::string& label) const;

    bool isEDFPlus() const { return reserved.find("EDF+") != std::string::npos; }

private:
    std::string filepath_;
    std::vector<uint8_t> buffer_;  // For memory-buffer mode
    bool from_buffer_ = false;

    static std::string trimField(const char* data, int len);

    /** Parse header from raw bytes. file_size is total available bytes. */
    bool parseHeader(const char* raw, long long file_size);

    /** Read bytes from the backing store (file or buffer). */
    bool readBytes(long long offset, char* dest, size_t count) const;
};

} // namespace sleeplink::parser
