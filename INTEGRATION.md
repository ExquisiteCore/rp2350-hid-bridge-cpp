# RP2350 HID 桥接器 C++ SDK 详细接入指南

本文说明如何在 Windows C++ 应用中接入 ExquisiteCore RP2350 KeyMouse Bridge。目标是让应用开发者只使用 SDK 的公开接口完成设备连接、键鼠控制、脚本执行、错误恢复和安全退出，不需要理解串口帧、CRC 或固件内部实现。

## 1. 适用场景与安全提示

本 SDK 适用于由 Windows 应用通过 USB CDC 串口控制 RP2350 板卡，再由板卡向操作系统发送标准 USB HID 键盘和鼠标报告的场景。

数据流如下：

```text
应用程序 → C++ SDK → CDC 串口 → RP2350 固件 → USB HID 键盘/鼠标 → 操作系统
```

板卡产生的输入会发送给当前获得焦点的窗口。SDK 不知道哪个程序处于前台，也不能替应用判断一次点击或按键是否安全。因此：

- 首次测试只运行第 5 节的连通测试；它只执行 `ping()`、`info()` 和 `caps()`，不会产生键鼠输入。
- 真实输入示例默认不执行控制，必须显式传入 `--run COMx` 才会启用。
- 测试真实输入前，先打开一个允许接收测试字符和点击的空白窗口。
- 应用退出前主动调用 `stop_all()`，随后调用 `close()`。
- 不要把 SDK 用于绕过授权、访问控制或第三方软件规则。

## 2. 工作原理与前置条件

### 硬件与固件

- Raspberry Pi Pico 2 或其他采用 RP2350、且与本固件引脚和 USB 配置兼容的板卡。
- 板卡已刷入与当前 SDK 匹配的 RP2350 KeyMouse Bridge 协议 v2 固件。
- 一根支持数据传输的 USB 线；只支持充电的线无法枚举设备。
- Windows 应同时看到固件提供的 CDC 串口和标准 HID 键盘/鼠标接口。

本文不介绍固件编译、刷写和 USB 描述符开发。相关操作请查阅固件仓库的 `README.md` 与 `docs/BUILD.md`。

### 主机开发环境

```text
Windows 10/11
C++17 编译器
CMake 3.20+
Git（推荐，用于子模块或源码依赖）
```

SDK 是仅头文件 C++17 库。协议、按键和脚本解析代码可以在其他平台编译，但 SDK 默认提供的真实串口传输层使用 Win32 API，因此直接连接板卡时需要 Windows。非 Windows 平台只有在应用自行注入 `SerialTransport` 实现时才能连接真实设备；本文不展开自定义传输层。

## 3. 构建并接入 SDK

### 先验证 SDK 本身

在 SDK 根目录执行：

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`cmake -S . -B build` 不固定生成器，CMake 会根据本机环境选择已安装的编译工具链。`test_protocol` 不需要连接板卡。

### 方式一：作为 CMake 子目录

假设应用目录如下：

```text
your_app/
├─ CMakeLists.txt
├─ src/main.cpp
└─ third_party/rp2350_hid_bridge_cpp/
```

在应用的 `CMakeLists.txt` 中加入：

```cmake
cmake_minimum_required(VERSION 3.20)
project(your_app LANGUAGES CXX)

add_subdirectory(third_party/rp2350_hid_bridge_cpp)

add_executable(your_app src/main.cpp)
target_link_libraries(your_app PRIVATE rp2350_hid_bridge)
```

SDK 的 CMake 目标会传递头文件路径、C++17 要求和线程依赖。构建应用：

```powershell
cmake -S . -B build
cmake --build build --config Release
```

如果 SDK 作为 Git 子模块保存，可以使用：

```powershell
git submodule add https://github.com/ExquisiteCore/rp2350-hid-bridge-cpp.git third_party/rp2350_hid_bridge_cpp
git submodule update --init --recursive
```

### 方式二：手动引用头文件

不使用 SDK 自带 CMake 目标时，显式添加头文件目录、C++17 和线程依赖：

```cmake
find_package(Threads REQUIRED)

add_executable(your_app src/main.cpp)
target_include_directories(
    your_app
    PRIVATE third_party/rp2350_hid_bridge_cpp/include
)
target_compile_features(your_app PRIVATE cxx_std_17)
target_link_libraries(your_app PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(your_app PRIVATE /utf-8)
endif()
```

应用代码通常只需要总入口头文件：

```cpp
#include "rp2350_hid_bridge.hpp"
```

### `HidBridgeOptions`

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---:|---|
| `port` | `std::string` | 空 | 必填的 Windows 串口名，例如 `COM3` |
| `baud` | `std::uint32_t` | `115200` | CDC 串口波特率；通常保持默认值 |
| `timeout_ms` | `std::uint32_t` | `1000` | 基础串口超时，单位为毫秒 |
| `retries` | `int` | `2` | 响应超时或固件 `BUSY` 后的重试次数；不能为负数 |
| `heartbeat_interval_ms` | `std::uint32_t` | `500` | 心跳发送间隔，单位为毫秒；必须大于零 |

普通命令的实际响应截止时间至少为一秒。长等待、长文本、大幅鼠标移动和脚本批处理会根据预计执行时间自动延长截止时间，所以不要为了这些正常操作随意设置很大的基础超时。

## 4. 查找并确认 COM 端口

### 使用设备管理器

1. 连接板卡。
2. 打开“设备管理器”。
3. 展开“端口（COM 和 LPT）”。
4. 记录桥接器对应的端口，例如 `COM3`。

### 使用 PowerShell

```powershell
Get-CimInstance Win32_SerialPort |
    Select-Object DeviceID, Name, PNPDeviceID
```

拔下板卡后再次执行命令，可以通过消失的条目确认对应端口。应用中直接传入 `COM3` 即可；SDK 会自动转换为 Win32 可打开的完整设备路径，所以 `COM10` 等两位数端口也不需要手动添加 `\\.\` 前缀。

如果没有看到串口：

- 更换确认支持数据传输的 USB 线和 USB 端口。
- 检查板卡是否确实运行桥接器固件，而不是仍处于 BOOTSEL 模式。
- 在设备管理器中检查未知设备或带警告图标的设备。
- 确认没有另一个程序独占该 COM 口。

## 5. 最小连通测试

下面的程序只查询设备，不发送键盘或鼠标报告。将其保存为 `bridge_probe.cpp`：

```cpp
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#include "rp2350_hid_bridge.hpp"

int main(int argc, char** argv) {
#ifdef _WIN32
    if (argc != 2) {
        std::cerr << "usage: bridge_probe COMx\n";
        return 2;
    }

    try {
        rp2350_hid_bridge::HidBridgeOptions options;
        options.port = argv[1];
        options.timeout_ms = 1000;
        options.retries = 2;

        rp2350_hid_bridge::HidBridge hid(options);
        hid.open();
        hid.ping();

        const auto print_bytes = [](const char* name, const auto& bytes) {
            std::cout << name << ':';
            for (const auto byte : bytes) {
                std::cout << ' ' << std::hex << std::setw(2)
                          << std::setfill('0') << static_cast<int>(byte);
            }
            std::cout << std::dec << '\n';
        };

        print_bytes("info", hid.info());
        print_bytes("caps", hid.caps());
        hid.close();
        return 0;
    } catch (const rp2350_hid_bridge::TimeoutError& exc) {
        std::cerr << "device timeout: " << exc.what() << '\n';
    } catch (const std::invalid_argument& exc) {
        std::cerr << "invalid option: " << exc.what() << '\n';
    } catch (const std::runtime_error& exc) {
        std::cerr << "bridge error: " << exc.what() << '\n';
    }
    return 1;
#else
    std::cerr << "the default serial transport currently requires Windows\n";
    return 3;
#endif
}
```

假设 SDK 位于 `third_party/rp2350_hid_bridge_cpp`，在源码旁创建：

```cmake
cmake_minimum_required(VERSION 3.20)
project(bridge_probe LANGUAGES CXX)

add_subdirectory(third_party/rp2350_hid_bridge_cpp)
add_executable(bridge_probe bridge_probe.cpp)
target_link_libraries(bridge_probe PRIVATE rp2350_hid_bridge)
```

构建并运行：

```powershell
cmake -S . -B build
cmake --build build --config Release
.\build\Release\bridge_probe.exe COM3
```

某些单配置生成器会把程序直接放在 `build` 目录，此时运行 `.\build\bridge_probe.exe COM3`。

成功时会打印 `info:` 和 `caps:` 后的十六进制字节。`ping()`、`info()` 和 `caps()` 只查询桥接器状态，不产生 HID 输入；它们适合作为安装后的首次检查和应用启动时的健康检查。

## 6. 直接控制 API

连接的基本生命周期是：构造对象、`open()`、发送命令、`stop_all()`、`close()`。

| 方法 | 作用 | 关键约束 |
|---|---|---|
| `open()` | 打开串口、置位 DTR 并启动心跳 | 重复调用已打开对象时直接返回 |
| `ping()` | 检查协议端点是否响应 | 不产生 HID 输入 |
| `info()` | 读取固件信息载荷 | 返回 `std::vector<std::uint8_t>` |
| `caps()` | 读取能力载荷 | 返回 `std::vector<std::uint8_t>` |
| `type_text(text)` | 输入 ASCII 文本 | 仅支持美式键盘 ASCII，不支持 Unicode |
| `key_tap(combo)` | 按下并释放组合键 | 例如 `CTRL+C`、`ENTER` |
| `key_down(combo)` | 保持组合键 | 退出前必须释放或调用 `stop_all()` |
| `key_up(combo)` | 释放组合键 | 组合键格式与 `key_down()` 相同 |
| `mouse_move(dx, dy)` | 相对移动鼠标 | 两个参数均为有符号 16 位整数 |
| `mouse_click(button)` | 点击鼠标按钮 | `left`、`right` 或 `middle` |
| `mouse_down(button)` | 保持鼠标按钮 | 退出前必须释放或调用 `stop_all()` |
| `mouse_up(button)` | 释放鼠标按钮 | 与 `mouse_down()` 配对 |
| `mouse_wheel(delta)` | 滚动滚轮 | 有符号 8 位整数，即 `-128..127` |
| `wait_ms(milliseconds)` | 让设备等待 | 非负 32 位毫秒数 |
| `stop_all()` | 释放所有保持中的键和鼠标按钮 | 成功、失败和退出路径都应尝试调用 |
| `run_script(text)` | 串行执行脚本 | 详见第 7 节 |
| `close()` | 停止会话并关闭端口 | `noexcept`，会尽力发送 `STOP_ALL` |

按键名不区分大小写，常用名称包括：

```text
A-Z  0-9  ENTER  ESC  TAB  SPACE  BACKSPACE
F1-F12  LEFT  RIGHT  UP  DOWN
HOME  END  PAGEUP  PAGEDOWN  DELETE  INSERT
SLASH  DOT  COMMA  BACKSLASH
```

组合键用 `+` 连接修饰键：

```text
CTRL+C
SHIFT+F5
ALT+TAB
WIN+R
```

一个组合键最多包含一个普通按键，但可以包含多个修饰键。`SHIFT`、`CTRL+SHIFT` 这类只有修饰键的组合也有效。要同时保持多个普通按键，分别调用多次 `key_down()`，结束时分别 `key_up()`，并始终保留 `stop_all()` 兜底。

下面的调用会产生真实输入：

```cpp
hid.type_text("hello");
hid.key_tap("ENTER");
hid.key_down("CTRL");
hid.key_up("CTRL");
hid.mouse_move(10, -5);
hid.mouse_click("left");
hid.mouse_down("right");
hid.mouse_up("right");
hid.mouse_wheel(-1);
hid.wait_ms(100);
hid.stop_all();
```

应用应在调用这些方法前自行确认运行模式和活动窗口。

## 7. 脚本批处理接入

`run_script()` 适合把一组必须按顺序执行的操作一次性交给 SDK：

```cpp
const std::string script =
    "type \"hello from script\"\n"
    "key tap ENTER\n"
    "mouse move 20 0\n"
    "mouse click left\n"
    "wait 100\n"
    "stop\n";

hid.run_script(script);
```

支持的语法：

| 命令 | 参数 | 示例 |
|---|---|---|
| `type` | 双引号包围的 ASCII 文本 | `type "hello"` |
| `key tap` | 一个组合键 | `key tap CTRL+C` |
| `key down` | 一个组合键 | `key down SHIFT` |
| `key up` | 一个组合键 | `key up SHIFT` |
| `mouse move` | `DX DY` | `mouse move 20 -10` |
| `mouse click` | 按钮名 | `mouse click left` |
| `mouse down` | 按钮名 | `mouse down right` |
| `mouse up` | 按钮名 | `mouse up right` |
| `mouse wheel` | `-128..127` | `mouse wheel -1` |
| `wait` | 非负毫秒数 | `wait 100` |
| `stop` | 无参数 | `stop` |

空行会被忽略。脚本中的参数在发送前解析；格式错误会抛出 `std::invalid_argument`。

脚本事务与普通命令共用命令锁。一个脚本片段执行期间，其他线程的普通命令不能插入批处理。`stop` 会先执行它前面的非空片段，再发送 `STOP_ALL`；后续命令会进入新的片段。只包含 `stop` 的脚本不会发送空批处理。

如果脚本执行失败，SDK 会尽力发送一次 `STOP_ALL`，同时保留原始异常。上层仍应在自己的清理路径中再次尽力调用 `stop_all()`，因为故障可能发生在串口已断开的时刻。

不要在响应是否已到达不明确时自行无限重放整段脚本。SDK 已对协议允许的超时和 `BUSY` 做有限重试；最终失败后应关闭旧会话、检查设备状态，再由明确的业务策略决定是否从头提交一项新任务。

## 8. 完整应用接入模板

以下程序默认不连接设备，也不发送输入。只有参数严格为 `--run COMx` 时才会启用真实 HID 控制：

```cpp
#include <iostream>
#include <stdexcept>
#include <string>

#include "rp2350_hid_bridge.hpp"

namespace {

void run_input(const std::string& port) {
    rp2350_hid_bridge::HidBridgeOptions options;
    options.port = port;
    options.timeout_ms = 1000;
    options.retries = 2;
    options.heartbeat_interval_ms = 500;

    rp2350_hid_bridge::HidBridge hid(options);
    hid.open();
    try {
        std::cout << "warning: real HID input is enabled; verify the active window\n";
        hid.mouse_move(20, 0);
        hid.mouse_click("left");
        hid.key_tap("ENTER");
        hid.type_text("hello from rp2350");
        hid.wait_ms(100);
        hid.stop_all();
        hid.close();
    } catch (...) {
        try {
            hid.stop_all();
        } catch (...) {
        }
        hid.close();
        throw;
    }
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    if (argc != 3 || std::string(argv[1]) != "--run") {
        std::cout << "未启用真实输入。确认活动窗口安全后，使用 --run COMx。\n";
        return 0;
    }

    try {
        run_input(argv[2]);
        return 0;
    } catch (const rp2350_hid_bridge::TimeoutError& exc) {
        std::cerr << "设备响应超时：" << exc.what() << '\n';
    } catch (const std::invalid_argument& exc) {
        std::cerr << "参数错误：" << exc.what() << '\n';
    } catch (const std::runtime_error& exc) {
        std::cerr << "桥接器错误：" << exc.what() << '\n';
    }
    return 1;
#else
    std::cerr << "默认串口传输当前只支持 Windows。\n";
    return 3;
#endif
}
```

未带参数运行时：

```powershell
.\build\Release\your_app.exe
```

程序会直接退出，不构造 `HidBridge`。确认活动窗口安全后才运行：

```powershell
.\build\Release\your_app.exe --run COM3
```

这里的 `--run` 是显式授权开关，不是“试运行”参数。业务应用可以换成配置项、管理员确认或 UI 二次确认，但默认状态必须保持为不发送输入。

## 9. 错误处理与恢复

捕获异常时应先捕获 `TimeoutError`，因为它继承自 `std::runtime_error`。

| 场景 | 异常 | 建议处理 |
|---|---|---|
| COM 口不存在、被占用或打开失败 | `std::runtime_error` | 检查端口号，关闭串口工具或另一个客户端后重新打开 |
| 设备最终没有匹配响应 | `TimeoutError` | 记录操作和端口，关闭旧会话，检查 USB/固件后再决定是否重连 |
| 固件持续返回 `BUSY` | `std::runtime_error` | 等待当前操作结束；确认没有第二个应用同时控制设备 |
| 固件返回 `NACK` | `std::runtime_error` | 根据异常中的错误名和编号修正命令、参数或 SDK/固件版本 |
| 组合键、按钮、文本或数值无效 | `std::invalid_argument` | 修正输入；这类错误通常在写串口前产生 |
| 串口读取、写入或 DTR 操作失败 | `std::runtime_error` | 视为当前会话不可用，关闭后检查连接 |
| 对象未打开、正在关闭或会话已更换 | `std::runtime_error` | 停止旧任务，只向新会话重新提交明确的新任务 |

推荐恢复顺序：

1. 记录失败的方法、参数范围、端口和异常文本，不记录敏感业务文本。
2. 尽力调用 `stop_all()`；它失败时不要覆盖原始异常。
3. 调用 `close()` 使旧会话失效。
4. 检查 USB 枚举、COM 口占用、固件版本和设备电源状态。
5. 重新运行只读连通测试。
6. 只有业务逻辑能确认重复执行安全时，才建立新会话并重新提交操作。

SDK 只对响应超时和 `BUSY` 进行配置次数内的重试。`NACK`、串口 I/O 错误、格式错误的响应和意外响应类型会立即终止。不要在上层增加无上限循环，否则可能在不知道上一项操作是否已执行时重复产生输入。

## 10. 心跳、DTR、并发与会话生命周期

### 心跳和控制租约

`open()` 成功后，SDK 默认每 500 毫秒发送一次心跳。固件要求持续心跳来维持两秒控制租约；进程停止、串口断开或心跳中断后，租约到期会释放保持中的键和鼠标按钮。

打开连接时 SDK 置位 DTR，正常关闭时取消 DTR。DTR 或 USB 连接丢失也会触发固件侧的安全重置。一般不应把 `heartbeat_interval_ms` 调得接近或超过两秒租约；默认的 500 毫秒为调度延迟保留了余量。

### 并发规则

- 同一个 `HidBridge` 的普通命令和脚本事务通过递归命令互斥锁串行执行。
- 心跳与命令共享写入互斥锁，不会与命令字节交叉写入。
- 不要为同一个 COM 口创建两个客户端；Windows 串口独占和固件 `BUSY` 都可能导致失败。
- 可以从多个线程提交命令，但需要由应用定义顺序。对顺序敏感的一组操作应使用单一工作队列或 `run_script()`。
- 对象析构开始后，其他线程不得继续访问该对象；应用必须保证调用线程先结束，再销毁 `HidBridge`。

### 会话代次

每次成功 `open()` 都创建新的会话代次。`close()` 或重新打开会使旧代次失效。排队中的旧脚本会检查代次并中止，剩余命令不会通过新连接继续发送；旧响应也不能满足新会话中的请求。

`close()` 的主要顺序是：

1. 标记对象正在关闭，并使当前会话代次失效。
2. 停止后续心跳并取消正在进行的读取或 `BUSY` 等待。
3. 在传输层仍可用时尽力写入 `STOP_ALL`。
4. 等待心跳线程和活动命令退出。
5. 取消 DTR 并关闭串口。

这个顺序防止旧会话在关闭后继续发送命令，也防止旧脚本进入重新打开后的连接。

## 11. 正常退出与异常退出

正常路径应显式执行：

```cpp
hid.stop_all();
hid.close();
```

异常路径应保留原始异常，同时尽力清理：

```cpp
try {
    // 真实输入操作
} catch (...) {
    try {
        hid.stop_all();
    } catch (...) {
    }
    hid.close();
    throw;
}
```

`HidBridge` 析构函数会调用 `close()`，所以正常栈展开还有 RAII 兜底。但析构和 `close()` 不是进程崩溃、强制结束、系统掉电或 USB 瞬断时的绝对保证。在这些情况下，最终保护来自固件的两秒控制租约以及 DTR/USB 断开触发的安全重置。

对于服务或长期运行程序，还应：

- 在停止工作线程后再销毁桥接器对象。
- 在可控的服务停止、窗口关闭和取消操作路径中统一执行清理。
- 不在信号处理器或异常终止回调中执行复杂的串口逻辑。
- 重连时创建清晰的新任务边界，不延续旧会话中尚未确认的脚本。

## 12. 常见问题

### `open()` 报告无法打开串口

端口名可能错误，或者串口被串口监视器、另一个 SDK 客户端或旧进程占用。重新核对设备管理器中的 COM 口，并关闭其他客户端。

### 能看到 HID 键鼠，但看不到 COM 口

检查数据线、设备管理器和固件版本。当前 SDK 必须通过固件的 CDC 命令端点通信，仅有 HID 枚举还不够。

### `ping()` 成功，但没有键鼠动作

确认运行的是带 `--run COMx` 的真实输入路径，当前窗口可以接收输入，并且 SDK 与固件版本匹配。先测试小幅鼠标移动或在空白文本编辑器中测试一个安全字符。

### 中文或其他 Unicode 文本无法输入

`type_text()` 和脚本 `type` 使用美式键盘 ASCII 映射，不是 Unicode 文本注入。非 ASCII 输入需要由应用根据目标键盘布局拆成受支持的按键操作；SDK 不提供布局无关的文本输入。

### 经常出现 `BUSY`

确认没有第二个客户端，同一应用也没有反复创建多个桥接器实例。长脚本执行期间的其他命令会被 SDK 串行化；跨进程竞争则无法由单个客户端锁解决。

### 超时后是否可以直接重发

SDK 已在协议允许的范围内重试。最终 `TimeoutError` 表示执行状态可能未知，不应盲目重发会产生副作用的命令。先关闭会话、检查设备并运行只读连通测试。

### 退出后按键仍像被按住

正常退出路径必须调用 `stop_all()`。如果进程被强制终止，等待两秒控制租约到期；仍未恢复时断开板卡 USB，并检查固件是否为当前版本。

## 13. 生产接入检查清单

发布应用前逐项确认：

- [ ] 固件与 SDK 来自相互匹配的仓库版本。
- [ ] 安装文档明确支持的 Windows、编译器和 CMake 版本。
- [ ] 启动时先用 `ping()`、`info()` 和 `caps()` 完成只读健康检查。
- [ ] COM 口可配置，并在错误信息中显示实际端口。
- [ ] 默认运行模式不产生 HID 输入，真实输入需要显式授权。
- [ ] UI 或命令行在启用输入前提示活动窗口风险。
- [ ] 所有组合键、文本、移动量、滚轮量和等待时间在业务层验证范围。
- [ ] 顺序敏感操作使用单一队列或 `run_script()`。
- [ ] 同一设备同一时间只有一个客户端。
- [ ] 分别处理 `TimeoutError`、`std::invalid_argument` 和 `std::runtime_error`。
- [ ] 最终失败后不进行无上限或无条件重试。
- [ ] 成功、失败、取消和正常退出路径都尽力调用 `stop_all()`。
- [ ] 工作线程退出后才调用 `close()` 并销毁对象。
- [ ] 已实际验证两秒租约、USB 拔出和应用异常退出后的输入释放行为。
- [ ] 日志足以定位端口、操作类别和异常，但不会泄露敏感输入文本。

完成以上检查后，再把真实输入功能交付给最终用户或上层业务模块。
