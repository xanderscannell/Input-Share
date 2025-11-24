#include "common.hpp"
#include "network.hpp"
#include "input_capture.hpp"
#include <iostream>
#include <chrono>
#include <atomic>
#include <thread>

using namespace MouseShare;

std::atomic<bool> g_running{true};

BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

class Server {
public:
    Server(uint16_t port, ScreenEdge switch_edge)
        : port_(port), switch_edge_(switch_edge), active_on_client_(false) {}
    
    bool run() {
        // Initialize input capture
        if (!input_.init()) {
            std::cerr << "Failed to initialize input capture\n";
            return false;
        }
        
        std::cout << "Screen size: " << input_.screen_width() << "x" 
                  << input_.screen_height() << "\n";
        
        // Set up input callbacks
        setup_callbacks();
        
        // Start capturing events
        input_.start();
        
        // Create server socket
        socket_.create();
        socket_.bind(port_);
        socket_.listen();
        
        std::cout << "Server listening on port " << port_ << "\n";
        std::cout << "Switch to client by moving mouse to the " 
                  << edge_name(switch_edge_) << " edge\n";
        std::cout << "Press Scroll Lock to toggle between computers\n";
        
        while (g_running) {
            std::cout << "Waiting for client connection...\n";
            
            try {
                client_socket_ = socket_.accept();
                connected_ = true;
                last_keepalive_sent_ = std::chrono::steady_clock::now();
                
                std::cout << "Client connected!\n";
                
                // Send our screen info
                send_screen_info();
                
                // Main loop while client is connected
                while (g_running && connected_) {
                    // Send keepalive every 5 seconds
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        now - last_keepalive_sent_
                    ).count();
                    
                    if (elapsed >= 5) {
                        auto data = serialize_keepalive();
                        if (client_socket_.send(data) <= 0) {
                            connected_ = false;
                            break;
                        }
                        last_keepalive_sent_ = now;
                    }
                    
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                
                // Clean disconnect handling
                if (active_on_client_) {
                    std::cout << "Client disconnected while active - releasing input\n";
                    switch_to_server();
                    
                    // Move cursor to center to prevent immediate retrigger
                    input_.warp_cursor(input_.screen_width() / 2, input_.screen_height() / 2);
                }
                
                client_socket_.close();
                std::cout << "Client disconnected\n";
                
            } catch (const NetworkError& e) {
                std::cerr << "Network error: " << e.what() << "\n";
            }
        }
        
        input_.stop();
        return true;
    }
    
private:
    void setup_callbacks() {
        // Set all callbacks
        input_.set_callbacks(
            // Move callback
            [this](int x, int y, int dx, int dy) {
                if (!connected_) return;
                
                // Check for edge switching
                bool at_edge = false;
                int edge_pos = 0;
                
                switch (switch_edge_) {
                    case ScreenEdge::LEFT:
                        at_edge = (x <= 0);
                        edge_pos = y;
                        break;
                    case ScreenEdge::RIGHT:
                        at_edge = (x >= input_.screen_width() - 1);
                        edge_pos = y;
                        break;
                    case ScreenEdge::TOP:
                        at_edge = (y <= 0);
                        edge_pos = x;
                        break;
                    case ScreenEdge::BOTTOM:
                        at_edge = (y >= input_.screen_height() - 1);
                        edge_pos = x;
                        break;
                    default:
                        break;
                }
                
                if (at_edge && !active_on_client_) {
                    // Switch to client
                    switch_to_client(edge_pos);
                } else if (active_on_client_) {
                    // Send relative movement to client
                    MouseMoveEvent event;
                    event.x = x;
                    event.y = y;
                    event.dx = dx;
                    event.dy = dy;
                    send_event(EventType::MOUSE_MOVE, event);
                }
            },
            // Button callback
            [this](MouseButton button, bool pressed) {
                if (!connected_ || !active_on_client_) return;
                
                MouseButtonEvent event;
                event.button = button;
                event.pressed = pressed;
                send_event(EventType::MOUSE_BUTTON, event);
            },
            // Scroll callback
            [this](int dx, int dy) {
                if (!connected_ || !active_on_client_) return;
                
                MouseScrollEvent event;
                event.dx = dx;
                event.dy = dy;
                send_event(EventType::MOUSE_SCROLL, event);
            },
            // Key callback
            [this](uint32_t vkCode, uint32_t scanCode, uint32_t flags, bool pressed) {
                // Check for Scroll Lock to toggle
                if (vkCode == VK_SCROLL && pressed) {
                    if (active_on_client_) {
                        switch_to_server();
                    } else if (connected_) {
                        switch_to_client(input_.screen_height() / 2);
                    }
                    return;
                }
                
                if (!connected_ || !active_on_client_) return;
                
                KeyEvent event;
                event.vkCode = vkCode;
                event.scanCode = scanCode;
                event.flags = flags;
                send_event(pressed ? EventType::KEY_PRESS : EventType::KEY_RELEASE, event);
            }
        );
    }
    
    void switch_to_client(int edge_position) {
        std::cout << "Switching to client\n";
        active_on_client_ = true;
        
        // Capture input (block local events)
        input_.capture_input(true);
        
        // Tell client to activate
        SwitchScreenEvent event;
        event.edge = opposite_edge(switch_edge_);
        event.position = edge_position;
        send_event(EventType::SWITCH_SCREEN, event);
        
        // Move cursor away from edge to center
        input_.warp_cursor(input_.screen_width() / 2, input_.screen_height() / 2);
    }
    
    void switch_to_server() {
        std::cout << "Switching to server\n";
        active_on_client_ = false;
        
        // Release input
        input_.capture_input(false);
    }
    
    template<typename T>
    void send_event(EventType type, const T& payload) {
        if (!connected_) return;
        
        auto data = serialize_packet(type, payload);
        int sent = client_socket_.send(data);
        
        if (sent <= 0) {
            connected_ = false;
            if (active_on_client_) {
                std::cout << "Send failed - switching back to server\n";
                switch_to_server();
            }
        }
    }
    
    void send_screen_info() {
        ScreenInfo info;
        info.width = input_.screen_width();
        info.height = input_.screen_height();
        info.x = 0;
        info.y = 0;
        send_event(EventType::SCREEN_INFO, info);
    }
    
    static ScreenEdge opposite_edge(ScreenEdge edge) {
        switch (edge) {
            case ScreenEdge::LEFT: return ScreenEdge::RIGHT;
            case ScreenEdge::RIGHT: return ScreenEdge::LEFT;
            case ScreenEdge::TOP: return ScreenEdge::BOTTOM;
            case ScreenEdge::BOTTOM: return ScreenEdge::TOP;
            default: return ScreenEdge::NONE;
        }
    }
    
    static const char* edge_name(ScreenEdge edge) {
        switch (edge) {
            case ScreenEdge::LEFT: return "left";
            case ScreenEdge::RIGHT: return "right";
            case ScreenEdge::TOP: return "top";
            case ScreenEdge::BOTTOM: return "bottom";
            default: return "none";
        }
    }
    
    uint16_t port_;
    ScreenEdge switch_edge_;
    
    InputCapture input_;
    Socket socket_;
    Socket client_socket_;
    
    std::atomic<bool> connected_{false};
    std::atomic<bool> active_on_client_{false};
    
    std::chrono::steady_clock::time_point last_keepalive_sent_;
};

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "Options:\n"
              << "  -p, --port PORT      Port to listen on (default: 24800)\n"
              << "  -e, --edge EDGE      Edge to switch screens (left/right/top/bottom)\n"
              << "  -h, --help           Show this help\n";
}

int main(int argc, char* argv[]) {
    uint16_t port = DEFAULT_PORT;
    ScreenEdge edge = ScreenEdge::RIGHT;  // Default: client is to the right
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if ((arg == "-e" || arg == "--edge") && i + 1 < argc) {
            std::string e = argv[++i];
            if (e == "left") edge = ScreenEdge::LEFT;
            else if (e == "right") edge = ScreenEdge::RIGHT;
            else if (e == "top") edge = ScreenEdge::TOP;
            else if (e == "bottom") edge = ScreenEdge::BOTTOM;
            else {
                std::cerr << "Invalid edge: " << e << "\n";
                return 1;
            }
        }
    }
    
    // Initialize Winsock
    if (!init_winsock()) {
        std::cerr << "Failed to initialize Winsock\n";
        return 1;
    }
    
    // Set console handler
    SetConsoleCtrlHandler(console_handler, TRUE);
    
    Server server(port, edge);
    bool result = server.run();
    
    cleanup_winsock();
    return result ? 0 : 1;
}