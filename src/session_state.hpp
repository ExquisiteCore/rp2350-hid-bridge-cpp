#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

#include "rp2350_hid_bridge/c_api.h"
#include "rp2350_hid_bridge/serial.hpp"

struct Rp2350HidSession {
    std::atomic<std::uint32_t> references{1};
    std::atomic_bool faulted{false};
    rp2350_hid_bridge::detail::HidBridgeCore core;

    explicit Rp2350HidSession(rp2350_hid_bridge::HidBridgeOptions options)
        : core(std::move(options)) {}

    Rp2350HidSession(
        rp2350_hid_bridge::HidBridgeOptions options,
        std::shared_ptr<rp2350_hid_bridge::SerialTransport> transport)
        : core(std::move(options), std::move(transport)) {}
};

#if defined(RP2350_HID_BRIDGE_TESTING)
namespace rp2350_hid_bridge::detail {

inline Rp2350HidSession* make_test_session(
    HidBridgeOptions options,
    std::shared_ptr<SerialTransport> transport) {
    return new Rp2350HidSession(std::move(options), std::move(transport));
}

inline bool session_faulted(const Rp2350HidSession* session) {
    return session != nullptr && session->faulted.load(std::memory_order_acquire);
}

}  // namespace rp2350_hid_bridge::detail
#endif
