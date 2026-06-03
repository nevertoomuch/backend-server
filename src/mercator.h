#pragma once
#include <cmath>
#include <algorithm>

double XToTileX(double mercatorX, int zoom);
double YToTileY(double mercatorY, int zoom);
double TileXToX(int tileX, int zoom);
double TileYToY(int tileY, int zoom);
double LatToY(double lat);
double YToLat(double mercator_y);
int CalculateZoom(double lonMin, double lonMax);