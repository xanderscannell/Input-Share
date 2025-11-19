@echo off
echo MouseShare Build Script for Windows
echo =====================================
echo.

:: Check for CMake
where cmake >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: CMake not found in PATH
    echo Please install CMake from https://cmake.org/download/
    echo Or install Visual Studio with C++ Desktop Development workload
    pause
    exit /b 1
)

:: Create build directory
if not exist build mkdir build
cd build

:: Try to detect build system
echo Detecting build system...

:: Check for Visual Studio
where cl >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    echo Found Visual Studio compiler
    cmake ..
    cmake --build . --config Release
    goto :done
)

:: Check for MinGW
where gcc >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    echo Found MinGW compiler
    cmake -G "MinGW Makefiles" ..
    mingw32-make
    goto :done
)

:: Default to letting CMake figure it out
echo Using default generator
cmake ..
cmake --build . --config Release

:done
echo.
echo Build complete!
echo.
echo Executables:
if exist Release\mouse-share-gui.exe (
    echo   - Release\mouse-share-gui.exe [GUI - Recommended]
    echo   - Release\mouse-share-server.exe
    echo   - Release\mouse-share-client.exe
) else if exist mouse-share-gui.exe (
    echo   - mouse-share-gui.exe [GUI - Recommended]
    echo   - mouse-share-server.exe
    echo   - mouse-share-client.exe
)
echo.
pause
