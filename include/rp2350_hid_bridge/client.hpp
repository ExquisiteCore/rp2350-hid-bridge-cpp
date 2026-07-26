#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rp2350_hid_bridge/c_api.h"
#include "rp2350_hid_bridge/serial.hpp"

namespace rp2350_hid_bridge {

class RP2350_HID_API HidSession {
public:
    explicit HidSession(HidBridgeOptions options);
    explicit HidSession(
        std::string port,
        std::uint32_t baud = 115200,
        std::uint32_t timeout_ms = 1000,
        int retries = 2);
    ~HidSession();

    HidSession(const HidSession&) = delete;
    HidSession& operator=(const HidSession&) = delete;
    HidSession(HidSession&& other) noexcept;
    HidSession& operator=(HidSession&& other) noexcept;

    void open();
    void close() noexcept;
    [[nodiscard]] bool is_open() const;
    void ping();
    [[nodiscard]] std::vector<std::uint8_t> info();
    [[nodiscard]] std::vector<std::uint8_t> caps();
    void type_text(const std::string& text);
    void key_tap(const std::string& combo);
    void key_down(const std::string& combo);
    void key_up(const std::string& combo);
    void mouse_move(std::int16_t dx, std::int16_t dy);
    void mouse_click(const std::string& button = "left");
    void mouse_down(const std::string& button = "left");
    void mouse_up(const std::string& button = "left");
    void mouse_wheel(std::int8_t delta);
    void wait_ms(std::uint32_t milliseconds);
    void stop_all();
    void run_script(const std::string& script);

private:
    void ensure_created();
    static void check_status(std::int32_t status);

    HidBridgeOptions options_;
    Rp2350HidSession* session_ = nullptr;
};

using HidBridge = HidSession;

}  // namespace rp2350_hid_bridge
