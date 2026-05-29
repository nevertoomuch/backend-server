#include "database.h"
#include "server.h"
#include "gui.h"
#include "tile_cache.h"
#include <thread>
#include <iostream>
#include <csignal>

void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        running = false;
        std::cout << "\n[Signal] Stopping..." << std::endl;
    }
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "\n=== START ===" << std::endl;

    PGconn* con = ConnectToDatabase();
    if (!con) return 1;

    std::thread(FetchWorker).detach();
    std::thread server(RunServer, con);

    RunGUI();

    running = false;
    server.join();
    DisconnectFromDatabase(con);

    std::cout << "=== CLOSED ===" << std::endl;
    return 0;
}

