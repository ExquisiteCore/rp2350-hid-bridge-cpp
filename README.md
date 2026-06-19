# RP2350 HID Bridge C++ SDK

Header-only C++17 SDK for the ExquisiteCore RP2350 KeyMouse Bridge.

The protocol, key parser, and script parser are portable C++17. The serial client is currently implemented with the Windows Win32 serial API, so real device control expects a COM port such as `COM3`.

## Header Layout

Use the umbrella header for normal applications:

```cpp
#include "rp2350_hid_bridge.hpp"
```

Focused headers are also available:

```cpp
#include "rp2350_hid_bridge/protocol.hpp"
#include "rp2350_hid_bridge/keys.hpp"
#include "rp2350_hid_bridge/script.hpp"
#include "rp2350_hid_bridge/serial.hpp"
```

## Use In CMake

```cmake
add_subdirectory(sdk/cpp)
target_link_libraries(your_app PRIVATE rp2350_hid_bridge)
```

Build SDK tests and examples:

```powershell
cmake -S sdk/cpp -B sdk/cpp/build
cmake --build sdk/cpp/build --config Debug
sdk\cpp\build\Debug\test_protocol.exe
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

Common key names include letters, digits, `ENTER`, `ESC`, `TAB`, `SPACE`, `F1`-`F12`, arrows, `HOME`, `END`, `PAGEUP`, `PAGEDOWN`, `DELETE`, `INSERT`, and punctuation names such as `SLASH`, `DOT`, `COMMA`, `BACKSLASH`.

Modifiers are combined with `+`: `CTRL+C`, `SHIFT+F5`, `ALT+TAB`, `WIN+R`.

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

Supported commands:

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
sdk\cpp\build\Debug\script_example.exe
```

Send it intentionally on Windows:

```powershell
sdk\cpp\build\Debug\script_example.exe --run COM3
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

The examples produce real keyboard and mouse input only when explicitly run against a device. Make sure the active window is safe before sending commands.
