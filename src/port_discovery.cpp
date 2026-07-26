#include "rp2350_hid_bridge/port_discovery.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ntddser.h>
#include <setupapi.h>
#endif

namespace rp2350_hid_bridge::detail {
namespace {

std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

std::string usb_token(const char* prefix, std::uint16_t value) {
    std::array<char, 9> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s%04X", prefix, value);
    return buffer.data();
}

#ifdef _WIN32
class DeviceInfoSet {
public:
    explicit DeviceInfoSet(HDEVINFO value) : value_(value) {}
    ~DeviceInfoSet() {
        if (value_ != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(value_);
        }
    }

    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;

    [[nodiscard]] HDEVINFO get() const { return value_; }

private:
    HDEVINFO value_;
};

std::string get_instance_id(HDEVINFO devices, SP_DEVINFO_DATA& device) {
    DWORD required = 0;
    SetupDiGetDeviceInstanceIdA(devices, &device, nullptr, 0, &required);
    if (required == 0) {
        throw std::runtime_error("SetupDiGetDeviceInstanceId size query failed");
    }
    std::vector<char> buffer(required);
    if (!SetupDiGetDeviceInstanceIdA(
            devices,
            &device,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            nullptr)) {
        throw std::runtime_error("SetupDiGetDeviceInstanceId failed");
    }
    return buffer.data();
}

std::string get_friendly_name(HDEVINFO devices, SP_DEVINFO_DATA& device) {
    DWORD property_type = 0;
    DWORD required = 0;
    SetupDiGetDeviceRegistryPropertyA(
        devices,
        &device,
        SPDRP_FRIENDLYNAME,
        &property_type,
        nullptr,
        0,
        &required);
    if (required == 0) {
        return {};
    }
    std::vector<BYTE> buffer(required);
    if (!SetupDiGetDeviceRegistryPropertyA(
            devices,
            &device,
            SPDRP_FRIENDLYNAME,
            &property_type,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            nullptr)) {
        return {};
    }
    return reinterpret_cast<const char*>(buffer.data());
}
#endif

}  // namespace

bool matches_usb_vid_pid(
    const std::string& instance_id,
    std::uint16_t vid,
    std::uint16_t pid) {
    const std::string value = uppercase(instance_id);
    return value.find(usb_token("VID_", vid)) != std::string::npos &&
           value.find(usb_token("PID_", pid)) != std::string::npos;
}

std::string extract_com_port(const std::string& friendly_name) {
    if (friendly_name.empty() || friendly_name.back() != ')') {
        return {};
    }
    const std::size_t opening = friendly_name.rfind('(');
    if (opening == std::string::npos || opening + 5 >= friendly_name.size()) {
        return {};
    }
    const std::string candidate =
        uppercase(friendly_name.substr(opening + 1, friendly_name.size() - opening - 2));
    if (candidate.rfind("COM", 0) != 0 || candidate.size() == 3) {
        return {};
    }
    if (!std::all_of(candidate.begin() + 3, candidate.end(), [](unsigned char character) {
            return std::isdigit(character) != 0;
        })) {
        return {};
    }
    return candidate;
}

std::string find_windows_com_port(std::uint16_t vid, std::uint16_t pid) {
#ifdef _WIN32
    DeviceInfoSet devices(SetupDiGetClassDevsA(
        &GUID_DEVINTERFACE_COMPORT,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (devices.get() == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("SetupDiGetClassDevs for COM ports failed");
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(devices.get(), index, &device)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                return {};
            }
            throw std::runtime_error("SetupDiEnumDeviceInfo failed");
        }

        const std::string instance_id = get_instance_id(devices.get(), device);
        if (!matches_usb_vid_pid(instance_id, vid, pid)) {
            continue;
        }
        const std::string port = extract_com_port(get_friendly_name(devices.get(), device));
        if (!port.empty()) {
            return port;
        }
    }
#else
    (void)vid;
    (void)pid;
    return {};
#endif
}

}  // namespace rp2350_hid_bridge::detail
