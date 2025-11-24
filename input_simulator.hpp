#pragma once

#include "common.hpp"
#include <iostream>
#include <algorithm>

namespace MouseShare {

class InputSimulator {
public:
    InputSimulator() {}
    
    ~InputSimulator() {}
    
    bool init() {
        update_screen_dimensions();
        
        // Get initial cursor position
        POINT pt;
        GetCursorPos(&pt);
        current_x_ = pt.x;
        current_y_ = pt.y;
        
        return true;
    }
    
    void update_screen_dimensions() {
        screen_width_ = GetSystemMetrics(SM_CXSCREEN);
        screen_height_ = GetSystemMetrics(SM_CYSCREEN);
    }
    
    void move_mouse(int x, int y) {
        // Clamp to screen bounds
        x = (std::max)(0, (std::min)(x, screen_width_ - 1));
        y = (std::max)(0, (std::min)(y, screen_height_ - 1));
        
        // Convert to normalized coordinates (0-65535)
        int norm_x = (x * 65535) / (screen_width_ - 1);
        int norm_y = (y * 65535) / (screen_height_ - 1);
        
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dx = norm_x;
        input.mi.dy = norm_y;
        input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
        
        SendInput(1, &input, sizeof(INPUT));
        
        current_x_ = x;
        current_y_ = y;
    }
    
    void move_mouse_relative(int dx, int dy) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dx = dx;
        input.mi.dy = dy;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        
        SendInput(1, &input, sizeof(INPUT));
        
        current_x_ += dx;
        current_y_ += dy;
    }
    
    void mouse_button(MouseButton button, bool pressed) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        
        switch (button) {
            case MouseButton::LEFT:
                input.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                break;
            case MouseButton::RIGHT:
                input.mi.dwFlags = pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                break;
            case MouseButton::MIDDLE:
                input.mi.dwFlags = pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
                break;
            case MouseButton::BUTTON4:
                input.mi.dwFlags = pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
                input.mi.mouseData = XBUTTON1;
                break;
            case MouseButton::BUTTON5:
                input.mi.dwFlags = pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
                input.mi.mouseData = XBUTTON2;
                break;
        }
        
        SendInput(1, &input, sizeof(INPUT));
    }
    
    void mouse_scroll(int dx, int dy) {
        // Vertical scroll
        if (dy != 0) {
            INPUT input = {};
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_WHEEL;
            input.mi.mouseData = dy * WHEEL_DELTA;
            SendInput(1, &input, sizeof(INPUT));
        }
        
        // Horizontal scroll
        if (dx != 0) {
            INPUT input = {};
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
            input.mi.mouseData = dx * WHEEL_DELTA;
            SendInput(1, &input, sizeof(INPUT));
        }
    }
    
    void key_event(uint32_t vkCode, uint32_t scanCode, uint32_t flags, bool pressed) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = static_cast<WORD>(vkCode);
        input.ki.wScan = static_cast<WORD>(scanCode);
        input.ki.dwFlags = 0;
        
        // Check for extended key
        if (flags & LLKHF_EXTENDED) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        
        // Key release
        if (!pressed) {
            input.ki.dwFlags |= KEYEVENTF_KEYUP;
        }
        
        SendInput(1, &input, sizeof(INPUT));
    }
    
    void get_cursor_position(int& x, int& y) {
        POINT pt;
        GetCursorPos(&pt);
        x = pt.x;
        y = pt.y;
    }
    
    int screen_width() const { return screen_width_; }
    int screen_height() const { return screen_height_; }
    
private:
    int screen_width_ = 0;
    int screen_height_ = 0;
    int current_x_ = 0;
    int current_y_ = 0;
};

} // namespace MouseShare