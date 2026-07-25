# RP2350 HID 桥接器 C++ SDK

面向 ExquisiteCore RP2350 KeyMouse Bridge 的仅头文件 C++17 SDK。

该 SDK 实现了串口帧协议、按键解析器、脚本解析器和 Windows 串口客户端。C++ 视觉
运行时通过它向 RP2350 板卡发送相对鼠标移动和可选的按键命令。

## 环境要求

```text
C++17 compiler
CMake 3.20+
Windows for real serial-device control
```

协议和解析器头文件使用可移植的 C++17。随附的串口客户端使用 Win32 串口 API，因此
控制真实设备时需要 `COM3` 之类的 Windows COM 端口。

## 构建与测试

在本 SDK 仓库中执行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

在父项目中执行：

```powershell
cd tools\rp2350_hid_bridge_cpp
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

构建产物：

```text
build\Release\test_protocol.exe
build\Release\basic_example.exe
build\Release\script_example.exe
```

`test_protocol.exe` 不需要硬件，可以安全运行。只有明确指定 COM 端口运行示例程序时，
示例程序才会发送真实输入。

## 在 CMake 中使用

作为子目录引入：

```cmake
add_subdirectory(path/to/rp2350-hid-bridge-cpp)
target_link_libraries(your_app PRIVATE rp2350_hid_bridge)
```

作为手动包含的仅头文件 SDK：

```cmake
target_include_directories(your_app PRIVATE path/to/rp2350-hid-bridge-cpp/include)
target_compile_features(your_app PRIVATE cxx_std_17)
```

## 头文件结构

普通应用建议使用总入口头文件：

```cpp
#include "rp2350_hid_bridge.hpp"
```

也可以按功能使用以下头文件：

```cpp
#include "rp2350_hid_bridge/protocol.hpp"
#include "rp2350_hid_bridge/keys.hpp"
#include "rp2350_hid_bridge/script.hpp"
#include "rp2350_hid_bridge/serial.hpp"
```

## 直接控制 API

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

常用按键名包括字母、数字、`ENTER`、`ESC`、`TAB`、`SPACE`、`F1`-`F12`、
方向键、`HOME`、`END`、`PAGEUP`、`PAGEDOWN`、`DELETE`、`INSERT`，以及
`SLASH`、`DOT`、`COMMA`、`BACKSLASH` 等标点符号名称。

组合键使用 `+` 连接修饰键：

```text
CTRL+C
SHIFT+F5
ALT+TAB
WIN+R
```

也可以只发送修饰键而不包含普通按键。例如，`key_down("SHIFT")` 和
`key_down("CTRL+SHIFT")` 会使用零键码和指定的修饰键掩码进行编码。一个组合键最多
只能包含一个普通按键。

## 脚本 API

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

支持的脚本命令：

```text
type "ASCII text"
key tap|down|up COMBO
mouse move DX DY
mouse click|down|up left|right|middle
mouse wheel DELTA
wait MILLISECONDS
stop
```

`stop` 会先完成前面非空的批处理，再发送 `STOP_ALL`，后续命令则进入新的批处理。
只包含 `stop` 的脚本不会发送空批处理。脚本事务采用串行执行，因此普通命令无法插入
`BATCH_BEGIN` 与 `BATCH_END` 之间。

预览随附脚本但不发送输入：

```powershell
.\build\Release\script_example.exe
```

确认后实际发送：

```powershell
.\build\Release\script_example.exe --run COM3
```

## 协议辅助接口

底层辅助接口适合用于测试或自定义传输层：

```cpp
using namespace rp2350_hid_bridge;

auto frame = encode_frame(1, CommandType::Ping, {});
auto decoded = decode_frame(frame);
auto combo = parse_combo("CTRL+C");
auto commands = parse_script("key tap ENTER\n");
auto packet = script_command_to_packet(commands.front());
```

## 协议 v2 可靠性

协议 v2 只会重试响应超时和 `BUSY`。每次重试都复用完全相同的编码帧和序列号。
三字节 `BUSY` 载荷包含一个原因字节，后接采用大端序、以毫秒为单位的重试延迟，
客户端会遵守该延迟。`NACK`、串口 I/O 错误、格式错误的 `BUSY` 响应以及意外的响应
类型都会立即终止当前操作。接收流不会被清空；解析器重新同步时会跳过无效帧或过期帧。

响应截止时间包含设备执行时间。普通命令至少有一秒；等待命令包含请求的等待时长和
传输余量；文本输入与拆分后的鼠标移动使用估算的 HID 执行时长；`BATCH_END` 还包含
该批处理累计的执行时长。

`HidBridge` 支持注入 `SerialTransport`，从而无需 COM 端口或 HID 硬件即可进行
确定性测试。使用普通端口构造函数时，会自动选择随附的 `Win32SerialTransport`。

## 协议 v2 安全租约

连接打开期间，客户端每 500 毫秒串行发送一个序列号为零且带 `NO_RESPONSE` 标志的
`HEARTBEAT` 帧。心跳与其他写入共用互斥锁，并且不会读取响应。打开连接时会置位
DTR。进程、串口连接或心跳中断后，固件的两秒控制租约会释放所有保持中的输入。

## 并发与会话生命周期

所有命令和脚本共用一个递归命令互斥锁，因此同一时间只会有一个请求/响应交换，普通
命令也无法插入脚本事务。`run_script()` 会记录当前会话代次，并在每个批处理片段中
持续检查。关闭或重新打开桥接器时，旧的排队脚本会被中止，其剩余命令不会通过新连接
继续发送。

`close()` 会将桥接器标记为正在关闭、使当前会话代次失效、停止后续心跳写入、取消
正在进行的读取或 `BUSY` 延迟，并尽力抢先写入一次 `STOP_ALL`。随后它会等待心跳
工作线程结束，等待所有活动命令退出传输层，再取消 DTR 并关闭端口。该顺序可防止
停止后继续写入命令或心跳，也可避免旧会话影响后续重新打开的连接。

## 注意事项

SDK 向板卡发送命令，板卡随后生成标准 USB HID 键盘和鼠标报告。SDK 本身不知道当前
活动的是哪个应用程序，因此只有在确认活动窗口符合预期时才运行示例。
