#include "mercator.h"

int zoom = 12;

double XToTileX(double mercatorX, int zoom) {
    return (0.5 + mercatorX / 360.0) * (1 << zoom);
}

double YToTileY(double mercatorY, int zoom) {
    return (0.5 - mercatorY / 360.0) * (1 << zoom);
}

double TileXToX(int tileX, int zoom) {
    return (tileX / static_cast<double>(1 << zoom) - 0.5) * 360.0;
}

double TileYToY(int tileY, int zoom) {
    return (0.5 - tileY / static_cast<double>(1 << zoom)) * 360.0;
}

double LatToY(double lat) {
    lat = std::max(-85.0511, std::min(85.0511, lat));
    double lat_rad = lat * M_PI / 180.0;
    double mercator_y = std::log(std::tan(M_PI / 4 + lat_rad / 2));
    return mercator_y * 180.0 / M_PI;
}

double YToLat(double mercator_y) {
    double mercator_y_rad = mercator_y * M_PI / 180.0;
    double lat_rad = std::atan(std::sinh(mercator_y_rad));
    return lat_rad * 180.0 / M_PI;
}

int CalculateZoom(double lonMin, double lonMax) {
    double diff = lonMax - lonMin;
    if (diff <= 0 || diff >= 360.0) return 1;
    int z = (int)std::floor(std::log2(360.0 / diff)) + 2;
    return std::max(0, std::min(z, 19));
}