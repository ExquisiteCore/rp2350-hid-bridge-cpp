#include <iomanip>
#include <iostream>
#include <string_view>

#include "rp2350_hid_bridge.hpp"
#include "rp2350_hid_bridge/c_api.h"

int main(int argc, char** argv) {
    using namespace rp2350_hid_bridge;

    if (argc == 2 && std::string_view(argv[1]) == "--abi-smoke") {
        Rp2350HidAbiInfo info{};
        info.struct_size = sizeof(info);
        return rp2350_hid_get_abi_info(&info) == RP2350_HID_STATUS_OK &&
                       info.abi_major == RP2350_HID_ABI_MAJOR
                   ? 0
                   : 1;
    }

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
