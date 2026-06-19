#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "rp2350_hid_bridge.hpp"

int main() {
    using namespace rp2350_hid_bridge;

    auto expect_invalid_argument = [](auto&& fn) {
        bool rejected = false;
        try {
            fn();
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
    };

    auto frame = encode_frame(0x1234, CommandType::Ping, {});
    auto decoded = decode_frame(frame);
    assert(decoded.version == 1);
    assert(decoded.sequence == 0x1234);
    assert(decoded.command_type == CommandType::Ping);
    assert(decoded.payload.empty());

    auto bad_crc = frame;
    bad_crc.back() ^= 0x55;
    bool rejected = false;
    try {
        (void)decode_frame(bad_crc);
    } catch (const DecodeError&) {
        rejected = true;
    }
    assert(rejected);

    auto ctrl_c = parse_combo("CTRL+C");
    assert(ctrl_c.modifier == 0x01);
    assert(ctrl_c.keycode == 0x06);
    auto enter = parse_combo("ENTER");
    assert(enter.modifier == 0x00);
    assert(enter.keycode == 0x28);
    auto f5 = parse_combo("F5");
    assert(f5.modifier == 0x00);
    assert(f5.keycode == 0x3E);
    assert(parse_combo("[").keycode == 0x2F);
    assert(parse_combo("BACKSLASH").keycode == 0x31);
    assert(parse_combo("CAPSLOCK").keycode == 0x39);
    assert(parse_combo("PRINTSCREEN").keycode == 0x46);

    auto commands = parse_script(
        "type \"abc\"\n"
        "key tap ENTER\n"
        "mouse move 10 -5\n"
        "wait 100\n"
        "stop\n");
    assert(commands.size() == 5);
    assert(commands[0].kind == ScriptKind::Type);
    assert(commands[0].text == "abc");
    assert(commands[1].kind == ScriptKind::Key);
    assert(commands[1].key_action == KeyAction::Tap);
    assert(commands[2].kind == ScriptKind::Mouse);
    assert(commands[2].mouse_action == MouseAction::Move);
    assert(commands[2].dx == 10);
    assert(commands[2].dy == -5);
    assert(commands[3].kind == ScriptKind::Wait);
    assert(commands[3].ms == 100);
    assert(commands[4].kind == ScriptKind::Stop);

    auto type_packet = script_command_to_packet(commands[0]);
    assert(type_packet.command_type == CommandType::TypeAscii);
    assert(type_packet.payload == std::vector<std::uint8_t>({'a', 'b', 'c'}));
    auto key_packet = script_command_to_packet(commands[1]);
    assert(key_packet.command_type == CommandType::KeyTap);
    assert(key_packet.payload == std::vector<std::uint8_t>({0x00, 0x28}));
    auto move_packet = script_command_to_packet(commands[2]);
    assert(move_packet.command_type == CommandType::MouseMoveRel);
    assert(move_packet.payload == std::vector<std::uint8_t>({0x00, 0x0A, 0xFF, 0xFB}));
    auto wait_packet = script_command_to_packet(commands[3]);
    assert(wait_packet.command_type == CommandType::WaitMs);
    assert(wait_packet.payload == std::vector<std::uint8_t>({0x00, 0x00, 0x00, 0x64}));

    expect_invalid_argument([] { (void)parse_script("key press ENTER\n"); });
    expect_invalid_argument([] { (void)parse_script("mouse move 40000 0\n"); });
    expect_invalid_argument([] { (void)parse_script("mouse move 0 -40000\n"); });
    expect_invalid_argument([] { (void)parse_script("mouse wheel 200\n"); });
    expect_invalid_argument([] { (void)parse_script("wait -1\n"); });
    expect_invalid_argument([] { (void)parse_script("wait 100ms\n"); });

    std::cout << "C++ SDK protocol tests passed\n";
    return 0;
}
