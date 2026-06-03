#include "database.h"
#include <iostream>

#define HOST     "localhost"
#define PORT     "5432"
#define DB_NAME  "network_monitor_db"
#define DB_USER  "postgres"
#define DB_PASS  "postgres1234"


PGconn* ConnectToDatabase() {
    const char* info = "host=" HOST " port=" PORT
                       " dbname=" DB_NAME
                       " user=" DB_USER
                       " password=" DB_PASS;

    PGconn* con = PQconnectdb(info);

    if (PQstatus(con) != CONNECTION_OK) {
        std::cerr << "[DB] Connection failed: " << PQerrorMessage(con) << std::endl;
        PQfinish(con);
        return nullptr;
    }

    std::cout << "[DB] Connected successfully!" << std::endl;
    return con;
}


void DisconnectFromDatabase(PGconn* conn) {
    if (conn) PQfinish(conn);
}


static bool reconnect(PGconn*& con) {
    std::cerr << "[DB] Connection lost, reconnecting..." << std::endl;
    PQfinish(con);
    con = ConnectToDatabase();
    if (!con) {
        std::cerr << "[DB] Reconnect failed." << std::endl;
        return false;
    }
    std::cout << "[DB] Reconnected." << std::endl;
    return true;
}


bool SaveDataToDB(const char* req_data[], PGconn*& con) {
    if (PQstatus(con) != CONNECTION_OK) {
        if (!reconnect(con)) return false;
    }

    const char* query =
        "INSERT INTO telemetry_data (lat, lon, alt, accuracy, cell_type, rsrp) "
        "VALUES ($1, $2, $3, $4, $5, $6)";

    PGresult* res = PQexecParams(con, query, 6, NULL, req_data, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::cerr << "[DB] Insert failed: " << PQresultErrorMessage(res) << std::endl;
        PQclear(res);
        return false;
    }

    PQclear(res);
    return true;
}


std::vector<MapPoint> LoadMapDataFromDB(PGconn* con) {
    std::vector<MapPoint> points;

    if (!con) {
        std::cerr << "[DB] No connection." << std::endl;
        return points;
    }

    const char* query =
        "SELECT lat, lon, rsrp, rssi, alt, rsrq, earfcn, pci, timestamp, cell_type "
        "FROM telemetry_data "
        "WHERE lat IS NOT NULL AND lon IS NOT NULL "
        "ORDER BY timestamp ASC";

    PGresult* res = PQexec(con, query);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "[DB] Query error: " << PQresultErrorMessage(res) << std::endl;
        PQclear(res);
        return points;
    }

    int nrows = PQntuples(res);
    std::cout << "[DB] Loaded " << nrows << " records." << std::endl;
    points.reserve(nrows);

    for (int i = 0; i < nrows; ++i) {
        if (PQgetisnull(res, i, 0) || PQgetisnull(res, i, 1)) continue;

        MapPoint p;
        p.lat = atof(PQgetvalue(res, i, 0));
        p.lon = atof(PQgetvalue(res, i, 1));
        p.rsrp = PQgetisnull(res, i, 2) ? -145.0 : atof(PQgetvalue(res, i, 2));
        p.rssi = PQgetisnull(res, i, 3) ? -120.0 : atof(PQgetvalue(res, i, 3));
        p.altitude = PQgetisnull(res, i, 4) ? 0.0 : atof(PQgetvalue(res, i, 4));
        p.rsrq = PQgetisnull(res, i, 5) ? -20.0 : atof(PQgetvalue(res, i, 5));
        p.earfcn = PQgetisnull(res, i, 6) ? -1 : atoi(PQgetvalue(res, i, 6));
        p.pci = PQgetisnull(res, i, 7) ? -1 : atoi(PQgetvalue(res, i, 7));
        p.timestamp = PQgetisnull(res, i, 8) ? "" : PQgetvalue(res, i, 8);
        p.cell_type = PQgetisnull(res, i, 9) ?  "N/A" : PQgetvalue(res, i, 9);

        points.push_back(p);
    }

    PQclear(res);
    return points;
}