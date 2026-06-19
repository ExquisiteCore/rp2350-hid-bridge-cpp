#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

#include "rp2350_hid_bridge.hpp"

namespace {

const char* kScript =
    "type \"hello from ExquisiteCore\"\n"
    "key tap ENTER\n"
    "mouse move 20 0\n"
    "wait 100\n"
    "stop\n";

void print_packet(const rp2350_hid_bridge::CommandPacket& packet) {
    std::cout << "type=0x" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<int>(packet.command_type) << " payload=";
    for (auto byte : packet.payload) {
        std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(byte);
    }
    std::cout << std::dec << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    using namespace rp2350_hid_bridge;

#ifdef _WIN32
    if (argc == 3 && std::string(argv[1]) == "--run") {
        HidBridge hid(argv[2]);
        hid.open();
        hid.run_script(kScript);
        return 0;
    }
#endif

    for (const auto& command : parse_script(kScript)) {
        print_packet(script_command_to_packet(command));
    }
    std::cout << "\nUse --run COMx to send real HID input on Windows.\n";
    return 0;
}
