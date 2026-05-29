#include "server.h"
#include "data_structures.h"
#include "database.h"
#include <zmq.hpp>
#include <thread>
#include <mutex>
#include <iostream>
#include <string>
#include <algorithm>

bool running = true;
bool start_server = true;

void RunServer(PGconn* db_con) {
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.set(zmq::sockopt::rcvtimeo, 1000);

    try {
        socket.bind("tcp://*:7777");
        std::cout << "[ZMQ Server] Started on port 7777. Waiting..." << std::endl;
    } catch (const zmq::error_t& e) {
        std::cerr << "[ZMQ Error] Bind failed: " << e.what() << std::endl;
        return;
    }

    while (running) {
        if (!start_server) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        zmq::message_t request;
        zmq::recv_result_t result = socket.recv(request, zmq::recv_flags::none);
        
        if (result) {
            std::string msg_str(static_cast<char*>(request.data()), request.size());
            try {
                auto j = json::parse(msg_str);
                std::lock_guard<std::mutex> lock(mtx);
                
                data_store.lat = std::to_string(j.value("lat", 0.0));
                data_store.lon = std::to_string(j.value("lon", 0.0));
                data_store.alt = std::to_string(j.value("alt", 0.0));
                data_store.acc = std::to_string(j.value("acc", 0.0));
                
                if (j.contains("cell_data") && j["cell_data"].contains("cells")) {
                    auto& cells = j["cell_data"]["cells"];
                    data_store.history.add_points(cells);
                    
                    for (auto& cell : cells) {
                        if (cell.value("registered", false) || cell.value("pci", -1) != -1) {
                            data_store.type = cell.value("type", "N/A");
                            float rsrp_val = -145.0f;
                            
                            if (data_store.type == "LTE")
                                rsrp_val = cell["signal"].value("rsrp", -145.0f);
                            else if (data_store.type == "NR")
                                rsrp_val = cell["signal"].value("ssRsrp", -145.0f);
                            else if (data_store.type == "GSM")
                                rsrp_val = cell["signal"].value("dbm", -145.0f);
                            
                            data_store.current_rsrp = rsrp_val;
                            
                            std::string s_rsrp = std::to_string(rsrp_val);
                            const char* params[6] = {
                                data_store.lat.c_str(), data_store.lon.c_str(),
                                data_store.alt.c_str(), data_store.acc.c_str(),
                                data_store.type.c_str(), s_rsrp.c_str()
                            };
                            SaveDataToDB(params, db_con);
                            
                            log_messages.push_back("Recv: " + data_store.type + " | RSRP: " + s_rsrp);
                            if (log_messages.size() > 50) log_messages.erase(log_messages.begin());
                            
                            break;
                        }
                    }
                }
                
                session_data_counter++;
                
            } catch (const std::exception& e) {
                std::cerr << "[Data Error] Failed to process JSON: " << e.what() << std::endl;
            }
            socket.send(zmq::buffer(std::string("OK")), zmq::send_flags::none);
        }
    }
    
    std::cout << "[ZMQ Server] Thread stopped." << std::endl;
}