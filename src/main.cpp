#include "data_structures.h"
#include "database.h"
#include "server.h"
#include "gui.h"
#include "tiles.h"
#include <thread>
#include <iostream>
#include <csignal>
#include <chrono>

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        running = false;
        std::cout << "\n[Signal]  " << signum
                  << ", shutting down..." << std::endl;
    }
}

static bool init_signals() {
    if (std::signal(SIGINT,  signal_handler) == SIG_ERR) return false;
    if (std::signal(SIGTERM, signal_handler) == SIG_ERR) return false;
    return true;
}

int main() {
    std::cout << "\n=== Starting ===" << std::endl;

    if (!init_signals()) {
        std::cerr << "[Main] Failed." << std::endl;
        return 1;
    }

    PGconn* con = ConnectToDatabase();
    if (!con) {
        std::cerr << "[Main] Database connection failed. Exiting." << std::endl;
        return 1;
    }

    std::thread tile_thread(FetchWorker);
    tile_thread.detach();

    std::thread server_thread(RunServer, con);

    RunGUI();

    running = false;

    server_thread.join();

    DisconnectFromDatabase(con);

    std::cout << "[Main] Session packets: " << session_data_counter << std::endl;
    std::cout << "=== STOP ===" << std::endl;
    return 0;
}