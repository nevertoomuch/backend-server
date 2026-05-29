#include "database.h"
#include <iostream>

#define HOST "localhost"
#define PORT "5432"
#define DB_NAME "network_monitor_db"
#define DB_USER "postgres"
#define DB_USER_PASSWORD "postgres1234"

PGconn* ConnectToDatabase() {
    const char* info = "host=" HOST " port=" PORT " dbname=" DB_NAME 
                      " user=" DB_USER " password=" DB_USER_PASSWORD;
    PGconn* con = PQconnectdb(info);
    if (PQstatus(con) != CONNECTION_OK) {
        std::cerr << "\033[31m[DB] Connection failed:\033[0m " << PQerrorMessage(con) << std::endl;
        PQfinish(con);
        return nullptr;
    }
    std::cout << "\033[32m[DB] Connected successfully!\033[0m" << std::endl;
    return con;
}

void DisconnectFromDatabase(PGconn* conn) {
    if (conn) PQfinish(conn);
}

	bool SaveDataToDB(const char *req_data[], PGconn *con)
	{
		const char *query = "INSERT INTO telemetry_data (lat, lon, alt, "
		"accuracy, cell_type, rsrp) VALUES ($1, $2, $3, $4, $5, $6)";

		PGresult *res = PQexecParams(con, query, 6, NULL, req_data, NULL, NULL, 0);
		
		bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
		PQclear(res); 
		return ok;
	}

std::vector<MapPoint> LoadMapDataFromDB(PGconn* con) {
    std::vector<MapPoint> points;
    if (!con) {
        std::cerr << "[DB] No connection" << std::endl;
        return points;
    }

    std::string query = "SELECT lat, lon, rsrp, rssi, alt, rsrq, earfcn, pci, timestamp, cell_type "
                        "FROM telemetry_data "
                        "WHERE lat IS NOT NULL AND lon IS NOT NULL "
                        "ORDER BY timestamp ASC";

    PGresult* res = PQexec(con, query.c_str());

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "[DB] Query error: " << PQresultErrorMessage(res) << std::endl;
        PQclear(res);
        return points;
    }

    int nrows = PQntuples(res);
    std::cout << "[DB] Found " << nrows << " records with coordinates" << std::endl;

    for (int i = 0; i < nrows; ++i) {
        if (PQgetisnull(res, i, 0) || PQgetisnull(res, i, 1)) continue;

        MapPoint point;
        
        point.lat = atof(PQgetvalue(res, i, 0));
        point.lon = atof(PQgetvalue(res, i, 1));
        
        point.rsrp = PQgetisnull(res, i, 2) ? -145.0 : atof(PQgetvalue(res, i, 2));
        point.rssi = PQgetisnull(res, i, 3) ? -120.0 : atof(PQgetvalue(res, i, 3));
        point.altitude = PQgetisnull(res, i, 4) ? 0.0    : atof(PQgetvalue(res, i, 4));
        point.rsrq = PQgetisnull(res, i, 5) ? -20.0   : atof(PQgetvalue(res, i, 5));
        
        point.earfcn = PQgetisnull(res, i, 6) ? -1     : atoi(PQgetvalue(res, i, 6));
        point.pci = PQgetisnull(res, i, 7) ? -1     : atoi(PQgetvalue(res, i, 7));
        
        point.timestamp = PQgetisnull(res, i, 8) ? ""     : PQgetvalue(res, i, 8);
        point.cell_type = PQgetisnull(res, i, 9) ? "N/A"  : PQgetvalue(res, i, 9);

        points.push_back(point);
    }

    PQclear(res);
    return points;
}