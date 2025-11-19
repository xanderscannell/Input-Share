cmake_minimum_required(VERSION 3.10)
project(MouseShare VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows-specific settings
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)  # Windows 7+
    add_definitions(-DNOMINMAX)              # Prevent min/max macro issues
endif()

# Server executable
add_executable(mouse-share-server
    server.cpp
)

target_link_libraries(mouse-share-server
    ws2_32
)

# Client executable
add_executable(mouse-share-client
    client.cpp
)

target_link_libraries(mouse-share-client
    ws2_32
)

# GUI Application
add_executable(mouse-share-gui WIN32
    gui_app.cpp
)

target_link_libraries(mouse-share-gui
    ws2_32
    comctl32
)

# Installation
install(TARGETS mouse-share-server mouse-share-client mouse-share-gui
    RUNTIME DESTINATION bin
)
