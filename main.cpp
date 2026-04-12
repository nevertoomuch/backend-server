#include <zmq.hpp>
#include <string>
#include <iostream>
#include <thread>
#include <mutex>
#include <vector>
#include <map>
#include <atomic>
#include <csignal>
#include <chrono>
#include <nlohmann/json.hpp>
#include <algorithm>

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include <libpq-fe.h>


#define HOST "localhost"
#define PORT "5432"
#define DB_NAME "network_monitor_db"
#define DB_USER "postgres"
#define DB_USER_PASSWORD "postgres1234"

using json = nlohmann::json;


std::atomic<bool> running{true};
bool start_server = true;
int session_data_counter = 0;


void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        std::cout << "\n[Signal] Завершение работы..." << std::endl;
        running = false;
    }
}
struct SignalHistory {
    std::map<int, std::vector<float>> streams_y;
    std::vector<float> x;
    float current_step = 0;
    const int max_points = 200;

    void add_points(const json& cells_array) {
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
    
    void clear() { streams_y.clear(); x.clear(); current_step = 0; }
    bool empty() const { return x.empty(); }
};

struct Telemetry {
    std::string lat = "0", lon = "0", alt = "0", acc = "0", type = "N/A";
    float current_rsrp = -140.0f;
    SignalHistory history;
    std::map<int, std::string> pci_types;
} data_store;

std::mutex mtx;
std::vector<std::string> log_messages;


bool save_to_db(PGconn* con, const Telemetry& data, int64_t timestamp,
                int pci, const std::string& type, float rsrp, float rssi, float sinr,
                const std::string& mcc, const std::string& mnc) {
    if (!con) return false;
    
    const char* params[13];
    std::string p[13] = {
        "android_device",
        data.lat, data.lon, data.alt, data.acc,
        std::to_string(timestamp),
        type,
        std::to_string(pci),
        (rsrp > -200.0f) ? std::to_string(rsrp) : "",
        (rssi > -200.0f) ? std::to_string(rssi) : "",
        (sinr > -100.0f) ? std::to_string(sinr) : "",
        mcc,
        mnc
    };
    for(int i=0; i<13; ++i) params[i] = p[i].c_str();
    
    std::string query = "INSERT INTO telemetry_data "
        "(imei, lat, lon, alt, accuracy, timestamp, cell_type, pci, rsrp, rssi, sinr, mcc, mnc) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13)";
    
    PGresult* res = PQexecParams(con, query.c_str(), 13, nullptr, params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) std::cerr << "\033[31m[DB] Error:\033[0m " << PQresultErrorMessage(res) << std::endl;
    PQclear(res);
    return ok;
}

void run_server(PGconn* db_con) {
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.set(zmq::sockopt::rcvtimeo, 1000);

    try {
        socket.bind("tcp://*:7777");
        std::cout << "[ZMQ] Listening on port 7777..." << std::endl;
    } catch (const zmq::error_t& e) {
        std::cerr << "[ZMQ] Bind failed: " << e.what() << std::endl;
        return;
    }

    while (running) {
        if (!start_server) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        zmq::message_t request;
        auto result = socket.recv(request, zmq::recv_flags::none);

        if (result) {
            std::string msg(static_cast<char*>(request.data()), request.size());
            
            try {
                auto j = json::parse(msg);
                std::lock_guard<std::mutex> lock(mtx);
                
                data_store.lat = std::to_string(j.value("lat", 0.0));
                data_store.lon = std::to_string(j.value("lon", 0.0));
                data_store.alt = std::to_string(j.value("alt", 0.0));
                data_store.acc = std::to_string(j.value("acc", 0.0));
                
                int64_t timestamp = j.value("time", 
                    (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                
                if (j.contains("cell_data") && j["cell_data"].contains("cells")) {
                    auto& cells = j["cell_data"]["cells"];
                    data_store.history.add_points(cells);
                    
                    for (const auto& cell : cells) {
                        int pci = cell.value("pci", -1);
                        if (pci < 0) continue;
                        
                        std::string type = cell.value("type", "N/A");
                        data_store.pci_types[pci] = type;
                        
                        float rsrp = -145.0f, rssi = -145.0f, sinr = -20.0f;
                        std::string mcc = cell.value("mcc", "");
                        std::string mnc = cell.value("mnc", "");
                        
                        if (type == "LTE") {
                            rsrp = cell.value("rsrp", -145.0f);
                            rssi = cell.value("rssi", -145.0f);
                            sinr = cell.value("rssnr", -20.0f);  
                        } else if (type == "NR") {
                            rsrp = cell.value("ssRsrp", -145.0f);
                            rssi = cell.value("rssi", -145.0f);
                            sinr = cell.value("ssSinr", -20.0f);
                        } else if (type == "GSM") {
                            rsrp = cell.value("dbm", -145.0f);
                            rssi = cell.value("rssi", rsrp);
                        }
                        
                        if (db_con) save_to_db(db_con, data_store, timestamp, pci, type, rsrp, rssi, sinr, mcc, mnc);
                        
                        std::string log = "[" + type + " PCI:" + std::to_string(pci) + 
                                         "] RSRP:" + std::to_string((int)rsrp) + "dBm";
                        log_messages.push_back(log);
                        if (log_messages.size() > 50) log_messages.erase(log_messages.begin());
                        
                        data_store.current_rsrp = rsrp;
                        session_data_counter++;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[JSON] " << e.what() << std::endl;
            }
            
            socket.send(zmq::buffer(std::string("OK")), zmq::send_flags::none);
        }
    }
    std::cout << "[ZMQ] Server stopped." << std::endl;
}


void ColoredIndicator(const char* label, bool condition, 
                     const char* true_text = "ON", const char* false_text = "OFF") {
    ImGui::Text("%s: ", label); ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, condition ? IM_COL32(0,255,0,255) : IM_COL32(255,100,100,255));
    ImGui::Text("* %s", condition ? true_text : false_text);
    ImGui::PopStyleColor();
}

void run_gui() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return;
    
    SDL_Window* window = SDL_CreateWindow("Network Analyzer", 
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1200, 800,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    glewInit();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");
    ImGui::StyleColorsDark();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Current stats");
        ImGui::Text("Network: %s", data_store.type.c_str());
        ImGui::Text("RSRP: %.1f dBm", data_store.current_rsrp);
        ImGui::Separator();
        ImGui::Text("Lat: %s | Lon: %s", data_store.lat.c_str(), data_store.lon.c_str());
        ImGui::Text("Alt: %s m | Acc: %s m", data_store.alt.c_str(), data_store.acc.c_str());
        ImGui::Separator();
        ImGui::Text("Packets: %d", session_data_counter);
        
        ImGui::Separator();
        ImGui::Text("Active cells:");
        for (const auto& [pci, type] : data_store.pci_types) {
            if (data_store.history.streams_y.count(pci) && 
                !data_store.history.streams_y[pci].empty()) {
                float val = data_store.history.streams_y[pci].back();
                ImGui::Text("  PCI %3d [%s]: %.1f dBm", pci, type.c_str(), val);
            }
        }
        ImGui::End();

        ImGui::Begin("Control");
        ColoredIndicator("Server", start_server, "RUNNING", "STOPPED");
        if (ImGui::Button("Restart", ImVec2(100,0))) {
            start_server = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            start_server = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear", ImVec2(80,0))) {
            std::lock_guard<std::mutex> l(mtx);
            data_store.history.clear();
            session_data_counter = 0;
            log_messages.clear();
        }
        ImGui::End();

        ImGui::Begin("Signal Graph (RSRP)");
        if (ImPlot::BeginPlot("##RSRP", ImVec2(-1, 300))) {
            ImPlot::SetupAxes("Samples", "RSRP [dBm]");
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, 200, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -145, -40);
            ImPlot::SetupLegend(ImPlotLocation_NorthEast);
            
            std::lock_guard<std::mutex> lock(mtx);
            for (const auto& [pci, y_vec] : data_store.history.streams_y) {
                if (y_vec.empty()) continue;
                std::string label = "PCI " + std::to_string(pci);
                if (data_store.pci_types.count(pci)) label += " [" + data_store.pci_types[pci] + "]";
                ImPlot::PlotLine(label.c_str(), 
                                data_store.history.x.data(),
                                y_vec.data(),
                                static_cast<int>(y_vec.size()));
            }
            ImPlot::EndPlot();
        }
        ImGui::End();

        ImGui::Begin("Log");
        ImGui::BeginChild("##log", ImVec2(0, 100), true);
        {
            std::lock_guard<std::mutex> lock(mtx);
            for (const auto& msg : log_messages) ImGui::Text("%s", msg.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        int w, h;
        SDL_GL_GetDrawableSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ImPlot::DestroyContext();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_Quit();
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    std::cout << "\n=== START ===" << std::endl;
    
    const char* conn_info = "host=" HOST " port=" PORT " dbname=" DB_NAME 
                          " user=" DB_USER " password=" DB_USER_PASSWORD;
    PGconn* con = PQconnectdb(conn_info);
    
    if (PQstatus(con) != CONNECTION_OK) {
        std::cerr << "\033[31m[DB] Connection failed:\033[0m " << PQerrorMessage(con) << std::endl;
        PQfinish(con);
        con = nullptr;
    } else {
        std::cout << "\033[32m[DB] Connected to " << DB_NAME << "\033[0m" << std::endl;
    }
    
    std::thread srv_thread(run_server, con);
    run_gui();
    
    running = false;
    start_server = false;
    
    if (srv_thread.joinable()) srv_thread.join();
    if (con) { PQfinish(con); std::cout << "[DB] Disconnected." << std::endl; }
    
    std::cout << "=== Closed ===" << std::endl;
    return 0;
}