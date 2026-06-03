#pragma once
#include <libpq-fe.h>
#include <vector>
#include <string>
struct MapPoint {
    double lat;
    double lon;
    double rsrp;
    double rsrq;
    double rssi;
    double altitude;
    int earfcn;
    int pci;
    std::string timestamp;
    std::string cell_type;
};
 
PGconn* ConnectToDatabase();
void DisconnectFromDatabase(PGconn* conn);
bool SaveDataToDB(const char* req_data[], PGconn*& con);
std::vector<MapPoint> LoadMapDataFromDB(PGconn* con);
 