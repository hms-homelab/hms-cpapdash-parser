#include "sleeplink/parser/EDFParser.h"
#include <iostream>
#include <algorithm>

namespace sleeplink::parser {

bool EDFParser::parseSADFile(EDFFile& edf, ParsedSession& session) {
    auto start_time = edf.getStartTime();

    // Find SpO2 and Pulse/HR signals
    int spo2_idx  = edf.findSignal("SpO2");
    int pulse_idx = edf.findSignal("Pulse");
    if (pulse_idx < 0) pulse_idx = edf.findSignal("Heart");
    if (pulse_idx < 0) pulse_idx = edf.findSignal("HR");

    std::vector<double> spo2_data, pulse_data;
    if (spo2_idx >= 0)  edf.readSignal(spo2_idx, spo2_data);
    if (pulse_idx >= 0) edf.readSignal(pulse_idx, pulse_data);

    // Both at 1 Hz -- one sample per second
    int n_samples = static_cast<int>(std::max(spo2_data.size(), pulse_data.size()));

    for (int i = 0; i < n_samples; ++i) {
        VitalSample vital(start_time + std::chrono::seconds(i));

        if (i < static_cast<int>(spo2_data.size())) {
            double val = spo2_data[i];
            // Filter invalid readings (0 = sensor disconnected)
            if (val > 0 && val <= 100) {
                vital.spo2 = val;
            }
        }
        if (i < static_cast<int>(pulse_data.size())) {
            int val = static_cast<int>(pulse_data[i]);
            // Filter invalid readings
            if (val > 0 && val < 300) {
                vital.heart_rate = val;
            }
        }

        session.vitals.push_back(vital);
    }

    return true;
}

} // namespace sleeplink::parser
