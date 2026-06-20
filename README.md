# RP2350 HID Bridge C++ SDK

Header-only C++17 SDK for the ExquisiteCore RP2350 KeyMouse Bridge.

The SDK implements the serial frame protocol, key parser, script parser, and a
Windows serial client. It is used by the C++ vision runtime to send relative
mouse movement and optional button commands to the RP2350 board.

## Requirements

```text
C++17 compiler
CMake 3.20+
Windows for real serial-device control
```

The protocol and parser headers are portable C++17. The bundled serial client
uses the Win32 serial API, so real device control expects a Windows COM port
such as `COM3`.

## Build And Test

From this repository:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\Release\test_protocol.exe
```

From the parent project:

```powershell
cd tools\rp2350_hid_bridge_cpp
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\Release\test_protocol.exe
```

Build outputs:

```text
build\Release\test_protocol.exe
build\Release\basic_example.exe
build\Release\script_example.exe
```

`test_protocol.exe` is safe and does not require hardware. Example executables
only send real input when explicitly run against a COM port.

## Use In CMake

As a subdirectory:

```cmake
add_subdirectory(path/to/rp2350-hid-bridge-cpp)
target_link_libraries(your_app PRIVATE rp2350_hid_bridge)
```

As a manually included header-only SDK:

```cmake
target_include_directories(your_app PRIVATE path/to/rp2350-hid-bridge-cpp/include)
target_compile_features(your_app PRIVATE cxx_std_17)
```

## Header Layout

Use the umbrella header for normal applications:

```cpp
#include "rp2350_hid_bridge.hpp"
```

Focused headers:

```cpp
#include "rp2350_hid_bridge/protocol.hpp"
#include "rp2350_hid_bridge/keys.hpp"
#include "rp2350_hid_bridge/script.hpp"
#include "rp2350_hid_bridge/serial.hpp"
```

## Direct Control API

```cpp
#include "rp2350_hid_bridge.hpp"

int main() {
    rp2350_hid_bridge::HidBridge hid("COM3");
    hid.open();

    hid.ping();
    hid.type_text("hello");
    hid.key_tap("ENTER");
    hid.mouse_move(10, -5);
    hid.mouse_click("left");
    hid.wait_ms(100);
    hid.stop_all();
}
```

Common key names include letters, digits, `ENTER`, `ESC`, `TAB`, `SPACE`,
`F1`-`F12`, arrows, `HOME`, `END`, `PAGEUP`, `PAGEDOWN`, `DELETE`, `INSERT`,
and punctuation names such as `SLASH`, `DOT`, `COMMA`, `BACKSLASH`.

Modifiers are combined with `+`:

```text
CTRL+C
SHIFT+F5
ALT+TAB
WIN+R
```

## Script API

```cpp
const char* script =
    "type \"hello from script\"\n"
    "key tap ENTER\n"
    "mouse move 20 0\n"
    "mouse click left\n"
    "wait 100\n"
    "stop\n";

rp2350_hid_bridge::HidBridge hid("COM3");
hid.open();
hid.run_script(script);
```

Supported script commands:

```text
type "ASCII text"
key tap|down|up COMBO
mouse move DX DY
mouse click|down|up left|right|middle
mouse wheel DELTA
wait MILLISECONDS
stop
```

Preview the bundled script without sending input:

```powershell
.\build\Release\script_example.exe
```

Send it intentionally:

```powershell
.\build\Release\script_example.exe --run COM3
```

## Protocol Helpers

The lower-level helpers are useful for tests or custom transports:

```cpp
using namespace rp2350_hid_bridge;

auto frame = encode_frame(1, CommandType::Ping, {});
auto decoded = decode_frame(frame);
auto combo = parse_combo("CTRL+C");
auto commands = parse_script("key tap ENTER\n");
auto packet = script_command_to_packet(commands.front());
```

## Notes

The SDK sends commands to the board. The board then emits standard USB HID
keyboard and mouse reports. The SDK itself does not know which application is
active, so run examples only when the active window is expected.
