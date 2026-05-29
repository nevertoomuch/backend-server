#pragma once
#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <filesystem>
#include "database.h" 

enum class HeatmapCriterion { RSRP, RSRQ, RSSI, Altitude };

struct HeatmapConfig {
    HeatmapCriterion criterion = HeatmapCriterion::RSRP;
    int earfcn_filter = -1;
    int pci_filter = -1; 
    float min_radius_m = 10.0f;
    float max_radius_m = 40.0f;
    double power = 2.0;
    int img_width = 1024;       
    int img_height = 1024;
    bool per_tile = true;
    float alpha = 0.75f;        
    std::function<void(float)> progress_callback;
};

class IDWHeatmapGenerator {
public:
    IDWHeatmapGenerator();
    
    bool generate(const std::vector<MapPoint>& points,
                  const HeatmapConfig& config,
                  double lat_min, double lon_min, 
                  double lat_max, double lon_max,
                  const std::string& output_path);
    
    void cancel();
    bool isRunning() const { return running_; }
    float getProgress() const { return progress_; }

private:
    double haversineDistance(double lat1, double lon1, double lat2, double lon2);
    double fastDistanceMeters(double lat1, double lon1, double lat2, double lon2);
    double getCriterionValue(const MapPoint& p, HeatmapCriterion crit);
    double interpolateIDW(double lat, double lon,
                         const std::vector<MapPoint>& neighbors,
                         const HeatmapConfig& config);
    
    struct Color { uint8_t r, g, b, a; };
    Color valueToColor(double value, HeatmapCriterion crit);
    
    std::vector<MapPoint> filterPoints(const std::vector<MapPoint>& all,
                                       double center_lat, double center_lon,
                                       double max_radius_m, int earfcn);
    
    bool savePNG(const std::string& path, int w, int h, 
                 const std::vector<uint8_t>& rgba);
    
    std::atomic<bool> running_{false};
    std::atomic<float> progress_{0.0f};
    std::mutex cancel_mutex_;
};