#include "data_structures.h"
#include <algorithm>

OfflineData offline_store;
Telemetry data_store;
std::mutex mtx;
std::vector<std::string> log_messages;
int session_data_counter = 0;

void SignalHistory::add_points(const json& cells_array) {
    if (x.size() >= static_cast<size_t>(max_points)) {
        x.erase(x.begin());
        for (auto& [pci, vec] : streams_y)
            if (!vec.empty()) vec.erase(vec.begin());
    }
    x.push_back(current_step++);
    
    std::vector<int> present_pcis;
    for (const auto& cell : cells_array) {
        int pci = cell.value("pci", -1);
        if (pci < 0 || pci > 1000) continue;
        present_pcis.push_back(pci);
        if (streams_y.find(pci) == streams_y.end())
            streams_y[pci] = std::vector<float>(x.size() - 1, -145.0f);
        
        float rsrp = -145.0f;
        std::string type = cell.value("type", "");
        if (type == "LTE") rsrp = cell.value("rsrp", -145.0f);
        else if (type == "NR") rsrp = cell.value("ssRsrp", -145.0f);
        else if (type == "GSM") rsrp = cell.value("dbm", -145.0f);
        streams_y[pci].push_back(rsrp);
    }
    for (auto& [pci, vec] : streams_y) {
        if (std::find(present_pcis.begin(), present_pcis.end(), pci) == present_pcis.end())
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
    lats.clear();
    lons.clear();
    rsrps.clear();
    rsrqs.clear();        
    rssis.clear();        
    altitudes.clear();    
    earfcns.clear();
    pcis.clear(); 
    times.clear();
    indices.clear();
    loaded = false;
}