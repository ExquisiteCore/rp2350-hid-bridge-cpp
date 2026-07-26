#pragma once

#include <cstdint>
#include <string>

namespace rp2350_hid_bridge::detail {

[[nodiscard]] bool matches_usb_vid_pid(
    const std::string& instance_id,
    std::uint16_t vid,
    std::uint16_t pid
);

[[nodiscard]] std::string extract_com_port(const std::string& friendly_name);

[[nodiscard]] std::string find_windows_com_port(std::uint16_t vid, std::uint16_t pid);

}  // namespace rp2350_hid_bridge::detail
