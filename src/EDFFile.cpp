#include "cpapdash/parser/EDFFile.h"
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <iostream>

namespace cpapdash::parser {

std::string EDFFile::trimField(const char* data, int len) {
    std::string s(data, len);
    // Trim trailing spaces and nulls
    auto end = s.find_last_not_of(" \0");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

bool EDFFile::readBytes(long long offset, char* dest, size_t count) const {
    if (from_buffer_) {
        if (offset < 0 || static_cast<size_t>(offset + count) > buffer_.size()) {
            return false;
        }
        std::memcpy(dest, buffer_.data() + offset, count);
        return true;
    } else {
        std::ifstream f(filepath_, std::ios::binary);
        if (!f.is_open()) return false;
        f.seekg(offset);
        f.read(dest, count);
        return f.gcount() == static_cast<std::streamsize>(count);
    }
}

bool EDFFile::parseHeader(const char* raw, long long file_size) {
    // --- Main header: 256 bytes ---
    if (file_size < 256) {
        std::cerr << "EDF: Data too small for header" << std::endl;
        return false;
    }

    const char* hdr = raw;

    // version (8)  -- should be "0       "
    // patient (80)
    patient = trimField(hdr + 8, 80);
    // recording (80)
    recording = trimField(hdr + 88, 80);
    // startdate dd.mm.yy (8)
    std::string date_str = trimField(hdr + 168, 8);
    // starttime hh.mm.ss (8)
    std::string time_str = trimField(hdr + 176, 8);
    // header_bytes (8)
    header_bytes = std::atoi(std::string(hdr + 184, 8).c_str());
    // reserved (44) -- "EDF+C" or "EDF+D" for EDF+
    reserved = trimField(hdr + 192, 44);
    // number of data records (8)
    num_records_header = std::atoi(std::string(hdr + 236, 8).c_str());
    // data record duration (8)
    record_duration = std::atof(std::string(hdr + 244, 8).c_str());
    // number of signals (4)
    num_signals = std::atoi(std::string(hdr + 252, 4).c_str());

    // Parse date: dd.mm.yy
    if (date_str.size() >= 8) {
        start_day   = std::atoi(date_str.substr(0, 2).c_str());
        start_month = std::atoi(date_str.substr(3, 2).c_str());
        int yy      = std::atoi(date_str.substr(6, 2).c_str());
        start_year  = (yy >= 85) ? 1900 + yy : 2000 + yy;
    }
    // Parse time: hh.mm.ss
    if (time_str.size() >= 8) {
        start_hour   = std::atoi(time_str.substr(0, 2).c_str());
        start_minute = std::atoi(time_str.substr(3, 2).c_str());
        start_second = std::atoi(time_str.substr(6, 2).c_str());
    }

    if (num_signals <= 0 || num_signals > 512) {
        std::cerr << "EDF: Invalid signal count " << num_signals << std::endl;
        return false;
    }

    // --- Signal headers: 256 bytes x num_signals ---
    int sig_hdr_size = 256 * num_signals;
    if (file_size < 256 + sig_hdr_size) {
        std::cerr << "EDF: Data too small for signal headers" << std::endl;
        return false;
    }

    const char* sig_hdr = raw + 256;

    signals.resize(num_signals);
    int pos = 0;

    // label (16 x ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].label = trimField(sig_hdr + pos, 16);
        pos += 16;
    }
    // transducer (80 x ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].transducer = trimField(sig_hdr + pos, 80);
        pos += 80;
    }
    // physical dimension (8 x ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].phys_dim = trimField(sig_hdr + pos, 8);
        pos += 8;
    }
    // physical minimum (8 x ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].phys_min = std::atof(std::string(sig_hdr + pos, 8).c_str());
        pos += 8;
    }
    // physical maximum (8 x ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].phys_max = std::atof(std::string(sig_hdr + pos, 8).c_str());
        pos += 8;
    }
    // digital minimum (8 x ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].dig_min = std::atoi(std::string(sig_hdr + pos, 8).c_str());
        pos += 8;
    }
    // digital maximum (8 x ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].dig_max = std::atoi(std::string(sig_hdr + pos, 8).c_str());
        pos += 8;
    }
    // prefiltering (80 x ns) -- skip
    pos += 80 * num_signals;
    // samples per data record (8 x ns)
    for (int i = 0; i < num_signals; ++i) {
        signals[i].samples_per_record = std::atoi(std::string(sig_hdr + pos, 8).c_str());
        pos += 8;
    }
    // reserved (32 x ns) -- skip

    // Compute scaling for each signal
    // physical = (digital - dig_min) * scale + phys_min
    // where scale = (phys_max - phys_min) / (dig_max - dig_min)
    for (auto& sig : signals) {
        int dig_range = sig.dig_max - sig.dig_min;
        if (dig_range != 0) {
            sig.scale  = (sig.phys_max - sig.phys_min) / dig_range;
            sig.offset = sig.phys_min - sig.dig_min * sig.scale;
        }
    }

    // Compute record size in bytes (each sample = 2 bytes for EDF)
    record_size_bytes = 0;
    for (auto& sig : signals) {
        record_size_bytes += sig.samples_per_record * 2;
    }

    // How many complete data records are actually in the data?
    long long data_bytes = file_size - header_bytes;
    if (record_size_bytes > 0) {
        actual_records = static_cast<int>(data_bytes / record_size_bytes);
    }

    // Completeness check
    if (num_records_header == -1) {
        complete = false;
        growing = true;
        extra_records = actual_records;
    } else if (actual_records > num_records_header) {
        complete = false;
        growing = true;
        extra_records = actual_records - num_records_header;
    } else if (actual_records == num_records_header) {
        complete = true;
        growing = false;
        extra_records = 0;
    } else {
        complete = false;
        growing = false;
        extra_records = 0;
    }

    return true;
}

bool EDFFile::open(const std::string& filepath) {
    filepath_ = filepath;
    from_buffer_ = false;

    std::ifstream f(filepath, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        std::cerr << "EDF: Cannot open " << filepath << std::endl;
        return false;
    }

    auto file_size = static_cast<long long>(f.tellg());
    f.seekg(0, std::ios::beg);

    // Read entire header (main + signal headers) into memory for parsing
    // We need at least 256 bytes; signal header size depends on num_signals
    // Read enough for main header first, then the rest
    int initial_read = std::min(file_size, static_cast<long long>(256 + 256 * 512));
    std::vector<char> hdr_buf(initial_read);
    f.read(hdr_buf.data(), initial_read);

    return parseHeader(hdr_buf.data(), file_size);
}

bool EDFFile::open(const uint8_t* data, size_t len) {
    from_buffer_ = true;
    buffer_.assign(data, data + len);

    return parseHeader(reinterpret_cast<const char*>(buffer_.data()),
                       static_cast<long long>(len));
}

std::chrono::system_clock::time_point EDFFile::getStartTime() const {
    std::tm t = {};
    t.tm_year = start_year - 1900;
    t.tm_mon  = start_month - 1;
    t.tm_mday = start_day;
    t.tm_hour = start_hour;
    t.tm_min  = start_minute;
    t.tm_sec  = start_second;
    t.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&t));
}

int EDFFile::findSignal(const std::string& partial_label) const {
    for (int i = 0; i < num_signals; ++i) {
        if (signals[i].label.find(partial_label) != std::string::npos) {
            return i;
        }
    }
    return -1;
}

int EDFFile::findSignalExact(const std::string& label) const {
    for (int i = 0; i < num_signals; ++i) {
        if (signals[i].label == label) {
            return i;
        }
    }
    return -1;
}

int EDFFile::readSignal(int signal_idx, std::vector<double>& out) {
    if (signal_idx < 0 || signal_idx >= num_signals || actual_records <= 0) {
        return 0;
    }

    const auto& sig = signals[signal_idx];
    int total_samples = actual_records * sig.samples_per_record;
    out.resize(total_samples);

    // Compute byte offset of this signal within each data record
    int sig_offset_in_record = 0;
    for (int i = 0; i < signal_idx; ++i) {
        sig_offset_in_record += signals[i].samples_per_record * 2;
    }

    int sample_out = 0;
    std::vector<int16_t> raw(sig.samples_per_record);

    for (int rec = 0; rec < actual_records; ++rec) {
        long long record_start = header_bytes + (long long)rec * record_size_bytes;
        long long read_offset = record_start + sig_offset_in_record;
        size_t read_size = sig.samples_per_record * 2;

        if (!readBytes(read_offset, reinterpret_cast<char*>(raw.data()), read_size)) {
            // Incomplete record -- stop here
            out.resize(sample_out);
            return sample_out;
        }

        for (int s = 0; s < sig.samples_per_record; ++s) {
            out[sample_out++] = raw[s] * sig.scale + sig.offset;
        }
    }

    return sample_out;
}

std::vector<EDFAnnotation> EDFFile::readAnnotations() {
    std::vector<EDFAnnotation> annotations;

    // Find annotation signal(s) -- label starts with "EDF Annotations"
    int annot_idx = -1;
    for (int i = 0; i < num_signals; ++i) {
        if (signals[i].label.find("EDF Annotations") != std::string::npos) {
            annot_idx = i;
            break;
        }
    }
    if (annot_idx < 0) return annotations;

    const auto& sig = signals[annot_idx];
    int annot_bytes_per_record = sig.samples_per_record * 2;

    // Compute byte offset of annotation signal in each record
    int sig_offset = 0;
    for (int i = 0; i < annot_idx; ++i) {
        sig_offset += signals[i].samples_per_record * 2;
    }

    std::vector<char> buf(annot_bytes_per_record);

    for (int rec = 0; rec < actual_records; ++rec) {
        long long record_start = header_bytes + (long long)rec * record_size_bytes;
        if (!readBytes(record_start + sig_offset, buf.data(), annot_bytes_per_record)) {
            break;
        }

        // Parse TAL (Time-stamped Annotation List)
        // Format: +onset\x15duration\x14description\x14\x00
        //   or:   +onset\x14description\x14\x00  (no duration)
        int pos = 0;
        while (pos < annot_bytes_per_record) {
            // Skip null padding
            while (pos < annot_bytes_per_record && buf[pos] == '\0') pos++;
            if (pos >= annot_bytes_per_record) break;

            // Read onset: starts with '+' or '-', ends with \x14 or \x15
            int tal_start = pos;
            if (buf[pos] != '+' && buf[pos] != '-') break;

            // Find the end of this TAL entry (\x00 terminates)
            int tal_end = pos;
            while (tal_end < annot_bytes_per_record && buf[tal_end] != '\0') tal_end++;

            std::string tal(buf.data() + tal_start, tal_end - tal_start);
            pos = tal_end + 1;

            // Parse the TAL string
            double onset = 0;
            double duration = -1;

            size_t first_sep = tal.find('\x14');
            if (first_sep == std::string::npos) continue;

            std::string onset_part = tal.substr(0, first_sep);

            // onset_part may contain \x15 separating onset from duration
            size_t dur_sep = onset_part.find('\x15');
            if (dur_sep != std::string::npos) {
                onset = std::atof(onset_part.substr(0, dur_sep).c_str());
                std::string dur_str = onset_part.substr(dur_sep + 1);
                if (!dur_str.empty()) duration = std::atof(dur_str.c_str());
            } else {
                onset = std::atof(onset_part.c_str());
            }

            // Everything after first \x14 are descriptions separated by \x14
            std::string rest = tal.substr(first_sep + 1);
            size_t dpos = 0;
            while (dpos < rest.size()) {
                size_t next = rest.find('\x14', dpos);
                if (next == std::string::npos) next = rest.size();
                std::string desc = rest.substr(dpos, next - dpos);
                dpos = next + 1;

                if (desc.empty()) continue;
                // Skip "Recording starts" / "Recording ends" markers
                if (desc == "Recording starts" || desc == "Recording ends") continue;

                EDFAnnotation annot;
                annot.onset_sec = onset;
                annot.duration_sec = duration;
                annot.description = desc;
                annotations.push_back(annot);
            }
        }
    }

    return annotations;
}

} // namespace cpapdash::parser
