#include "rp2350_hid_bridge/client.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rp2350_hid_bridge {
namespace {

HidBridgeOptions make_options(
    std::string port,
    std::uint32_t baud,
    std::uint32_t timeout_ms,
    int retries) {
    HidBridgeOptions options;
    options.port = std::move(port);
    options.baud = baud;
    options.timeout_ms = timeout_ms;
    options.retries = retries;
    return options;
}

}  // namespace

HidSession::HidSession(HidBridgeOptions options) : options_(std::move(options)) {}

HidSession::HidSession(
    std::string port,
    std::uint32_t baud,
    std::uint32_t timeout_ms,
    int retries)
    : HidSession(make_options(std::move(port), baud, timeout_ms, retries)) {}

HidSession::~HidSession() {
    close();
}

HidSession::HidSession(HidSession&& other) noexcept
    : options_(std::move(other.options_)), session_(std::exchange(other.session_, nullptr)) {}

HidSession& HidSession::operator=(HidSession&& other) noexcept {
    if (this != &other) {
        close();
        options_ = std::move(other.options_);
        session_ = std::exchange(other.session_, nullptr);
    }
    return *this;
}

void HidSession::ensure_created() {
    if (session_ != nullptr) {
        return;
    }
    Rp2350HidOptions options{};
    options.struct_size = sizeof(options);
    options.port = options_.port.c_str();
    options.baud = options_.baud;
    options.timeout_ms = options_.timeout_ms;
    options.retries = options_.retries;
    options.heartbeat_interval_ms = options_.heartbeat_interval_ms;

    Rp2350HidSession* candidate = nullptr;
    const std::int32_t status = rp2350_hid_session_create(&options, &candidate);
    if (status != RP2350_HID_STATUS_OK) {
        if (candidate != nullptr) {
            rp2350_hid_session_release(candidate);
        }
        check_status(status);
    }
    session_ = candidate;
}

void HidSession::check_status(std::int32_t status) {
    if (status == RP2350_HID_STATUS_OK) {
        return;
    }
    const char* message = rp2350_hid_last_error();
    const std::string error =
        message == nullptr || message[0] == '\0' ? "RP2350 HID command failed" : message;
    if (status == RP2350_HID_STATUS_TIMEOUT) {
        throw TimeoutError(error);
    }
    throw std::runtime_error(error);
}

void HidSession::open() {
    ensure_created();
    const std::int32_t status = rp2350_hid_session_open(session_);
    if (status != RP2350_HID_STATUS_OK) {
        rp2350_hid_session_release(session_);
        session_ = nullptr;
        check_status(status);
    }
}

void HidSession::close() noexcept {
    if (session_ != nullptr) {
        rp2350_hid_session_release(session_);
        session_ = nullptr;
    }
}

bool HidSession::is_open() const {
    if (session_ == nullptr) {
        return false;
    }
    std::int32_t value = 0;
    check_status(rp2350_hid_session_is_open(session_, &value));
    return value != 0;
}

void HidSession::ping() {
    ensure_created();
    check_status(rp2350_hid_session_ping(session_));
}

std::vector<std::uint8_t> HidSession::info() {
    ensure_created();
    std::array<std::uint8_t, 256> output{};
    std::uint32_t bytes_written = 0;
    check_status(rp2350_hid_session_info(
        session_,
        output.data(),
        static_cast<std::uint32_t>(output.size()),
        &bytes_written));
    return {output.begin(), output.begin() + bytes_written};
}

std::vector<std::uint8_t> HidSession::caps() {
    ensure_created();
    std::array<std::uint8_t, 256> output{};
    std::uint32_t bytes_written = 0;
    check_status(rp2350_hid_session_caps(
        session_,
        output.data(),
        static_cast<std::uint32_t>(output.size()),
        &bytes_written));
    return {output.begin(), output.begin() + bytes_written};
}

void HidSession::type_text(const std::string& text) {
    ensure_created();
    check_status(rp2350_hid_session_type_text(session_, text.c_str()));
}

void HidSession::key_tap(const std::string& combo) {
    ensure_created();
    check_status(rp2350_hid_session_key_tap(session_, combo.c_str()));
}

void HidSession::key_down(const std::string& combo) {
    ensure_created();
    check_status(rp2350_hid_session_key_down(session_, combo.c_str()));
}

void HidSession::key_up(const std::string& combo) {
    ensure_created();
    check_status(rp2350_hid_session_key_up(session_, combo.c_str()));
}

void HidSession::mouse_move(std::int16_t dx, std::int16_t dy) {
    ensure_created();
    check_status(rp2350_hid_session_mouse_move(session_, dx, dy));
}

void HidSession::mouse_click(const std::string& button) {
    ensure_created();
    check_status(rp2350_hid_session_mouse_click(session_, button.c_str()));
}

void HidSession::mouse_down(const std::string& button) {
    ensure_created();
    check_status(rp2350_hid_session_mouse_down(session_, button.c_str()));
}

void HidSession::mouse_up(const std::string& button) {
    ensure_created();
    check_status(rp2350_hid_session_mouse_up(session_, button.c_str()));
}

void HidSession::mouse_wheel(std::int8_t delta) {
    ensure_created();
    check_status(rp2350_hid_session_mouse_wheel(session_, delta));
}

void HidSession::wait_ms(std::uint32_t milliseconds) {
    ensure_created();
    check_status(rp2350_hid_session_wait_ms(session_, milliseconds));
}

void HidSession::stop_all() {
    ensure_created();
    check_status(rp2350_hid_session_stop_all(session_));
}

void HidSession::run_script(const std::string& script) {
    ensure_created();
    check_status(rp2350_hid_session_run_script(session_, script.c_str()));
}

}  // namespace rp2350_hid_bridge
