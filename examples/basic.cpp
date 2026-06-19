#include <iomanip>
#include <iostream>

#include "rp2350_hid_bridge.hpp"

int main() {
    using namespace rp2350_hid_bridge;

#ifdef _WIN32
    HidBridge hid("COM3");
    hid.open();
    hid.ping();

    auto caps = hid.caps();
    std::cout << "caps:";
    for (auto byte : caps) {
        std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    std::cout << "\n";

    // 下面会产生真实 HID 输入，使用前确认当前焦点安全。
    hid.type_text("hello from cpp sdk");
    hid.key_tap("ENTER");
    hid.mouse_move(20, 0);
    hid.stop_all();
#else
    std::cerr << "Serial client is currently implemented for Windows only.\n";
#endif

    return 0;
}
