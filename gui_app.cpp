#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <fstream>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Include our existing headers
#include "common.hpp"
#include "network.hpp"
#include "input_capture.hpp"
#include "input_simulator.hpp"

using namespace MouseShare;

// ============================================================================
// Constants and IDs
// ============================================================================

constexpr int DISCOVERY_PORT = 24801;
constexpr int DISCOVERY_INTERVAL_MS = 3000;

// Window IDs
constexpr int ID_LISTVIEW_COMPUTERS = 101;
constexpr int ID_BTN_CONNECT = 102;
constexpr int ID_BTN_DISCONNECT = 103;
constexpr int ID_BTN_START_SERVER = 104;
constexpr int ID_BTN_STOP_SERVER = 105;
constexpr int ID_EDIT_NAME = 106;
constexpr int ID_EDIT_PORT = 107;
constexpr int ID_STATUS_BAR = 108;
constexpr int ID_TRAY_ICON = 109;

// Menu IDs
constexpr int ID_TRAY_SHOW = 1001;
constexpr int ID_TRAY_EXIT = 1002;

// Timer IDs
constexpr int TIMER_DISCOVERY = 1;
constexpr int TIMER_UPDATE = 2;

// Custom messages
constexpr UINT WM_TRAY_ICON = WM_USER + 1;
constexpr UINT WM_UPDATE_STATUS = WM_USER + 2;

// ============================================================================
// Data Structures
// ============================================================================

struct ComputerInfo {
    std::string name;
    std::string ip;
    uint16_t port;
    int screen_width;
    int screen_height;
    bool is_server;
    bool is_connected;
    DWORD last_seen;
    
    // Position in virtual screen layout (for arrangement)
    int layout_x;
    int layout_y;
};

struct ScreenLayout {
    std::vector<ComputerInfo> computers;
    int selected_index;
    bool dragging;
    POINT drag_offset;
    
    // Scale for drawing
    float scale;
    int offset_x;
    int offset_y;
};

// ============================================================================
// Global State
// ============================================================================

class AppState {
public:
    // Window handles
    HWND hwnd_main = nullptr;
    HWND hwnd_layout = nullptr;
    HWND hwnd_list = nullptr;
    HWND hwnd_status = nullptr;
    
    // Computer info
    std::string computer_name;
    uint16_t port = DEFAULT_PORT;
    
    // Network state
    std::atomic<bool> server_running{false};
    std::atomic<bool> client_connected{false};
    std::string connected_to;
    
    // Screen layout
    ScreenLayout layout;
    std::mutex layout_mutex;
    
    // Discovery
    Socket discovery_socket;
    std::thread discovery_thread;
    std::atomic<bool> discovery_running{false};
    
    // Server/Client
    std::unique_ptr<std::thread> server_thread;
    std::unique_ptr<std::thread> client_thread;
    InputCapture input_capture;
    InputSimulator input_simulator;
    Socket server_socket;
    Socket client_socket;
    Socket active_client;
    std::atomic<bool> active_on_remote{false};
    
    // This computer's info
    ComputerInfo local_info;
    
    void init() {
        // Get computer name
        char name[256];
        DWORD size = sizeof(name);
        GetComputerNameA(name, &size);
        computer_name = name;
        
        // Initialize local info
        local_info.name = computer_name;
        local_info.port = port;
        local_info.screen_width = GetSystemMetrics(SM_CXSCREEN);
        local_info.screen_height = GetSystemMetrics(SM_CYSCREEN);
        local_info.is_server = false;
        local_info.is_connected = false;
        local_info.layout_x = 0;
        local_info.layout_y = 0;
        
        // Add local computer to layout
        layout.computers.push_back(local_info);
        layout.selected_index = -1;
        layout.dragging = false;
        layout.scale = 0.1f;
        layout.offset_x = 50;
        layout.offset_y = 50;
    }
};

AppState g_app;

// ============================================================================
// Discovery Protocol
// ============================================================================

#pragma pack(push, 1)
struct DiscoveryPacket {
    char magic[4];  // "MSHR"
    uint8_t type;   // 1 = announce, 2 = query
    uint16_t port;
    int32_t screen_width;
    int32_t screen_height;
    char name[64];
};
#pragma pack(pop)

void broadcast_presence() {
    DiscoveryPacket packet = {};
    memcpy(packet.magic, "MSHR", 4);
    packet.type = 1;  // announce
    packet.port = g_app.port;
    packet.screen_width = g_app.local_info.screen_width;
    packet.screen_height = g_app.local_info.screen_height;
    strncpy_s(packet.name, g_app.computer_name.c_str(), sizeof(packet.name) - 1);
    
    sockaddr_in broadcast_addr = {};
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(DISCOVERY_PORT);
    broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
    
    sendto(g_app.discovery_socket.handle(), (char*)&packet, sizeof(packet), 0,
           (sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
}

void discovery_thread_func() {
    // Create UDP socket for discovery
    g_app.discovery_socket.close();
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;
    
    // Enable broadcast
    BOOL broadcast = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));
    
    // Enable address reuse
    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
    
    // Bind to discovery port
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DISCOVERY_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return;
    }
    
    // Set non-blocking
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
    
    g_app.discovery_socket = Socket(sock);
    
    while (g_app.discovery_running) {
        // Broadcast our presence
        broadcast_presence();
        
        // Listen for others
        char buffer[512];
        sockaddr_in from_addr;
        int from_len = sizeof(from_addr);
        
        while (true) {
            int received = recvfrom(g_app.discovery_socket.handle(), buffer, sizeof(buffer), 0,
                                   (sockaddr*)&from_addr, &from_len);
            if (received <= 0) break;
            
            if (received >= sizeof(DiscoveryPacket)) {
                auto* packet = (DiscoveryPacket*)buffer;
                if (memcmp(packet->magic, "MSHR", 4) == 0) {
                    // Get IP string
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &from_addr.sin_addr, ip_str, sizeof(ip_str));
                    
                    // Don't add ourselves
                    if (strcmp(packet->name, g_app.computer_name.c_str()) == 0) continue;
                    
                    // Update or add computer
                    std::lock_guard<std::mutex> lock(g_app.layout_mutex);
                    
                    bool found = false;
                    for (auto& comp : g_app.layout.computers) {
                        if (comp.name == packet->name) {
                            comp.ip = ip_str;
                            comp.port = packet->port;
                            comp.screen_width = packet->screen_width;
                            comp.screen_height = packet->screen_height;
                            comp.last_seen = GetTickCount();
                            found = true;
                            break;
                        }
                    }
                    
                    if (!found) {
                        ComputerInfo info;
                        info.name = packet->name;
                        info.ip = ip_str;
                        info.port = packet->port;
                        info.screen_width = packet->screen_width;
                        info.screen_height = packet->screen_height;
                        info.is_server = false;
                        info.is_connected = false;
                        info.last_seen = GetTickCount();
                        
                        // Position to the right of existing screens
                        int max_x = 0;
                        for (const auto& c : g_app.layout.computers) {
                            max_x = (std::max)(max_x, c.layout_x + c.screen_width);
                        }
                        info.layout_x = max_x + 50;
                        info.layout_y = 0;
                        
                        g_app.layout.computers.push_back(info);
                        
                        // Update UI
                        PostMessage(g_app.hwnd_main, WM_UPDATE_STATUS, 0, 0);
                    }
                }
            }
        }
        
        // Remove stale computers (not seen in 10 seconds)
        {
            std::lock_guard<std::mutex> lock(g_app.layout_mutex);
            DWORD now = GetTickCount();
            g_app.layout.computers.erase(
                std::remove_if(g_app.layout.computers.begin(), g_app.layout.computers.end(),
                    [now](const ComputerInfo& c) {
                        return !c.name.empty() && 
                               c.name != g_app.computer_name &&
                               (now - c.last_seen) > 10000;
                    }),
                g_app.layout.computers.end()
            );
        }
        
        Sleep(DISCOVERY_INTERVAL_MS);
    }
}

// ============================================================================
// Server/Client Logic
// ============================================================================

void server_thread_func() {
    if (!g_app.input_capture.init()) {
        PostMessage(g_app.hwnd_main, WM_UPDATE_STATUS, 0, (LPARAM)"Failed to init input capture");
        return;
    }
    
    // Set up callbacks
    g_app.input_capture.set_callbacks(
        // Mouse move
        [](int x, int y, int dx, int dy) {
            if (!g_app.active_client.is_valid()) return;
            
            // Check for edge switching
            bool at_right_edge = (x >= g_app.local_info.screen_width - 1);
            
            if (at_right_edge && !g_app.active_on_remote) {
                // Switch to client
                g_app.active_on_remote = true;
                g_app.input_capture.capture_input(true);
                
                SwitchScreenEvent event;
                event.edge = ScreenEdge::LEFT;
                event.position = y;
                auto data = serialize_packet(EventType::SWITCH_SCREEN, event);
                g_app.active_client.send(data);
                
                // Move cursor away from edge
                g_app.input_capture.warp_cursor(g_app.local_info.screen_width / 2, y);
            } else if (g_app.active_on_remote) {
                MouseMoveEvent event;
                event.x = x;
                event.y = y;
                event.dx = dx;
                event.dy = dy;
                auto data = serialize_packet(EventType::MOUSE_MOVE, event);
                g_app.active_client.send(data);
            }
        },
        // Mouse button
        [](MouseButton button, bool pressed) {
            if (!g_app.active_on_remote) return;
            MouseButtonEvent event;
            event.button = button;
            event.pressed = pressed;
            auto data = serialize_packet(EventType::MOUSE_BUTTON, event);
            g_app.active_client.send(data);
        },
        // Mouse scroll
        [](int dx, int dy) {
            if (!g_app.active_on_remote) return;
            MouseScrollEvent event;
            event.dx = dx;
            event.dy = dy;
            auto data = serialize_packet(EventType::MOUSE_SCROLL, event);
            g_app.active_client.send(data);
        },
        // Keyboard
        [](uint32_t vk, uint32_t scan, uint32_t flags, bool pressed) {
            // Scroll Lock to toggle
            if (vk == VK_SCROLL && pressed) {
                if (g_app.active_on_remote) {
                    g_app.active_on_remote = false;
                    g_app.input_capture.capture_input(false);
                }
                return;
            }
            
            if (!g_app.active_on_remote) return;
            KeyEvent event;
            event.vkCode = vk;
            event.scanCode = scan;
            event.flags = flags;
            auto data = serialize_packet(pressed ? EventType::KEY_PRESS : EventType::KEY_RELEASE, event);
            g_app.active_client.send(data);
        }
    );
    
    g_app.input_capture.start();
    
    try {
        g_app.server_socket.create();
        g_app.server_socket.bind(g_app.port);
        g_app.server_socket.listen();
        
        PostMessage(g_app.hwnd_main, WM_UPDATE_STATUS, 0, (LPARAM)"Server started");
        
        while (g_app.server_running) {
            // Accept with timeout
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(g_app.server_socket.handle(), &readSet);
            
            timeval tv = {1, 0};
            if (select(0, &readSet, nullptr, nullptr, &tv) > 0) {
                g_app.active_client = g_app.server_socket.accept();
                
                // Send screen info
                ScreenInfo info;
                info.width = g_app.local_info.screen_width;
                info.height = g_app.local_info.screen_height;
                auto data = serialize_packet(EventType::SCREEN_INFO, info);
                g_app.active_client.send(data);
                
                PostMessage(g_app.hwnd_main, WM_UPDATE_STATUS, 0, (LPARAM)"Client connected");
                
                // Keep connection alive
                while (g_app.server_running && g_app.active_client.is_valid()) {
                    Sleep(100);
                }
                
                g_app.active_client.close();
                g_app.active_on_remote = false;
                g_app.input_capture.capture_input(false);
            }
        }
    } catch (const NetworkError& e) {
        PostMessage(g_app.hwnd_main, WM_UPDATE_STATUS, 0, (LPARAM)e.what());
    }
    
    g_app.input_capture.stop();
    g_app.server_socket.close();
}

void client_thread_func(std::string host, uint16_t port) {
    if (!g_app.input_simulator.init()) {
        PostMessage(g_app.hwnd_main, WM_UPDATE_STATUS, 0, (LPARAM)"Failed to init input simulator");
        return;
    }
    
    try {
        g_app.client_socket.create();
        g_app.client_socket.connect(host, port);
        g_app.client_connected = true;
        g_app.connected_to = host;
        
        PostMessage(g_app.hwnd_main, WM_UPDATE_STATUS, 0, (LPARAM)"Connected to server");
        
        int cursor_x = 0, cursor_y = 0;
        bool active = false;
        
        while (g_app.client_connected) {
            PacketHeader header;
            if (!g_app.client_socket.recv_exact(&header, sizeof(header), 100)) {
                continue;
            }
            
            std::vector<char> payload(header.payload_size);
            if (!g_app.client_socket.recv_exact(payload.data(), header.payload_size)) {
                break;
            }
            
            switch (header.type) {
                case EventType::MOUSE_MOVE: {
                    if (!active) break;
                    auto* e = (MouseMoveEvent*)payload.data();
                    cursor_x += e->dx;
                    cursor_y += e->dy;
                    cursor_x = (std::max)(0, (std::min)(cursor_x, g_app.local_info.screen_width - 1));
                    cursor_y = (std::max)(0, (std::min)(cursor_y, g_app.local_info.screen_height - 1));
                    g_app.input_simulator.move_mouse(cursor_x, cursor_y);
                    
                    // Check for return to server
                    if (cursor_x <= 0) {
                        active = false;
                    }
                    break;
                }
                case EventType::MOUSE_BUTTON: {
                    if (!active) break;
                    auto* e = (MouseButtonEvent*)payload.data();
                    g_app.input_simulator.mouse_button(e->button, e->pressed);
                    break;
                }
                case EventType::MOUSE_SCROLL: {
                    if (!active) break;
                    auto* e = (MouseScrollEvent*)payload.data();
                    g_app.input_simulator.mouse_scroll(e->dx, e->dy);
                    break;
                }
                case EventType::KEY_PRESS:
                case EventType::KEY_RELEASE: {
                    if (!active) break;
                    auto* e = (KeyEvent*)payload.data();
                    g_app.input_simulator.key_event(e->vkCode, e->scanCode, e->flags,
                                                    header.type == EventType::KEY_PRESS);
                    break;
                }
                case EventType::SWITCH_SCREEN: {
                    auto* e = (SwitchScreenEvent*)payload.data();
                    active = true;
                    cursor_x = 0;
                    cursor_y = e->position;
                    g_app.input_simulator.move_mouse(cursor_x, cursor_y);
                    break;
                }
                default:
                    break;
            }
        }
    } catch (const NetworkError& e) {
        PostMessage(g_app.hwnd_main, WM_UPDATE_STATUS, 0, (LPARAM)e.what());
    }
    
    g_app.client_socket.close();
    g_app.client_connected = false;
    PostMessage(g_app.hwnd_main, WM_UPDATE_STATUS, 0, (LPARAM)"Disconnected");
}

// ============================================================================
// Screen Layout Drawing
// ============================================================================

void draw_layout(HDC hdc, RECT& rect) {
    // Background
    HBRUSH bg_brush = CreateSolidBrush(RGB(240, 240, 240));
    FillRect(hdc, &rect, bg_brush);
    DeleteObject(bg_brush);
    
    // Draw grid
    HPEN grid_pen = CreatePen(PS_DOT, 1, RGB(200, 200, 200));
    SelectObject(hdc, grid_pen);
    
    for (int x = 0; x < rect.right; x += 50) {
        MoveToEx(hdc, x, 0, nullptr);
        LineTo(hdc, x, rect.bottom);
    }
    for (int y = 0; y < rect.bottom; y += 50) {
        MoveToEx(hdc, 0, y, nullptr);
        LineTo(hdc, rect.right, y);
    }
    DeleteObject(grid_pen);
    
    std::lock_guard<std::mutex> lock(g_app.layout_mutex);
    
    // Calculate scale to fit all monitors
    float scale = g_app.layout.scale;
    int offset_x = g_app.layout.offset_x;
    int offset_y = g_app.layout.offset_y;
    
    // Draw each computer's screen
    for (size_t i = 0; i < g_app.layout.computers.size(); i++) {
        const auto& comp = g_app.layout.computers[i];
        
        int x = offset_x + (int)(comp.layout_x * scale);
        int y = offset_y + (int)(comp.layout_y * scale);
        int w = (int)(comp.screen_width * scale);
        int h = (int)(comp.screen_height * scale);
        
        // Monitor rectangle
        RECT monitor_rect = {x, y, x + w, y + h};
        
        // Fill color based on state
        COLORREF fill_color;
        if (comp.name == g_app.computer_name) {
            fill_color = RGB(100, 200, 100);  // Green for local
        } else if (comp.is_connected) {
            fill_color = RGB(100, 150, 255);  // Blue for connected
        } else {
            fill_color = RGB(200, 200, 200);  // Gray for available
        }
        
        HBRUSH fill_brush = CreateSolidBrush(fill_color);
        FillRect(hdc, &monitor_rect, fill_brush);
        DeleteObject(fill_brush);
        
        // Border
        HPEN border_pen;
        if ((int)i == g_app.layout.selected_index) {
            border_pen = CreatePen(PS_SOLID, 3, RGB(255, 100, 100));
        } else {
            border_pen = CreatePen(PS_SOLID, 2, RGB(50, 50, 50));
        }
        SelectObject(hdc, border_pen);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, x, y, x + w, y + h);
        DeleteObject(border_pen);
        
        // Computer name
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 0, 0));
        
        std::string label = comp.name;
        if (comp.name == g_app.computer_name) {
            label += " (This PC)";
        }
        
        RECT text_rect = {x + 5, y + 5, x + w - 5, y + h - 5};
        DrawTextA(hdc, label.c_str(), -1, &text_rect, DT_LEFT | DT_TOP | DT_WORDBREAK);
        
        // Resolution
        std::string res = std::to_string(comp.screen_width) + "x" + std::to_string(comp.screen_height);
        DrawTextA(hdc, res.c_str(), -1, &text_rect, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
    }
    
    // Instructions
    SetTextColor(hdc, RGB(100, 100, 100));
    const char* instructions = "Drag monitors to arrange. Click to select.";
    TextOutA(hdc, 10, rect.bottom - 25, instructions, (int)strlen(instructions));
}

int hit_test_layout(int mouse_x, int mouse_y) {
    std::lock_guard<std::mutex> lock(g_app.layout_mutex);
    
    float scale = g_app.layout.scale;
    int offset_x = g_app.layout.offset_x;
    int offset_y = g_app.layout.offset_y;
    
    for (int i = (int)g_app.layout.computers.size() - 1; i >= 0; i--) {
        const auto& comp = g_app.layout.computers[i];
        
        int x = offset_x + (int)(comp.layout_x * scale);
        int y = offset_y + (int)(comp.layout_y * scale);
        int w = (int)(comp.screen_width * scale);
        int h = (int)(comp.screen_height * scale);
        
        if (mouse_x >= x && mouse_x < x + w && mouse_y >= y && mouse_y < y + h) {
            return i;
        }
    }
    
    return -1;
}

// ============================================================================
// Window Procedures
// ============================================================================

LRESULT CALLBACK layout_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT rect;
            GetClientRect(hwnd, &rect);
            
            // Double buffer
            HDC mem_dc = CreateCompatibleDC(hdc);
            HBITMAP mem_bmp = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
            SelectObject(mem_dc, mem_bmp);
            
            draw_layout(mem_dc, rect);
            
            BitBlt(hdc, 0, 0, rect.right, rect.bottom, mem_dc, 0, 0, SRCCOPY);
            
            DeleteObject(mem_bmp);
            DeleteDC(mem_dc);
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            
            int hit = hit_test_layout(x, y);
            g_app.layout.selected_index = hit;
            
            if (hit >= 0 && g_app.layout.computers[hit].name != g_app.computer_name) {
                // Start dragging
                g_app.layout.dragging = true;
                SetCapture(hwnd);
                
                auto& comp = g_app.layout.computers[hit];
                float scale = g_app.layout.scale;
                int offset_x = g_app.layout.offset_x;
                int offset_y = g_app.layout.offset_y;
                
                int screen_x = offset_x + (int)(comp.layout_x * scale);
                int screen_y = offset_y + (int)(comp.layout_y * scale);
                
                g_app.layout.drag_offset.x = x - screen_x;
                g_app.layout.drag_offset.y = y - screen_y;
            }
            
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        
        case WM_MOUSEMOVE: {
            if (g_app.layout.dragging && g_app.layout.selected_index >= 0) {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                
                std::lock_guard<std::mutex> lock(g_app.layout_mutex);
                auto& comp = g_app.layout.computers[g_app.layout.selected_index];
                
                float scale = g_app.layout.scale;
                int offset_x = g_app.layout.offset_x;
                int offset_y = g_app.layout.offset_y;
                
                comp.layout_x = (int)((x - g_app.layout.drag_offset.x - offset_x) / scale);
                comp.layout_y = (int)((y - g_app.layout.drag_offset.y - offset_y) / scale);
                
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        
        case WM_LBUTTONUP: {
            if (g_app.layout.dragging) {
                g_app.layout.dragging = false;
                ReleaseCapture();
            }
            return 0;
        }
        
        case WM_MOUSEWHEEL: {
            // Zoom
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            
            if (delta > 0) {
                g_app.layout.scale *= 1.1f;
            } else {
                g_app.layout.scale /= 1.1f;
            }
            
            g_app.layout.scale = (std::max)(0.02f, (std::min)(0.5f, g_app.layout.scale));
            
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
    }
    
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Create layout panel
            WNDCLASSA layout_class = {};
            layout_class.lpfnWndProc = layout_wnd_proc;
            layout_class.hInstance = GetModuleHandle(nullptr);
            layout_class.lpszClassName = "LayoutPanel";
            layout_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassA(&layout_class);
            
            g_app.hwnd_layout = CreateWindowExA(
                WS_EX_CLIENTEDGE,
                "LayoutPanel", "",
                WS_CHILD | WS_VISIBLE,
                10, 10, 500, 300,
                hwnd, nullptr, GetModuleHandle(nullptr), nullptr
            );
            
            // Create computer list
            g_app.hwnd_list = CreateWindowExA(
                WS_EX_CLIENTEDGE,
                WC_LISTVIEWA, "",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                520, 10, 260, 200,
                hwnd, (HMENU)(INT_PTR)ID_LISTVIEW_COMPUTERS, GetModuleHandle(nullptr), nullptr
            );
            
            ListView_SetExtendedListViewStyle(g_app.hwnd_list, 
                LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            
            // Add columns
            LVCOLUMNA col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = (LPSTR)"Computer";
            col.cx = 120;
            SendMessageA(g_app.hwnd_list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
            
            col.pszText = (LPSTR)"IP Address";
            col.cx = 100;
            SendMessageA(g_app.hwnd_list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
            
            // Create buttons
            CreateWindowA("BUTTON", "Start Server",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                520, 220, 125, 30,
                hwnd, (HMENU)(INT_PTR)ID_BTN_START_SERVER, GetModuleHandle(nullptr), nullptr);
            
            CreateWindowA("BUTTON", "Stop Server",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                655, 220, 125, 30,
                hwnd, (HMENU)(INT_PTR)ID_BTN_STOP_SERVER, GetModuleHandle(nullptr), nullptr);
            
            CreateWindowA("BUTTON", "Connect",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                520, 260, 125, 30,
                hwnd, (HMENU)(INT_PTR)ID_BTN_CONNECT, GetModuleHandle(nullptr), nullptr);
            
            CreateWindowA("BUTTON", "Disconnect",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                655, 260, 125, 30,
                hwnd, (HMENU)(INT_PTR)ID_BTN_DISCONNECT, GetModuleHandle(nullptr), nullptr);
            
            // Labels and edit controls
            CreateWindowA("STATIC", "Computer Name:",
                WS_CHILD | WS_VISIBLE,
                10, 320, 100, 20,
                hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
            
            HWND edit_name = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                120, 318, 150, 24,
                hwnd, (HMENU)(INT_PTR)ID_EDIT_NAME, GetModuleHandle(nullptr), nullptr);
            SetWindowTextA(edit_name, g_app.computer_name.c_str());
            
            CreateWindowA("STATIC", "Port:",
                WS_CHILD | WS_VISIBLE,
                290, 320, 40, 20,
                hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
            
            HWND edit_port = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                330, 318, 60, 24,
                hwnd, (HMENU)(INT_PTR)ID_EDIT_PORT, GetModuleHandle(nullptr), nullptr);
            SetWindowTextA(edit_port, std::to_string(g_app.port).c_str());
            
            // Status bar
            g_app.hwnd_status = CreateWindowA(STATUSCLASSNAMEA, "",
                WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                0, 0, 0, 0,
                hwnd, (HMENU)(INT_PTR)ID_STATUS_BAR, GetModuleHandle(nullptr), nullptr);
            
            SendMessageA(g_app.hwnd_status, SB_SETTEXTA, 0, (LPARAM)"Ready - Discovering computers on network...");
            
            // Start discovery
            g_app.discovery_running = true;
            g_app.discovery_thread = std::thread(discovery_thread_func);
            
            // Set update timer
            SetTimer(hwnd, TIMER_UPDATE, 1000, nullptr);
            
            return 0;
        }
        
        case WM_TIMER: {
            if (wParam == TIMER_UPDATE) {
                // Update computer list
                std::lock_guard<std::mutex> lock(g_app.layout_mutex);
                
                SendMessageA(g_app.hwnd_list, LVM_DELETEALLITEMS, 0, 0);
                
                for (size_t i = 0; i < g_app.layout.computers.size(); i++) {
                    const auto& comp = g_app.layout.computers[i];
                    if (comp.name == g_app.computer_name) continue;
                    
                    LVITEMA item = {};
                    item.mask = LVIF_TEXT;
                    item.iItem = (int)i;
                    item.pszText = (LPSTR)comp.name.c_str();
                    int idx = (int)SendMessageA(g_app.hwnd_list, LVM_INSERTITEMA, 0, (LPARAM)&item);
                    
                    LVITEMA subitem = {};
                    subitem.mask = LVIF_TEXT;
                    subitem.iItem = idx;
                    subitem.iSubItem = 1;
                    subitem.pszText = (LPSTR)comp.ip.c_str();
                    SendMessageA(g_app.hwnd_list, LVM_SETITEMA, 0, (LPARAM)&subitem);
                }
                
                InvalidateRect(g_app.hwnd_layout, nullptr, FALSE);
            }
            return 0;
        }
        
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            
            switch (id) {
                case ID_BTN_START_SERVER: {
                    // Get port from edit control
                    char port_str[16];
                    GetWindowTextA(GetDlgItem(hwnd, ID_EDIT_PORT), port_str, sizeof(port_str));
                    g_app.port = (uint16_t)atoi(port_str);
                    
                    g_app.server_running = true;
                    g_app.server_thread = std::make_unique<std::thread>(server_thread_func);
                    
                    EnableWindow(GetDlgItem(hwnd, ID_BTN_START_SERVER), FALSE);
                    EnableWindow(GetDlgItem(hwnd, ID_BTN_STOP_SERVER), TRUE);
                    break;
                }
                
                case ID_BTN_STOP_SERVER: {
                    g_app.server_running = false;
                    if (g_app.server_thread && g_app.server_thread->joinable()) {
                        g_app.server_thread->join();
                    }
                    g_app.server_socket.close();
                    g_app.active_client.close();
                    
                    EnableWindow(GetDlgItem(hwnd, ID_BTN_START_SERVER), TRUE);
                    EnableWindow(GetDlgItem(hwnd, ID_BTN_STOP_SERVER), FALSE);
                    
                    SendMessageA(g_app.hwnd_status, SB_SETTEXTA, 0, (LPARAM)"Server stopped");
                    break;
                }
                
                case ID_BTN_CONNECT: {
                    // Get selected computer
                    int sel = (int)SendMessageA(g_app.hwnd_list, LVM_GETNEXTITEM, -1, LVNI_SELECTED);
                    if (sel < 0) {
                        MessageBoxA(hwnd, "Please select a computer to connect to", 
                                   "MouseShare", MB_OK | MB_ICONINFORMATION);
                        break;
                    }
                    
                    char name[256], ip[64];
                    LVITEMA item = {};
                    item.mask = LVIF_TEXT;
                    item.iItem = sel;
                    item.iSubItem = 0;
                    item.pszText = name;
                    item.cchTextMax = sizeof(name);
                    SendMessageA(g_app.hwnd_list, LVM_GETITEMA, 0, (LPARAM)&item);
                    
                    item.iSubItem = 1;
                    item.pszText = ip;
                    item.cchTextMax = sizeof(ip);
                    SendMessageA(g_app.hwnd_list, LVM_GETITEMA, 0, (LPARAM)&item);
                    
                    // Find port
                    uint16_t port = DEFAULT_PORT;
                    {
                        std::lock_guard<std::mutex> lock(g_app.layout_mutex);
                        for (const auto& comp : g_app.layout.computers) {
                            if (comp.name == name) {
                                port = comp.port;
                                break;
                            }
                        }
                    }
                    
                    g_app.client_thread = std::make_unique<std::thread>(
                        client_thread_func, std::string(ip), port);
                    
                    EnableWindow(GetDlgItem(hwnd, ID_BTN_CONNECT), FALSE);
                    EnableWindow(GetDlgItem(hwnd, ID_BTN_DISCONNECT), TRUE);
                    break;
                }
                
                case ID_BTN_DISCONNECT: {
                    g_app.client_connected = false;
                    if (g_app.client_thread && g_app.client_thread->joinable()) {
                        g_app.client_thread->join();
                    }
                    g_app.client_socket.close();
                    
                    EnableWindow(GetDlgItem(hwnd, ID_BTN_CONNECT), TRUE);
                    EnableWindow(GetDlgItem(hwnd, ID_BTN_DISCONNECT), FALSE);
                    break;
                }
                
                case ID_TRAY_SHOW:
                    ShowWindow(hwnd, SW_SHOW);
                    SetForegroundWindow(hwnd);
                    break;
                    
                case ID_TRAY_EXIT:
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;
        }
        
        case WM_UPDATE_STATUS: {
            if (lParam) {
                SendMessageA(g_app.hwnd_status, SB_SETTEXTA, 0, lParam);
            }
            InvalidateRect(g_app.hwnd_layout, nullptr, FALSE);
            return 0;
        }
        
        case WM_TRAY_ICON: {
            if (lParam == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                
                HMENU menu = CreatePopupMenu();
                AppendMenuA(menu, MF_STRING, ID_TRAY_SHOW, "Show");
                AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuA(menu, MF_STRING, ID_TRAY_EXIT, "Exit");
                
                SetForegroundWindow(hwnd);
                TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                              pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
            } else if (lParam == WM_LBUTTONDBLCLK) {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            }
            return 0;
        }
        
        case WM_SIZE: {
            // Resize status bar
            SendMessage(g_app.hwnd_status, WM_SIZE, 0, 0);
            return 0;
        }
        
        case WM_CLOSE: {
            // Minimize to tray instead of closing
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        
        case WM_DESTROY: {
            // Cleanup
            g_app.discovery_running = false;
            g_app.server_running = false;
            g_app.client_connected = false;
            
            if (g_app.discovery_thread.joinable()) {
                g_app.discovery_thread.join();
            }
            if (g_app.server_thread && g_app.server_thread->joinable()) {
                g_app.server_thread->join();
            }
            if (g_app.client_thread && g_app.client_thread->joinable()) {
                g_app.client_thread->join();
            }
            
            // Remove tray icon
            NOTIFYICONDATAA nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = ID_TRAY_ICON;
            Shell_NotifyIconA(NIM_DELETE, &nid);
            
            PostQuitMessage(0);
            return 0;
        }
    }
    
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// ============================================================================
// Main Entry Point
// ============================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Initialize Winsock
    if (!init_winsock()) {
        MessageBoxA(nullptr, "Failed to initialize Winsock", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    // Initialize common controls
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    
    // Initialize app state
    g_app.init();
    
    // Register window class
    WNDCLASSA wc = {};
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "MouseShareMain";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassA(&wc);
    
    // Create main window
    g_app.hwnd_main = CreateWindowExA(
        0,
        "MouseShareMain",
        "MouseShare - Share Mouse & Keyboard",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 420,
        nullptr, nullptr, hInstance, nullptr
    );
    
    if (!g_app.hwnd_main) {
        MessageBoxA(nullptr, "Failed to create window", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    // Add tray icon
    NOTIFYICONDATAA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hwnd_main;
    nid.uID = ID_TRAY_ICON;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_ICON;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    strcpy_s(nid.szTip, "MouseShare");
    Shell_NotifyIconA(NIM_ADD, &nid);
    
    ShowWindow(g_app.hwnd_main, nCmdShow);
    UpdateWindow(g_app.hwnd_main);
    
    // Message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    cleanup_winsock();
    return (int)msg.wParam;
}