# MouseShare for Windows

A lightweight C++ application for sharing your mouse and keyboard across multiple Windows computers on a local network. Similar to Synergy/Barrier but simplified and focused on core functionality.

## Features

- **Visual GUI**: Drag-and-drop monitor arrangement with automatic network discovery
- **Seamless cursor switching**: Move your cursor to a screen edge to switch to another computer
- **Full input support**: Mouse movement, buttons, scroll wheel, and keyboard
- **Auto-discovery**: Automatically finds other MouseShare computers on your network
- **Low latency**: TCP with NO_DELAY for responsive input
- **Hotkey toggle**: Press Scroll Lock to manually switch between computers
- **System tray**: Runs in background with tray icon
- **Command-line tools**: Also includes CLI server/client for advanced users

## Architecture

```
┌─────────────────┐                    ┌─────────────────┐
│     Server      │                    │     Client      │
│  (Primary PC)   │                    │ (Secondary PC)  │
│                 │     TCP/24800      │                 │
│  Input Capture  │ ──────────────────▶│ Input Simulator │
│ (Low-level Hook)│   Mouse/Keyboard   │   (SendInput)   │
│                 │      Events        │                 │
└─────────────────┘                    └─────────────────┘
```

- **Server**: Runs on your primary computer (the one with the mouse/keyboard)
  - Captures all input events using low-level Windows hooks
  - Detects when cursor hits screen edge
  - Sends input events to client over network
  
- **Client**: Runs on secondary computer(s)
  - Receives input events from server
  - Simulates input using Windows SendInput API
  - Detects when cursor returns to entry edge

## Requirements

### Option 1: Visual Studio (Recommended)

- Visual Studio 2019 or later with C++ Desktop Development workload
- CMake (included with Visual Studio)

### Option 2: MinGW-w64

- MinGW-w64 (GCC for Windows)
- CMake

### Option 3: MSYS2

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake
```

## Building

### Using Visual Studio

1. Open Visual Studio
2. Select "Open a local folder" and choose the `mouse-share-win` folder
3. Visual Studio will auto-detect CMakeLists.txt
4. Select Build > Build All (or press Ctrl+Shift+B)
5. Executables will be in `out/build/<config>/`

### Using Visual Studio Command Line

```cmd
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

### Using MinGW

```cmd
mkdir build
cd build
cmake -G "MinGW Makefiles" ..
mingw32-make
```

### Using MSYS2/MINGW64

```bash
mkdir build && cd build
cmake -G "MSYS Makefiles" ..
make
```

This creates three executables:
- `mouse-share-gui.exe` - **GUI application (recommended)**
- `mouse-share-server.exe` - Command-line server
- `mouse-share-client.exe` - Command-line client

## Usage

### GUI Application (Recommended)

Simply run `mouse-share-gui.exe` on all computers you want to share between.

**Features:**
- **Auto-discovery**: Other computers running MouseShare appear automatically
- **Visual arrangement**: Drag monitor rectangles to position them
- **Easy connection**: Select a computer from the list and click Connect
- **System tray**: Minimizes to tray, double-click icon to restore

**How to use:**

1. Run `mouse-share-gui.exe` on all computers
2. Wait a few seconds for computers to discover each other
3. On your **primary PC** (with keyboard/mouse), click "Start Server"
4. On **secondary PCs**, select the primary PC from the list and click "Connect"
5. Move your cursor to the right edge to switch to the connected computer
6. Move cursor back to left edge (or press Scroll Lock) to return

**Monitor Arrangement:**
- Drag the monitor rectangles in the visual editor to arrange them
- Green = This PC
- Blue = Connected
- Gray = Available on network
- Use mouse wheel to zoom in/out

### Command Line Usage (Advanced)

1. **On the primary computer** (where your mouse/keyboard are):
   ```cmd
   mouse-share-server.exe
   ```

2. **On the secondary computer**:
   ```cmd
   mouse-share-client.exe 192.168.1.100
   ```

3. Move your cursor to the right edge of the primary screen to switch to the secondary computer.

### Server Options

```cmd
mouse-share-server.exe [options]

Options:
  -p, --port PORT      Port to listen on (default: 24800)
  -e, --edge EDGE      Edge to switch screens (left/right/top/bottom)
  -h, --help           Show help
```

**Examples:**

```cmd
# Client screen is to the right (default)
mouse-share-server.exe

# Client screen is to the left
mouse-share-server.exe --edge left

# Client screen is above
mouse-share-server.exe --edge top

# Use custom port
mouse-share-server.exe --port 12345
```

### Client Options

```cmd
mouse-share-client.exe <server-host> [options]

Options:
  -p, --port PORT      Port to connect to (default: 24800)
  -h, --help           Show help
```

**Examples:**

```cmd
# Connect to server at 192.168.1.100
mouse-share-client.exe 192.168.1.100

# Use custom port
mouse-share-client.exe 192.168.1.100 --port 12345

# Connect using hostname
mouse-share-client.exe my-desktop
```

### Switching Computers

There are two ways to switch between computers:

1. **Screen Edge**: Move cursor to the configured edge
   - Server moves cursor to edge → switches to client
   - Client moves cursor back to entry edge → switches to server

2. **Hotkey**: Press **Scroll Lock** to toggle manually

## Network Configuration

### Windows Firewall

When you first run the application, Windows may prompt to allow it through the firewall. Click "Allow access" for private networks.

To manually add a firewall rule:

```cmd
# Allow server through firewall (run as Administrator)
netsh advfirewall firewall add rule name="MouseShare Server" dir=in action=allow protocol=tcp localport=24800 profile=private

# Or using PowerShell
New-NetFirewallRule -DisplayName "MouseShare Server" -Direction Inbound -Protocol TCP -LocalPort 24800 -Action Allow -Profile Private
```

### Finding Your IP Address

```cmd
ipconfig
```

Look for "IPv4 Address" under your active network adapter.

## Troubleshooting

### "Access Denied" or Input Not Working

Low-level hooks require appropriate permissions. Try:

1. Run as Administrator (right-click → Run as administrator)
2. Some antivirus software may block keyboard/mouse hooks - add an exception

### Connection Refused

1. Check that server is running
2. Verify firewall settings on both computers
3. Ensure both computers are on same network
4. Try pinging the server: `ping <server-ip>`

### Input Lag

If you experience lag:

1. Ensure both computers are on a wired connection (or 5GHz WiFi)
2. Check for network congestion
3. TCP_NODELAY is already enabled for low latency

### Cursor Stuck or Not Releasing

Press **Scroll Lock** to manually toggle control back to the server.

### UAC/Admin Applications

When controlling the client, some elevated (admin) applications may not receive input. This is a Windows security feature. Solutions:

1. Run mouse-share-client.exe as Administrator
2. Use UIPI (User Interface Privilege Isolation) bypass (advanced)

## Protocol

The application uses a simple binary protocol over TCP:

| Field | Size | Description |
|-------|------|-------------|
| version | 2 bytes | Protocol version |
| type | 1 byte | Event type |
| timestamp | 4 bytes | Event timestamp |
| payload_size | 2 bytes | Size of payload |
| payload | variable | Event data |

Event types:
- `MOUSE_MOVE` (1): Cursor position/movement
- `MOUSE_BUTTON` (2): Button press/release
- `MOUSE_SCROLL` (3): Scroll wheel
- `KEY_PRESS` (4): Key press
- `KEY_RELEASE` (5): Key release
- `SCREEN_INFO` (8): Screen dimensions
- `SWITCH_SCREEN` (9): Activate client input

## How It Works

### Input Capture (Server)

The server uses Windows low-level hooks:
- `SetWindowsHookEx(WH_MOUSE_LL, ...)` - Captures all mouse events
- `SetWindowsHookEx(WH_KEYBOARD_LL, ...)` - Captures all keyboard events

When input is "captured" (active on client), the hooks return `1` to block events from reaching local applications.

### Input Simulation (Client)

The client uses `SendInput()` API to generate synthetic input events that are indistinguishable from real hardware input.

## Extending

### Adding Linux Support

See the companion `mouse-share` project which uses X11/XInput2 for Linux.

### Adding Clipboard Sharing

To share clipboard:

1. Monitor clipboard with `AddClipboardFormatListener()`
2. Serialize clipboard data
3. Add `CLIPBOARD` event type
4. On receive, use `SetClipboardData()`

### Adding Encryption

For secure networks:

1. Wrap socket with OpenSSL/Schannel
2. Add TLS handshake after TCP connect
3. All packets encrypted automatically

## Security Considerations

- **No encryption**: Traffic is unencrypted. Use only on trusted networks.
- **No authentication**: Any client can connect. Consider adding password auth.
- **Input capture**: Server captures all keyboard input including passwords.

For sensitive environments, consider:

- Running over an SSH tunnel
- Adding TLS encryption
- Implementing password authentication

## Known Limitations

1. **Single monitor**: Currently assumes single monitor per computer
2. **Single client**: Only one client can connect at a time
3. **No clipboard**: Clipboard sharing not implemented
4. **No drag-drop**: File drag-drop across screens not supported

## License

MIT License - feel free to use and modify as needed.

## Acknowledgments

Inspired by:
- [Synergy](https://symless.com/synergy)
- [Barrier](https://github.com/debauchee/barrier)
- [Input Leap](https://github.com/input-leap/input-leap)
- [ShareMouse](https://www.sharemouse.com/)
