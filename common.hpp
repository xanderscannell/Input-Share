#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace MouseShare {

// Protocol version
constexpr uint16_t PROTOCOL_VERSION = 1;
constexpr uint16_t DEFAULT_PORT = 24800;
constexpr uint16_t MAX_PAYLOAD_SIZE = 65535;  // Max reasonable packet size

// Event types
enum class EventType : uint8_t {
    MOUSE_MOVE = 1,
    MOUSE_BUTTON = 2,
    MOUSE_SCROLL = 3,
    KEY_PRESS = 4,
    KEY_RELEASE = 5,
    CLIPBOARD = 6,
    KEEPALIVE = 7,
    SCREEN_INFO = 8,
    SWITCH_SCREEN = 9
};

// Mouse buttons
enum class MouseButton : uint8_t {
    LEFT = 1,
    MIDDLE = 2,
    RIGHT = 3,
    BUTTON4 = 4,
    BUTTON5 = 5
};

// Screen edge for switching
enum class ScreenEdge : uint8_t {
    NONE = 0,
    LEFT = 1,
    RIGHT = 2,
    TOP = 3,
    BOTTOM = 4
};

#pragma pack(push, 1)

// Base packet header
struct PacketHeader {
    uint16_t version;
    EventType type;
    uint32_t timestamp;
    uint16_t payload_size;
};

// Mouse move event
struct MouseMoveEvent {
    int32_t x;
    int32_t y;
    int32_t dx;  // relative movement
    int32_t dy;
};

// Mouse button event
struct MouseButtonEvent {
    MouseButton button;
    bool pressed;
};

// Mouse scroll event
struct MouseScrollEvent {
    int32_t dx;
    int32_t dy;
};

// Keyboard event
struct KeyEvent {
    uint32_t vkCode;      // Virtual key code
    uint32_t scanCode;    // Hardware scan code
    uint32_t flags;       // Key flags
};

// Screen information
struct ScreenInfo {
    int32_t width;
    int32_t height;
    int32_t x;  // position in virtual screen
    int32_t y;
};

// Switch screen command
struct SwitchScreenEvent {
    ScreenEdge edge;
    int32_t position;  // position along the edge
};

#pragma pack(pop)

// Helper to get current timestamp in milliseconds
inline uint32_t get_timestamp() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    );
    return static_cast<uint32_t>(ms.count());
}

// Validate packet header
inline bool is_valid_packet_header(const PacketHeader& header) {
    if (header.version != PROTOCOL_VERSION) return false;
    if (header.payload_size > MAX_PAYLOAD_SIZE) return false;
    return true;
}

// Serialize packet to buffer
template<typename T>
std::string serialize_packet(EventType type, const T& payload) {
    std::string buffer;
    buffer.resize(sizeof(PacketHeader) + sizeof(T));
    
    PacketHeader header;
    header.version = PROTOCOL_VERSION;
    header.type = type;
    header.timestamp = get_timestamp();
    header.payload_size = sizeof(T);
    
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), &payload, sizeof(T));
    
    return buffer;
}

// Serialize empty packet (for keepalive)
inline std::string serialize_keepalive() {
    PacketHeader header;
    header.version = PROTOCOL_VERSION;
    header.type = EventType::KEEPALIVE;
    header.timestamp = get_timestamp();
    header.payload_size = 0;
    
    std::string buffer;
    buffer.resize(sizeof(PacketHeader));
    std::memcpy(buffer.data(), &header, sizeof(header));
    
    return buffer;
}

// Initialize Winsock
inline bool init_winsock() {
#ifdef _WIN32
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
    return true;
#endif
}

// Cleanup Winsock
inline void cleanup_winsock() {
#ifdef _WIN32
    WSACleanup();
#endif
}

} // namespace MouseShare