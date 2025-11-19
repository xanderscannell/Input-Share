#pragma once

#include "common.hpp"
#include <functional>
#include <atomic>
#include <thread>
#include <iostream>

namespace MouseShare {

// Global hooks need static/global callback functions
class InputCapture {
public:
    using MouseMoveCallback = std::function<void(int x, int y, int dx, int dy)>;
    using MouseButtonCallback = std::function<void(MouseButton button, bool pressed)>;
    using MouseScrollCallback = std::function<void(int dx, int dy)>;
    using KeyCallback = std::function<void(uint32_t vkCode, uint32_t scanCode, uint32_t flags, bool pressed)>;
    
    InputCapture() : running_(false), captured_(false) {
        instance_ = this;
    }
    
    ~InputCapture() {
        stop();
        instance_ = nullptr;
    }
    
    bool init() {
        // Get screen dimensions
        screen_width_ = GetSystemMetrics(SM_CXSCREEN);
        screen_height_ = GetSystemMetrics(SM_CYSCREEN);
        
        // Get initial cursor position
        POINT pt;
        GetCursorPos(&pt);
        last_x_ = pt.x;
        last_y_ = pt.y;
        
        return true;
    }
    
    void set_callbacks(MouseMoveCallback move_cb,
                      MouseButtonCallback button_cb,
                      MouseScrollCallback scroll_cb,
                      KeyCallback key_cb) {
        move_callback_ = std::move(move_cb);
        button_callback_ = std::move(button_cb);
        scroll_callback_ = std::move(scroll_cb);
        key_callback_ = std::move(key_cb);
    }
    
    void start() {
        running_ = true;
        hook_thread_ = std::thread(&InputCapture::hook_thread_func, this);
    }
    
    void stop() {
        running_ = false;
        if (hook_thread_.joinable()) {
            // Post quit message to hook thread
            PostThreadMessage(hook_thread_id_, WM_QUIT, 0, 0);
            hook_thread_.join();
        }
    }
    
    void capture_input(bool capture) {
        captured_ = capture;
    }
    
    bool is_captured() const {
        return captured_;
    }
    
    void get_cursor_position(int& x, int& y) {
        POINT pt;
        GetCursorPos(&pt);
        x = pt.x;
        y = pt.y;
    }
    
    void warp_cursor(int x, int y) {
        SetCursorPos(x, y);
        last_x_ = x;
        last_y_ = y;
    }
    
    int screen_width() const { return screen_width_; }
    int screen_height() const { return screen_height_; }
    
private:
    void hook_thread_func() {
        hook_thread_id_ = GetCurrentThreadId();
        
        // Install low-level mouse hook
        mouse_hook_ = SetWindowsHookEx(WH_MOUSE_LL, mouse_hook_proc, nullptr, 0);
        if (!mouse_hook_) {
            std::cerr << "Failed to install mouse hook: " << GetLastError() << "\n";
            return;
        }
        
        // Install low-level keyboard hook
        keyboard_hook_ = SetWindowsHookEx(WH_KEYBOARD_LL, keyboard_hook_proc, nullptr, 0);
        if (!keyboard_hook_) {
            std::cerr << "Failed to install keyboard hook: " << GetLastError() << "\n";
            UnhookWindowsHookEx(mouse_hook_);
            return;
        }
        
        // Message loop for hooks
        MSG msg;
        while (running_ && GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        
        // Cleanup hooks
        UnhookWindowsHookEx(mouse_hook_);
        UnhookWindowsHookEx(keyboard_hook_);
    }
    
    static LRESULT CALLBACK mouse_hook_proc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0 && instance_) {
            auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            
            switch (wParam) {
                case WM_MOUSEMOVE: {
                    int dx = ms->pt.x - instance_->last_x_;
                    int dy = ms->pt.y - instance_->last_y_;
                    
                    if (instance_->move_callback_ && (dx != 0 || dy != 0)) {
                        instance_->move_callback_(ms->pt.x, ms->pt.y, dx, dy);
                    }
                    
                    instance_->last_x_ = ms->pt.x;
                    instance_->last_y_ = ms->pt.y;
                    break;
                }
                
                case WM_LBUTTONDOWN:
                    if (instance_->button_callback_)
                        instance_->button_callback_(MouseButton::LEFT, true);
                    break;
                case WM_LBUTTONUP:
                    if (instance_->button_callback_)
                        instance_->button_callback_(MouseButton::LEFT, false);
                    break;
                    
                case WM_RBUTTONDOWN:
                    if (instance_->button_callback_)
                        instance_->button_callback_(MouseButton::RIGHT, true);
                    break;
                case WM_RBUTTONUP:
                    if (instance_->button_callback_)
                        instance_->button_callback_(MouseButton::RIGHT, false);
                    break;
                    
                case WM_MBUTTONDOWN:
                    if (instance_->button_callback_)
                        instance_->button_callback_(MouseButton::MIDDLE, true);
                    break;
                case WM_MBUTTONUP:
                    if (instance_->button_callback_)
                        instance_->button_callback_(MouseButton::MIDDLE, false);
                    break;
                    
                case WM_XBUTTONDOWN:
                    if (instance_->button_callback_) {
                        MouseButton btn = (HIWORD(ms->mouseData) == XBUTTON1) 
                            ? MouseButton::BUTTON4 : MouseButton::BUTTON5;
                        instance_->button_callback_(btn, true);
                    }
                    break;
                case WM_XBUTTONUP:
                    if (instance_->button_callback_) {
                        MouseButton btn = (HIWORD(ms->mouseData) == XBUTTON1) 
                            ? MouseButton::BUTTON4 : MouseButton::BUTTON5;
                        instance_->button_callback_(btn, false);
                    }
                    break;
                    
                case WM_MOUSEWHEEL:
                    if (instance_->scroll_callback_) {
                        int delta = GET_WHEEL_DELTA_WPARAM(ms->mouseData);
                        instance_->scroll_callback_(0, delta / WHEEL_DELTA);
                    }
                    break;
                    
                case WM_MOUSEHWHEEL:
                    if (instance_->scroll_callback_) {
                        int delta = GET_WHEEL_DELTA_WPARAM(ms->mouseData);
                        instance_->scroll_callback_(delta / WHEEL_DELTA, 0);
                    }
                    break;
            }
            
            // Block input if captured
            if (instance_->captured_) {
                return 1;
            }
        }
        
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }
    
    static LRESULT CALLBACK keyboard_hook_proc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0 && instance_) {
            auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            bool pressed = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            
            // Check for Scroll Lock to toggle
            if (kb->vkCode == VK_SCROLL && pressed) {
                // This will be handled by the callback
            }
            
            if (instance_->key_callback_) {
                instance_->key_callback_(kb->vkCode, kb->scanCode, kb->flags, pressed);
            }
            
            // Block input if captured
            if (instance_->captured_) {
                return 1;
            }
        }
        
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }
    
    static InputCapture* instance_;
    
    int screen_width_ = 0;
    int screen_height_ = 0;
    int last_x_ = 0;
    int last_y_ = 0;
    
    std::atomic<bool> running_;
    std::atomic<bool> captured_;
    std::thread hook_thread_;
    DWORD hook_thread_id_ = 0;
    
    HHOOK mouse_hook_ = nullptr;
    HHOOK keyboard_hook_ = nullptr;
    
    MouseMoveCallback move_callback_;
    MouseButtonCallback button_callback_;
    MouseScrollCallback scroll_callback_;
    KeyCallback key_callback_;
};

// Static member initialization
InputCapture* InputCapture::instance_ = nullptr;

} // namespace MouseShare
