#include "gui.h"
#include "data_structures.h"
#include "database.h"
#include "tile_cache.h"
#include "mercator.h"
#include "idw_heatmap.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"

#include <iostream>
#include <fstream>
#include <mutex>
#include <algorithm>
#include <thread>
#include <chrono>
#include <libpq-fe.h>
#include <ctime>
#include <atomic>
#include <filesystem>
#include <set>

#include "stb_image.h"

extern bool running;
extern bool start_server;
extern int zoom;
extern std::mutex g_CacheMutex;
extern std::mutex g_JobMutex;

static double viewLat = 55.01330949007172;
static double viewLon = 82.95111885647708;
static bool isDragging = false;
static ImVec2 dragStartPos;

static IDWHeatmapGenerator g_heatmap_gen;
static HeatmapConfig g_heatmap_config;
static bool g_show_heatmap = false;
static std::thread g_heatmap_thread;
static std::atomic<bool> g_heatmap_ready{false};

static GLuint g_heatmap_texture_id = 0;
static bool g_heatmap_visible = false;
static bool g_heatmap_load_failed = false;
static double g_hm_lat_min, g_hm_lat_max, g_hm_lon_min, g_hm_lon_max;
static double gen_min_lat, gen_max_lat, gen_min_lon, gen_max_lon;

static std::vector<int> unique_pcis;
static int selected_pci = -1;

GLuint LoadHeatmapTexture(const std::string& filename, 
                          double lat_min, double lon_min, 
                          double lat_max, double lon_max) {
    
    std::cout << "[Heatmap] Trying to load: " << filename << std::endl;
    if (!std::filesystem::exists(filename)) {
        std::cerr << "[Heatmap] File does not exist!\n";
        return 0;
    }
    int w, h, ch;
    unsigned char* data = stbi_load(filename.c_str(), &w, &h, &ch, 4);
    if (!data) { std::cerr << "[Heatmap] stbi_load failed\n"; return 0; }
    GLuint texID;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);
    g_hm_lat_min = lat_min; g_hm_lat_max = lat_max;
    g_hm_lon_min = lon_min; g_hm_lon_max = lon_max;
    return texID;
}

void DrawHeatmapLegend(HeatmapCriterion crit) {
    ImGui::Text("Legend:");
    float bar_w = 500.0f; 
    float bar_h = 20.0f; 
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    
    for (int i = 0; i < 20; i++) {
        ImVec2 p1 = ImVec2(p.x + i * (bar_w/20), p.y);
        ImVec2 p2 = ImVec2(p.x + (i+1) * (bar_w/20), p.y + bar_h);
        ImVec4 col;
        float t = (float)i / 19.0f;
        
        if (crit == HeatmapCriterion::RSRP) col = ImVec4(t, 0.0f, 1.0f - t, 1.0f); 
        else if (crit == HeatmapCriterion::RSRQ) col = ImVec4(1.0f - t, t, 0.0f, 1.0f);
        else if (crit == HeatmapCriterion::RSSI) col = ImVec4(t, t, 0.0f, 1.0f); 
        else if (crit == HeatmapCriterion::Altitude) col = ImVec4(t, 0.0f, 1.0f - t, 1.0f);
        else col = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
        
        draw->AddRectFilled(p1, p2, ImGui::ColorConvertFloat4ToU32(col));
    }
    draw->AddRect(p, ImVec2(p.x + bar_w, p.y + bar_h), IM_COL32(255,255,255,255));
    ImGui::Dummy(ImVec2(0, bar_h + 5));
    
    ImGui::SameLine();
    if (crit == HeatmapCriterion::RSRP) ImGui::Text("-110 (Bad) ........................................................... -80 (Good) dBm");
    else if (crit == HeatmapCriterion::RSRQ) ImGui::Text("-20 (Bad) ............................................................ -10 (Good) dB");
    else if (crit == HeatmapCriterion::RSSI) ImGui::Text("-110 (Weak) .......................................................... -70 (Strong) dBm");
    else if (crit == HeatmapCriterion::Altitude) ImGui::Text("0m (Low) ............................................................. 300m (High)");
}

void ColoredIndicator(const char* label, bool condition,
                     const char* true_text, const char* false_text) {
    ImGui::Text("%s: ", label); ImGui::SameLine();
    if (condition) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0,255,0,255));
        ImGui::Text("* %s", true_text); ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255,0,0,255));
        ImGui::Text("* %s", false_text); ImGui::PopStyleColor();
    }
}

void LoadDataFromDB(PGconn* con) {
    std::lock_guard<std::mutex> lock(mtx);
    offline_store.clear();
    unique_pcis.clear();
    selected_pci = -1;
    
    std::vector<MapPoint> points = LoadMapDataFromDB(con);
    if (points.empty()) { std::cout << "[GUI] No points found\n"; return; }
    
    std::set<int> pci_set;
    for (const auto& p : points) {
        if (p.pci > 0) pci_set.insert(p.pci);
    }
    for (int p : pci_set) unique_pcis.push_back(p);

    for (size_t i = 0; i < points.size(); ++i) {
        offline_store.lats.push_back(points[i].lat);
        offline_store.lons.push_back(points[i].lon);
        offline_store.rsrps.push_back(points[i].rsrp);
        offline_store.rsrqs.push_back(points[i].rsrq);
        offline_store.rssis.push_back(points[i].rssi);
        offline_store.altitudes.push_back(points[i].altitude);
        offline_store.earfcns.push_back(points[i].earfcn);
        offline_store.pcis.push_back(points[i].pci);
        offline_store.times.push_back(i);
        offline_store.indices.push_back(static_cast<double>(i));
    }
    offline_store.loaded = true;
    std::cout << "[GUI] Loaded " << offline_store.lats.size() << " points\n";
}

void ImportJsonToDB(PGconn* con, const std::string& filepath) {
    if (!con) { 
        std::cerr << "[Import] No database connection!\n"; 
        return; 
    }
    
    std::ifstream file(filepath);
    if (!file.is_open()) { 
        std::cerr << "[Import] Cannot open file: " << filepath << "\n"; 
        return; 
    }
    
    int inserted = 0, skipped = 0;
    std::string line;
    std::cout << "[Import] Starting import...\n";

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            
            double lat = j.value("lat", 0.0);
            double lon = j.value("lon", 0.0);
            double alt = j.value("alt", 0.0);
            double acc = j.value("acc", 0.0);
            long timestamp = j.value("time", 0);
            
            std::string cell_type = "N/A";
            double rsrp = -145.0;
            double rssi = -120.0;
            double rsrq = -20.0;
            int earfcn = -1;
            int pci = -1;
            
            if (j.contains("cell_data") && j["cell_data"].contains("cells")) {
                auto& cells = j["cell_data"]["cells"];
                if (!cells.empty()) {
                    auto& cell = cells[0];
                    std::string raw_type = cell.value("type", "N/A");
                    cell_type.clear();
                    for (char c : raw_type) {
                        if (c >= 32 && c < 127) cell_type += c;
                    }
                    
                    if (cell.contains("identity")) {
                        pci = cell["identity"].value("pci", -1);
                        earfcn = cell["identity"].value("earfcn", -1);
                        if (pci == 2147483647) pci = -1;
                    }
                    if (pci == -1 && cell.contains("pci")) {
                        pci = cell["pci"];
                        if (pci == 2147483647) pci = -1;
                    }

                    if (cell.contains("signal")) {
                        rsrp = cell["signal"].value("rsrp", -145.0);
                        rsrq = cell["signal"].value("rsrq", -20.0);
                        rssi = cell["signal"].value("rssi", -120.0);
                    } else {
                        rsrp = cell.value("rsrp", -145.0);
                        rsrq = cell.value("rsrq", -20.0);
                        rssi = cell.value("rssi", -120.0);
                    }
                }
            }
            
            std::string safe_cell_type;
            for (char c : cell_type) {
                if (c == '\'') safe_cell_type += "''";
                else if (c >= 32 && c < 127) safe_cell_type += c;
            }
            if (safe_cell_type.empty()) safe_cell_type = "N/A";
            
            std::string query = 
                "INSERT INTO telemetry_data "
                "(lat, lon, alt, accuracy, cell_type, rsrp, rssi, rsrq, earfcn, pci, timestamp) "
                "VALUES (" +
                std::to_string(lat) + ", " +
                std::to_string(lon) + ", " +
                std::to_string(alt) + ", " +
                std::to_string(acc) + ", '" +
                safe_cell_type + "', " +
                std::to_string(rsrp) + ", " +
                std::to_string(rssi) + ", " +
                std::to_string(rsrq) + ", " +
                std::to_string(earfcn) + ", " +
                std::to_string(pci) + ", " +
                std::to_string(timestamp) +
                ") ON CONFLICT (timestamp) DO NOTHING";
            
            PGresult* res = PQexec(con, query.c_str());
            
            if (PQresultStatus(res) == PGRES_COMMAND_OK) {
                inserted++;
            } else { 
                skipped++; 
            }
            PQclear(res);
            
        } catch (const std::exception& e) {
            skipped++;
        }
    }
    file.close();
    std::cout << "[Import] Inserted: " << inserted << ",Skipped: " << skipped << "\n";
}

void RunGUI() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return;
    SDL_Window* window = SDL_CreateWindow("Network Analyzer", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, 1100, 700,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    glewInit();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    PGconn* db_con = ConnectToDatabase();
    static char jsonFilePath[512] = "/mnt/d/backend-android/telemetry.jsonl";

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
        ImGui::End();

        ImGui::Begin("Control");
        ColoredIndicator("Server", start_server, "RUNNING", "STOPPED");
        if (ImGui::Button("Restart", ImVec2(100,0))) {
            start_server = false; std::this_thread::sleep_for(std::chrono::milliseconds(200)); start_server = true;
        }
        ImGui::End();

        ImGui::Begin("OpenStreetMap");
        if (ImGui::Button("-", ImVec2(30, 0))) zoom = std::max(zoom - 1, 1);
        ImGui::SameLine(); if (ImGui::Button("+", ImVec2(30, 0))) zoom = std::min(zoom + 1, 19);
        ImGui::SameLine(); ImGui::Text("Zoom: %d", zoom);
        ImGui::SameLine(); if (ImGui::Button("Reset View")) { viewLat = 55.0133; viewLon = 82.9511; zoom = 16; }
        ImGui::Separator();
        if (ImGui::Button("Load from Database", ImVec2(-1, 30))) { if (db_con) LoadDataFromDB(db_con); }
        ImGui::Separator();
        ImGui::Text("Import JSON:");
        ImGui::InputText("##jsonpath", jsonFilePath, IM_ARRAYSIZE(jsonFilePath));
        ImGui::SameLine();
        if (ImGui::Button("Import")) { if (db_con) ImportJsonToDB(db_con, jsonFilePath); }
        ImGui::Separator();
        if (offline_store.loaded) ImGui::TextColored(ImVec4(0,1,0,1), "Points: %d", (int)offline_store.lats.size());

        ImGui::Separator();
        ImGui::Checkbox("Show Heatmap", &g_show_heatmap);

        if (g_show_heatmap) {
            ImGui::Indent();
            static int crit_idx = 0;
            const char* crit_names[] = { "RSRP", "RSRQ", "RSSI", "Altitude" };
            if (ImGui::Combo("Criterion", &crit_idx, crit_names, IM_ARRAYSIZE(crit_names)))
                g_heatmap_config.criterion = static_cast<HeatmapCriterion>(crit_idx);
            
            if (!unique_pcis.empty()) {
                static std::vector<std::string> pci_labels;
                pci_labels.clear();
                pci_labels.push_back("All");
                for (int p : unique_pcis) pci_labels.push_back(std::to_string(p));
                
                static std::vector<const char*> pci_ptrs;
                pci_ptrs.clear();
                for (const auto& s : pci_labels) pci_ptrs.push_back(s.c_str());
                
                int current_idx = (selected_pci == -1) ? 0 : (int)(std::find(unique_pcis.begin(), unique_pcis.end(), selected_pci) - unique_pcis.begin()) + 1;
                
                if (ImGui::Combo("Filter by PCI", &current_idx, pci_ptrs.data(), (int)pci_ptrs.size())) {
                    selected_pci = (current_idx == 0) ? -1 : unique_pcis[current_idx - 1];
                }
            }

            ImGui::InputInt("EARFCN (-1=All)", &g_heatmap_config.earfcn_filter);
            ImGui::SliderFloat("Min Radius (m)", &g_heatmap_config.min_radius_m, 0.0f, 500.0f, "%.0f m");
            ImGui::SliderFloat("Max Radius (m)", &g_heatmap_config.max_radius_m, 100.0f, 3000.0f, "%.0f m");
            if (g_heatmap_config.min_radius_m > g_heatmap_config.max_radius_m) g_heatmap_config.min_radius_m = g_heatmap_config.max_radius_m;
            ImGui::SliderFloat("Opacity", &g_heatmap_config.alpha, 0.1f, 1.0f, "%.2f");
            ImGui::Unindent();

            ImGui::Separator();
            DrawHeatmapLegend(g_heatmap_config.criterion);
            ImGui::Separator();
            
            if (!g_heatmap_gen.isRunning()) {
                ImGui::BeginDisabled(g_heatmap_ready && !g_heatmap_visible && !g_heatmap_load_failed);
                if (ImGui::Button("Generate Heatmap", ImVec2(-1, 30))) {
                    if (offline_store.loaded && !offline_store.lats.empty()) {
                        std::cout << "[Heatmap] Calculating bounds...\n";
                        double min_lat = 1e9, max_lat = -1e9, min_lon = 1e9, max_lon = -1e9;
                        for (double lat : offline_store.lats) { min_lat = std::min(min_lat, lat); max_lat = std::max(max_lat, lat); }
                        for (double lon : offline_store.lons) { min_lon = std::min(min_lon, lon); max_lon = std::max(max_lon, lon); }
                        double pad_lat = (max_lat - min_lat) * 0.2, pad_lon = (max_lon - min_lon) * 0.2;
                        min_lat -= pad_lat; max_lat += pad_lat; min_lon -= pad_lon; max_lon += pad_lon;
                        gen_min_lat = min_lat; gen_max_lat = max_lat; gen_min_lon = min_lon; gen_max_lon = max_lon;
                        double ratio = (max_lon - min_lon) / (max_lat - min_lat);
                        g_heatmap_config.img_width  = (ratio >= 1.0) ? 1024 : (int)(1024 * ratio);
                        g_heatmap_config.img_height = (ratio <  1.0) ? 1024 : (int)(1024 / ratio);
                        
                        std::vector<MapPoint> points;
                        points.reserve(offline_store.lats.size());
                        for (size_t i = 0; i < offline_store.lats.size(); ++i) {
                            MapPoint p;
                            p.lat = offline_store.lats[i]; p.lon = offline_store.lons[i];
                            p.rsrp = offline_store.rsrps[i]; p.rsrq = offline_store.rsrqs[i];
                            p.rssi = offline_store.rssis[i]; p.altitude = offline_store.altitudes[i];
                            p.earfcn = offline_store.earfcns[i]; p.pci = offline_store.pcis[i];
                            points.push_back(p);
                        }
                        g_heatmap_ready = false; g_heatmap_visible = false; g_heatmap_load_failed = false; g_heatmap_texture_id = 0;
                        g_heatmap_thread = std::thread([points, min_lat, min_lon, max_lat, max_lon, pci=selected_pci]() {
                            HeatmapConfig cfg = g_heatmap_config;
                            cfg.pci_filter = pci;
                            bool ok = g_heatmap_gen.generate(points, cfg, min_lat, min_lon, max_lat, max_lon, "/mnt/d/backend-android/build/heatmap/full_map.png");
                            g_heatmap_ready = true;
                        });
                        g_heatmap_thread.detach();
                    }
                }
                ImGui::EndDisabled();
            } else {
                ImGui::ProgressBar(g_heatmap_gen.getProgress(), ImVec2(-1, 20));
                ImGui::SameLine(); if (ImGui::Button("Cancel")) g_heatmap_gen.cancel();
            }
            if (g_heatmap_ready && !g_heatmap_visible && !g_heatmap_load_failed) {
                ImGui::TextColored(ImVec4(0,1,0,1), "Generated!");
                g_heatmap_texture_id = LoadHeatmapTexture("/mnt/d/backend-android/build/heatmap/full_map.png", gen_min_lat, gen_min_lon, gen_max_lat, gen_max_lon);
                if (g_heatmap_texture_id != 0) g_heatmap_visible = true;
                else { g_heatmap_load_failed = true; ImGui::TextColored(ImVec4(1,0,0,1), "Texture load failed"); }
            }
            if (g_heatmap_load_failed) {
                ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Load failed");
                if (ImGui::Button("Retry")) { g_heatmap_load_failed = false; g_heatmap_ready = true; }
            }
        }

        if (ImPlot::BeginPlot("##Map", ImVec2(-1, -1), ImPlotFlags_NoFrame)) {
            ImPlot::SetupAxes("Longitude", "Latitude");
            if (zoom < 1) zoom = 16;
            double span = 0.005 * std::pow(2.0, 16 - zoom);
            ImPlot::SetupAxisLimits(ImAxis_X1, viewLon - span, viewLon + span, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, viewLat - span, viewLat + span, ImGuiCond_Always);
            if (ImPlot::IsPlotHovered()) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    if (!isDragging) { dragStartPos = ImGui::GetMousePos(); isDragging = true; } 
                    else {
                        ImVec2 delta = ImVec2(ImGui::GetMousePos().x - dragStartPos.x, ImGui::GetMousePos().y - dragStartPos.y);
                        ImVec2 plotSize = ImPlot::GetPlotSize();
                        viewLon -= delta.x * (span * 2) / plotSize.x;
                        viewLat += delta.y * (span * 2) / plotSize.y;
                        dragStartPos = ImGui::GetMousePos();
                    }
                } else isDragging = false;
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0) zoom = std::clamp(zoom + (int)wheel, 1, 19);
            }
            double mercator_left = viewLon - span, mercator_right = viewLon + span;
            double mercator_bottom = LatToMercatorY(viewLat - span), mercator_top = LatToMercatorY(viewLat + span);
            int minX = (int)floor(MercatorXToTileX(mercator_left, zoom)), maxX = (int)floor(MercatorXToTileX(mercator_right, zoom));
            int minY = (int)floor(MercatorYToTileY(mercator_top, zoom)), maxY = (int)floor(MercatorYToTileY(mercator_bottom, zoom));
            int maxTileCount = (1 << zoom) - 1;
            minX = std::max(0, std::min(minX, maxTileCount)); maxX = std::max(0, std::min(maxX, maxTileCount));
            minY = std::max(0, std::min(minY, maxTileCount)); maxY = std::max(0, std::min(maxY, maxTileCount));
            for (int x = minX; x <= maxX; ++x) for (int y = minY; y <= maxY; ++y) {
                std::string tileId = std::to_string(zoom) + "/" + std::to_string(x) + "/" + std::to_string(y);
                bool needLoad = false;
                { std::lock_guard<std::mutex> lock(g_CacheMutex); if (g_TileCache.find(tileId) == g_TileCache.end()) { TextureData newTex; newTex.isLoading = true; g_TileCache[tileId] = newTex; needLoad = true; } }
                if (needLoad) { std::lock_guard<std::mutex> lock(g_JobMutex); TileJob job; job.id = tileId; job.zoom = zoom; job.x = x; job.y = y; g_JobQueue.push(job); }
            }
            for (int x = minX; x <= maxX; ++x) for (int y = minY; y <= maxY; ++y) {
                std::string tileId = std::to_string(zoom) + "/" + std::to_string(x) + "/" + std::to_string(y);
                GLuint gpuId = 0;
                { std::lock_guard<std::mutex> lock(g_CacheMutex); auto it = g_TileCache.find(tileId); if (it != g_TileCache.end()) { auto& tex = it->second; if (!tex.rgbaBlob.empty() && tex.id == 0) { glGenTextures(1, &tex.id); glBindTexture(GL_TEXTURE_2D, tex.id); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.width, tex.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex.rgbaBlob.data()); tex.rgbaBlob.clear(); } gpuId = tex.id; } }
                if (gpuId != 0) {
                    double left = TileXToMercatorX(x, zoom), right = TileXToMercatorX(x+1, zoom);
                    double top_merc = TileYToMercatorY(y, zoom), bottom_merc = TileYToMercatorY(y+1, zoom);
                    double top_lat = MercatorYToLat(top_merc), bottom_lat = MercatorYToLat(bottom_merc);
                    ImPlot::PlotImage(("##tile_" + tileId).c_str(), (ImTextureID)(intptr_t)gpuId, ImPlotPoint{left, bottom_lat}, ImPlotPoint{right, top_lat});
                }
            }
            if (g_heatmap_visible && g_heatmap_texture_id != 0) {
                ImPlot::PlotImage("##heatmap_overlay", (ImTextureID)(intptr_t)g_heatmap_texture_id, ImPlotPoint{g_hm_lon_min, g_hm_lat_min}, ImPlotPoint{g_hm_lon_max, g_hm_lat_max});
            }
            if (!g_show_heatmap && offline_store.loaded && !offline_store.lats.empty()) {
                ImPlot::PushStyleColor(ImPlotProp_MarkerSize, ImVec4(1, 0, 0, 1));
                ImPlot::PlotScatter("route", offline_store.lons.data(), offline_store.lats.data(), (int)offline_store.lats.size());
                ImPlot::PopStyleColor();
            }
            ImPlot::PushStyleColor(ImPlotProp_MarkerSize, ImVec4(1, 0, 0, 1));
            ImPlot::PlotScatter("##center", &viewLon, &viewLat, 1);
            ImPlot::PopStyleColor();
            ImPlot::EndPlot();
        }
        ImGui::Text("Center: %.6f, %.6f | Zoom: %d", viewLat, viewLon, zoom);
        ImGui::End();

        ImGui::Render();
        int w, h; SDL_GL_GetDrawableSize(window, &w, &h);
        glViewport(0, 0, w, h); glClearColor(0.1f, 0.1f, 0.12f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (db_con) DisconnectFromDatabase(db_con);
    ImPlot::DestroyContext(); ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext(); SDL_Quit();
}