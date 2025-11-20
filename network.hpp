#pragma once

#include "common.hpp"
#include <string>
#include <stdexcept>

namespace MouseShare {

class NetworkError : public std::runtime_error {
public:
    explicit NetworkError(const std::string& msg) : std::runtime_error(msg) {}
};

class Socket {
public:
    Socket() : sock_(INVALID_SOCKET) {}
    
    explicit Socket(SOCKET s) : sock_(s) {}
    
    ~Socket() {
        close();
    }
    
    // Move-only
    Socket(Socket&& other) noexcept : sock_(other.sock_) {
        other.sock_ = INVALID_SOCKET;
    }
    
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            sock_ = other.sock_;
            other.sock_ = INVALID_SOCKET;
        }
        return *this;
    }
    
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    
    void create() {
        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) {
            throw NetworkError("Failed to create socket: " + std::to_string(WSAGetLastError()));
        }
        
        // Enable TCP_NODELAY for low latency
        BOOL flag = TRUE;
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
    }
    
    void bind(uint16_t port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        
        BOOL opt = TRUE;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
        
        if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            throw NetworkError("Failed to bind: " + std::to_string(WSAGetLastError()));
        }
    }
    
    void listen(int backlog = 5) {
        if (::listen(sock_, backlog) == SOCKET_ERROR) {
            throw NetworkError("Failed to listen: " + std::to_string(WSAGetLastError()));
        }
    }
    
    Socket accept() {
        sockaddr_in client_addr{};
        int len = sizeof(client_addr);
        
        SOCKET client_sock = ::accept(sock_, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_sock == INVALID_SOCKET) {
            throw NetworkError("Failed to accept: " + std::to_string(WSAGetLastError()));
        }
        
        // Enable TCP_NODELAY on accepted socket
        BOOL flag = TRUE;
        setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
        
        return Socket(client_sock);
    }
    
    void connect(const std::string& host, uint16_t port) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        
        addrinfo* result;
        int ret = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result);
        if (ret != 0) {
            throw NetworkError("Failed to resolve host: " + std::to_string(ret));
        }
        
        if (::connect(sock_, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
            freeaddrinfo(result);
            throw NetworkError("Failed to connect: " + std::to_string(WSAGetLastError()));
        }
        
        freeaddrinfo(result);
    }
    
    void set_nonblocking(bool nonblocking) {
        u_long mode = nonblocking ? 1 : 0;
        ioctlsocket(sock_, FIONBIO, &mode);
    }
    
    int send(const void* data, int len) {
        return ::send(sock_, (const char*)data, len, 0);
    }
    
    int send(const std::string& data) {
        return send(data.data(), (int)data.size());
    }
    
    int recv(void* buffer, int len) {
        return ::recv(sock_, (char*)buffer, len, 0);
    }
    
    bool recv_exact(void* buffer, int len, int timeout_ms = -1) {
        int received = 0;
        char* buf = static_cast<char*>(buffer);
        
        while (received < len) {
            if (timeout_ms >= 0) {
                fd_set readSet;
                FD_ZERO(&readSet);
                FD_SET(sock_, &readSet);
                
                timeval tv;
                tv.tv_sec = timeout_ms / 1000;
                tv.tv_usec = (timeout_ms % 1000) * 1000;
                
                int ret = select(0, &readSet, nullptr, nullptr, &tv);
                if (ret <= 0) {
                    return false;
                }
            }
            
            int n = recv(buf + received, len - received);
            if (n <= 0) {
                return false;
            }
            received += n;
        }
        
        return true;
    }
    
    void close() {
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }
    
    bool is_valid() const { return sock_ != INVALID_SOCKET; }
    SOCKET handle() const { return sock_; }

    // Check if connection is still alive (non-blocking)
    bool is_connected() const {
        if (sock_ == INVALID_SOCKET) return false;

        // Use select to check if socket is readable
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock_, &readSet);

        timeval tv = {0, 0};  // No timeout - immediate return
        int result = select(0, &readSet, nullptr, nullptr, &tv);

        if (result > 0) {
            // Socket is readable - check if it's because of disconnect
            char buffer[1];
            int n = ::recv(sock_, buffer, 1, MSG_PEEK);
            if (n <= 0) {
                // Connection closed or error
                return false;
            }
        }

        return true;
    }

private:
    SOCKET sock_;
};

} // namespace MouseShare
