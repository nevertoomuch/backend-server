#include "idw_heatmap.h"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <vector>

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image_write.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

IDWHeatmapGenerator::IDWHeatmapGenerator() = default;

double IDWHeatmapGenerator::haversineDistance(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double a = std::sin(dLat/2) * std::sin(dLat/2) +
               std::cos(lat1 * M_PI / 180.0) * std::cos(lat2 * M_PI / 180.0) *
               std::sin(dLon/2) * std::sin(dLon/2);
    return R * 2 * std::atan2(std::sqrt(a), std::sqrt(1-a));
}

double IDWHeatmapGenerator::fastDistanceMeters(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0 * std::cos(lat1 * M_PI / 180.0);
    return R * std::sqrt(dLat*dLat + dLon*dLon);
}

double IDWHeatmapGenerator::getCriterionValue(const MapPoint& p, HeatmapCriterion crit) {
    switch(crit) {
        case HeatmapCriterion::RSRP: return p.rsrp;
        case HeatmapCriterion::RSRQ: return p.rsrq;
        case HeatmapCriterion::RSSI: return p.rssi;
        case HeatmapCriterion::Altitude: return p.altitude;
        default: return p.rsrp;
    }
}

IDWHeatmapGenerator::Color IDWHeatmapGenerator::valueToColor(double value, HeatmapCriterion crit) {
    Color c{0, 0, 0, 0};
    if (crit == HeatmapCriterion::RSRP) {
        if (value > -80) c = {255, 0, 0, 255};
        else if (value > -90) { float t = (-80 - value)/10.0f; c = {255, (uint8_t)(255*t), 0, 255}; }
        else if (value > -100) { float t = (-90 - value)/10.0f; c = {(uint8_t)(255*(1-t)), 255, 0, 255}; }
        else if (value > -110) { float t = (-100 - value)/10.0f; c = {0, (uint8_t)(255*(1-t)), (uint8_t)(255*t), 255}; }
    }
    else if (crit == HeatmapCriterion::RSRQ) {
        if (value > -10) c = {0, 255, 0, 255};
        else if (value > -15) { float t = (-10 - value)/5.0f; c = {(uint8_t)(255*t), 255, 0, 255}; }
        else if (value > -20) { float t = (-15 - value)/5.0f; c = {255, (uint8_t)(255*(1-t)), 0, 255}; }
    }
    else if (crit == HeatmapCriterion::RSSI) {
        if (value > -70) c = {0, 255, 0, 255};
        else if (value > -90) { float t = (-70 - value)/20.0f; c = {(uint8_t)(255*t), 255, 0, 255}; }
        else if (value > -110) { float t = (-90 - value)/20.0f; c = {255, (uint8_t)(255*(1-t)), 0, 255}; }
    }
    else if (crit == HeatmapCriterion::Altitude) {
        float t = std::clamp((float)value / 300.0f, 0.0f, 1.0f);
        c = {(uint8_t)(255*t), (uint8_t)(255*std::min(1.0f, t*2)), (uint8_t)(255*(1-t)), 255};
    }
    return c;
}

double IDWHeatmapGenerator::interpolateIDW(double lat, double lon,
    const std::vector<MapPoint>& neighbors, const HeatmapConfig& config) {
    if (neighbors.empty()) return NAN;
    
    double num = 0.0;
    double den = 0.0; 
    bool ok = false;
    
    bool is_log_scale = (config.criterion == HeatmapCriterion::RSRP || 
                         config.criterion == HeatmapCriterion::RSRQ || 
                         config.criterion == HeatmapCriterion::RSSI);

    for (const auto& p : neighbors) {
        double d = fastDistanceMeters(lat, lon, p.lat, p.lon);
        if (d < config.min_radius_m || d > config.max_radius_m) continue;
        
        double v = getCriterionValue(p, config.criterion);
        if (std::isnan(v) || v < -200) continue;
        
        double w = 1.0 / (d * d);
        
        if (is_log_scale) {
            double v_lin = std::pow(10.0, v / 10.0);
            num += w * v_lin;
        } else {
            num += w * v;
        }
        
        den += w; 
        ok = true;
    }
    
    if (!ok || den <= 1e-10) return NAN;
    
    double result = num / den;
    
    if (is_log_scale) {
        result = 10.0 * std::log10(result);
    }
    
    return result;
}

bool IDWHeatmapGenerator::savePNG(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    return stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4) != 0;
}

void IDWHeatmapGenerator::cancel() {
    std::lock_guard<std::mutex> lock(cancel_mutex_);
    running_ = false;
}

bool IDWHeatmapGenerator::generate(const std::vector<MapPoint>& points,
    const HeatmapConfig& config,
    double lat_min, double lon_min, double lat_max, double lon_max,
    const std::string& output_path) {
    
    running_ = true; progress_ = 0.0f;
    int img_w = config.img_width; int img_h = config.img_height;
    std::vector<uint8_t> image(img_w * img_h * 4, 0);

    std::vector<MapPoint> candidates;
    candidates.reserve(points.size());
    for (const auto& p : points) {
        if (config.pci_filter >= 0 && p.pci != config.pci_filter) continue;
        if (config.earfcn_filter >= 0 && p.earfcn != config.earfcn_filter) continue;
        if (p.lat >= lat_min - 0.02 && p.lat <= lat_max + 0.02 &&
            p.lon >= lon_min - 0.02 && p.lon <= lon_max + 0.02) {
            candidates.push_back(p);
        }
    }

    if (candidates.empty()) {
        std::cerr << "[Heatmap] No points with selected filters.\n";
        running_ = false; return false;
    }
    std::cout << "[Heatmap] Using " << candidates.size() << " points (PCI Filter: " << config.pci_filter << ")\n";

    double lat_range = lat_max - lat_min;
    double lon_range = lon_max - lon_min;
    int total_pixels = img_w * img_h; 
    int processed = 0;

    for (int py = 0; py < img_h && running_; ++py) {
        for (int px = 0; px < img_w; ++px) {
            double lat = lat_max - (py / (double)img_h) * lat_range;
            double lon = lon_min + (px / (double)img_w) * lon_range;
            
            double val = interpolateIDW(lat, lon, candidates, config);
            
            int idx = (py * img_w + px) * 4;
            if (!std::isnan(val)) {
                Color c = valueToColor(val, config.criterion);
                image[idx+0] = c.r; image[idx+1] = c.g; image[idx+2] = c.b;
                image[idx+3] = static_cast<uint8_t>(c.a * config.alpha);
            }
            processed++;
        }
        progress_ = static_cast<float>(processed) / total_pixels;
    }

    if(!running_) { std::cout << "[Heatmap] Cancelled\n"; return false; }
    
    try { std::filesystem::create_directories(std::filesystem::path(output_path).parent_path()); } catch(...) {}
    
    bool ok = savePNG(output_path, img_w, img_h, image);
    progress_ = 1.0f; running_ = false;
    return ok;
}