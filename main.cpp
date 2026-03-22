#include <zmq.hpp>
#include <string>
#include <iostream>
#include <thread>
#include <fstream>
#include <mutex>
#include <vector>

#include <GL/glew.h>
#include <SDL2/SDL.h>

#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"

std::mutex mtx;
std::vector<std::string> log_messages;
bool running = true;

void zmq_server_thread() {
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);

    int timeout = 500;
    socket.set(zmq::sockopt::rcvtimeo, timeout);

    try {
        socket.bind("tcp://*:1337");
        std::cout << "Сервер на порту 1337..." << std::endl;
    } catch (const zmq::error_t& e) {
        std::cerr << "ZMQ error: " << e.what() << std::endl;
        return;
    }

    std::ofstream file("data.json", std::ios::app);

    while (running) {
        zmq::message_t request;
        auto res = socket.recv(request, zmq::recv_flags::none);

        if (res) {
            std::string message(static_cast<char*>(request.data()), request.size());
            file << message << "\n";
            file.flush();
            {
                std::lock_guard<std::mutex> lock(mtx);
                log_messages.push_back(message);
            }

            std::string reply_str = "Данные получены!";
            zmq::message_t reply(reply_str.size());
            memcpy(reply.data(), reply_str.data(), reply_str.size());
            socket.send(reply, zmq::send_flags::none);
        }
    }
}

int main(int argc, char* argv[]) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);
    SDL_Window* window = SDL_CreateWindow(
        "TelephoneActivity", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);

    glewInit();

    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    std::thread network_thread(zmq_server_thread);

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        {
            static int counter = 0;
            ImGui::Begin("MainStatus");
                ImGui::Text("Server start on port 1337");

                ImGui::Separator();
                ImGui::Text("Message ZEROMQ");
                
                std::lock_guard<std::mutex> lock(mtx);
                for (const auto& msg : log_messages) {
                    ImGui::TextWrapped("- %s", msg.c_str());
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                    ImGui::SetScrollHereY(1.0f);
                }
            ImGui::End();
        }

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.1f, 0.4f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }
    
    running = false;
    if (network_thread.joinable()) network_thread.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}