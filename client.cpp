#include "common.hpp"
#include "network.hpp"
#include "input_simulator.hpp"
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>

using namespace MouseShare;

std::atomic<bool> g_running{true};

BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

class Client {
public:
    Client(const std::string& server_host, uint16_t port)
        : server_host_(server_host), port_(port), active_(false) {}
    
    bool run() {
        // Initialize input simulator
        if (!simulator_.init()) {
            std::cerr << "Failed to initialize input simulator\n";
            return false;
        }
        
        std::cout << "Screen size: " << simulator_.screen_width() << "x" 
                  << simulator_.screen_height() << "\n";
        
        while (g_running) {
            std::cout << "Connecting to " << server_host_ << ":" << port_ << "...\n";
            
            try {
                socket_.create();
                socket_.connect(server_host_, port_, 5000);  // 5 second timeout
                connected_ = true;
                last_data_received_ = std::chrono::steady_clock::now();
                
                std::cout << "Connected to server!\n";
                
                // Receive and process events
                while (g_running && connected_) {
                    process_events();
                    
                    // Check for connection timeout (no data received for 30 seconds)
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        now - last_data_received_
                    ).count();
                    
                    if (elapsed > 30) {
                        std::cerr << "Connection timeout - no data received for 30 seconds\n";
                        connected_ = false;
                        break;
                    }
                }
                
                socket_.close();
                std::cout << "Disconnected from server\n";
                
            } catch (const NetworkError& e) {
                std::cerr << "Connection failed: " << e.what() << "\n";
                socket_.close();
            }
            
            if (g_running) {
                std::cout << "Reconnecting in 3 seconds...\n";
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
        }
        
        return true;
    }
    
private:
    void process_events() {
        PacketHeader header;
        
        if (!socket_.recv_exact(&header, sizeof(header), 100)) {
            // Timeout or disconnect
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAETIMEDOUT) {
                connected_ = false;
            }
            return;
        }
        
        // Update last received time
        last_data_received_ = std::chrono::steady_clock::now();
        
        // Validate packet header
        if (!is_valid_packet_header(header)) {
            std::cerr << "Invalid packet header: version=" << header.version 
                      << " size=" << header.payload_size << "\n";
            connected_ = false;
            return;
        }
        
        // Read payload if present
        std::vector<char> payload(header.payload_size);
        if (header.payload_size > 0) {
            if (!socket_.recv_exact(payload.data(), header.payload_size)) {
                connected_ = false;
                return;
            }
        }
        
        // Process based on event type
        switch (header.type) {
            case EventType::MOUSE_MOVE:
                if (payload.size() >= sizeof(MouseMoveEvent)) {
                    handle_mouse_move(payload.data());
                }
                break;
            case EventType::MOUSE_BUTTON:
                if (payload.size() >= sizeof(MouseButtonEvent)) {
                    handle_mouse_button(payload.data());
                }
                break;
            case EventType::MOUSE_SCROLL:
                if (payload.size() >= sizeof(MouseScrollEvent)) {
                    handle_mouse_scroll(payload.data());
                }
                break;
            case EventType::KEY_PRESS:
            case EventType::KEY_RELEASE:
                if (payload.size() >= sizeof(KeyEvent)) {
                    handle_key_event(payload.data(), header.type == EventType::KEY_PRESS);
                }
                break;
            case EventType::SCREEN_INFO:
                if (payload.size() >= sizeof(ScreenInfo)) {
                    handle_screen_info(payload.data());
                }
                break;
            case EventType::SWITCH_SCREEN:
                if (payload.size() >= sizeof(SwitchScreenEvent)) {
                    handle_switch_screen(payload.data());
                }
                break;
            case EventType::KEEPALIVE:
                // Just acknowledge keepalive by updating last_data_received_
                break;
            default:
                std::cerr << "Unknown event type: " << static_cast<int>(header.type) << "\n";
                break;
        }
    }
    
    void handle_mouse_move(const char* data) {
        if (!active_) return;
        
        auto* event = reinterpret_cast<const MouseMoveEvent*>(data);
        
        // Use relative movement for smoother tracking
        cursor_x_ += event->dx;
        cursor_y_ += event->dy;
        
        // Clamp to screen bounds
        cursor_x_ = (std::max)(0, (std::min)(cursor_x_, simulator_.screen_width() - 1));
        cursor_y_ = (std::max)(0, (std::min)(cursor_y_, simulator_.screen_height() - 1));
        
        simulator_.move_mouse(cursor_x_, cursor_y_);
        
        // Check if we should switch back to server
        check_edge_switch();
    }
    
    void handle_mouse_button(const char* data) {
        if (!active_) return;
        
        auto* event = reinterpret_cast<const MouseButtonEvent*>(data);
        simulator_.mouse_button(event->button, event->pressed);
    }
    
    void handle_mouse_scroll(const char* data) {
        if (!active_) return;
        
        auto* event = reinterpret_cast<const MouseScrollEvent*>(data);
        simulator_.mouse_scroll(event->dx, event->dy);
    }
    
    void handle_key_event(const char* data, bool pressed) {
        if (!active_) return;
        
        auto* event = reinterpret_cast<const KeyEvent*>(data);
        simulator_.key_event(event->vkCode, event->scanCode, event->flags, pressed);
    }
    
    void handle_screen_info(const char* data) {
        auto* info = reinterpret_cast<const ScreenInfo*>(data);
        server_width_ = info->width;
        server_height_ = info->height;
        
        std::cout << "Server screen: " << server_width_ << "x" << server_height_ << "\n";
    }
    
    void handle_switch_screen(const char* data) {
        auto* event = reinterpret_cast<const SwitchScreenEvent*>(data);
        
        active_ = true;
        entry_edge_ = event->edge;
        
        // Position cursor based on entry edge
        switch (event->edge) {
            case ScreenEdge::LEFT:
                cursor_x_ = 0;
                cursor_y_ = scale_position(event->position, server_height_, simulator_.screen_height());
                break;
            case ScreenEdge::RIGHT:
                cursor_x_ = simulator_.screen_width() - 1;
                cursor_y_ = scale_position(event->position, server_height_, simulator_.screen_height());
                break;
            case ScreenEdge::TOP:
                cursor_x_ = scale_position(event->position, server_width_, simulator_.screen_width());
                cursor_y_ = 0;
                break;
            case ScreenEdge::BOTTOM:
                cursor_x_ = scale_position(event->position, server_width_, simulator_.screen_width());
                cursor_y_ = simulator_.screen_height() - 1;
                break;
            default:
                cursor_x_ = simulator_.screen_width() / 2;
                cursor_y_ = simulator_.screen_height() / 2;
                break;
        }
        
        simulator_.move_mouse(cursor_x_, cursor_y_);
        std::cout << "Input active, entry edge: " << edge_name(entry_edge_) << "\n";
    }
    
    void check_edge_switch() {
        // If cursor moves back to entry edge, deactivate
        bool at_entry_edge = false;
        
        switch (entry_edge_) {
            case ScreenEdge::LEFT:
                at_entry_edge = (cursor_x_ <= 0);
                break;
            case ScreenEdge::RIGHT:
                at_entry_edge = (cursor_x_ >= simulator_.screen_width() - 1);
                break;
            case ScreenEdge::TOP:
                at_entry_edge = (cursor_y_ <= 0);
                break;
            case ScreenEdge::BOTTOM:
                at_entry_edge = (cursor_y_ >= simulator_.screen_height() - 1);
                break;
            default:
                break;
        }
        
        if (at_entry_edge) {
            active_ = false;
            std::cout << "Input returned to server\n";
        }
    }
    
    static int scale_position(int pos, int from_size, int to_size) {
        if (from_size <= 0 || to_size <= 0) return 0;
        return (pos * to_size) / from_size;
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
    
    std::string server_host_;
    uint16_t port_;
    
    InputSimulator simulator_;
    Socket socket_;
    
    std::atomic<bool> connected_{false};
    std::atomic<bool> active_;
    
    int cursor_x_ = 0;
    int cursor_y_ = 0;
    
    int server_width_ = 1920;
    int server_height_ = 1080;
    
    ScreenEdge entry_edge_ = ScreenEdge::NONE;
    
    std::chrono::steady_clock::time_point last_data_received_;
};

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " <server-host> [options]\n"
              << "Options:\n"
              << "  -p, --port PORT      Port to connect to (default: 24800)\n"
              << "  -h, --help           Show this help\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    std::string server_host;
    uint16_t port = DEFAULT_PORT;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (server_host.empty() && arg[0] != '-') {
            server_host = arg;
        }
    }
    
    if (server_host.empty()) {
        std::cerr << "Server host is required\n";
        print_usage(argv[0]);
        return 1;
    }
    
    // Initialize Winsock
    if (!init_winsock()) {
        std::cerr << "Failed to initialize Winsock\n";
        return 1;
    }
    
    // Set console handler
    SetConsoleCtrlHandler(console_handler, TRUE);
    
    Client client(server_host, port);
    bool result = client.run();
    
    cleanup_winsock();
    return result ? 0 : 1;
}