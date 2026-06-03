#include "data_structures.h"
#include <algorithm>

OfflineData offline_store;
Telemetry   data_store;
std::mutex  mtx;
std::vector<std::string> log_messages;
int session_data_counter = 0;

void SignalHistory::add_points(const json& cells_array, const std::string& metric) {

    if (x.size() >= static_cast<size_t>(max_points)) {
        x.erase(x.begin());
        for (auto& [pci, vec] : streams_y)
            if (!vec.empty()) vec.erase(vec.begin());
    }

    x.push_back(current_step++);

    std::vector<int> present_pcis;

    for (const auto& cell : cells_array) {
        int pci = cell.value("pci", -1);
        if (pci < 0 || pci > 1007) continue;

        present_pcis.push_back(pci);
        if (streams_y.find(pci) == streams_y.end())
            streams_y[pci] = std::vector<float>(x.size() - 1, -145.0f);

        float value = -145.0f;
        std::string type = cell.value("type", "");

        if (metric == "rsrp") {
            if      (type == "LTE") value = cell.value("rsrp",   -145.0f);
            else if (type == "NR")  value = cell.value("ssRsrp", -145.0f);
            else if (type == "GSM") value = cell.value("dbm",    -145.0f);

            if (value <= -145.0f && cell.contains("signal"))
                value = cell["signal"].value("rsrp", -145.0f);

        } else if (metric == "rssi") {
            value = cell.value("rssi", -120.0f);
            if (value <= -145.0f && cell.contains("signal"))
                value = cell["signal"].value("rssi", -120.0f);

        } else if (metric == "sinr") {
            if      (type == "LTE") value = cell.value("sinr",   -20.0f);
            else if (type == "NR")  value = cell.value("ssSinr", -20.0f);

            if (value <= -20.0f && cell.contains("signal")) {
                value = cell["signal"].value("sinr",   -20.0f);
                if (value <= -20.0f)
                    value = cell["signal"].value("ssSinr", -20.0f);
            }
        }

        streams_y[pci].push_back(value);
    }

    for (auto& [pci, vec] : streams_y) {
        bool present = std::find(present_pcis.begin(),
                                 present_pcis.end(), pci) != present_pcis.end();
        if (!present)
            vec.push_back(-145.0f);
    }
}

void SignalHistory::clear() {
    streams_y.clear();
    x.clear();
    current_step = 0;
}

bool SignalHistory::empty() const {
    return x.empty();
}

void OfflineData::clear() {
    lats.clear(); lons.clear();
    rsrps.clear(); rsrqs.clear(); rssis.clear();
    altitudes.clear(); earfcns.clear(); pcis.clear();
    times.clear(); indices.clear();
    loaded = false;
}
