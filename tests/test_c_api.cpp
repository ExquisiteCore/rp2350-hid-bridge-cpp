#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "rp2350_hid_bridge/c_api.h"
#include "rp2350_hid_bridge/client.hpp"
#include "rp2350_hid_bridge/port_discovery.hpp"
#include "../src/session_state.hpp"
#include "fake_transport.hpp"

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression)) {                                                                         \
            throw std::runtime_error(std::string("check failed: ") + #expression);                  \
        }                                                                                            \
    } while (false)

namespace {

using namespace rp2350_hid_bridge;
using testing::FakeTransport;
using testing::response_frame;

static_assert(sizeof(Rp2350HidAbiInfo) == 24);
static_assert(sizeof(Rp2350HidOptions) == 32);

HidBridgeOptions test_options() {
    HidBridgeOptions options;
    options.port = "COM_TEST";
    options.baud = 115200;
    options.timeout_ms = 1000;
    options.retries = 2;
    options.heartbeat_interval_ms = 60'000;
    return options;
}

std::string last_error() {
    const char* message = rp2350_hid_last_error();
    return message == nullptr ? std::string{} : std::string(message);
}

void test_abi_info() {
    CHECK(rp2350_hid_get_abi_info(nullptr) == RP2350_HID_STATUS_ERROR);
    CHECK(
        last_error().find("RP2350 protocol v2 capabilities are required") !=
        std::string::npos);
    Rp2350HidAbiInfo info{};
    info.struct_size = sizeof(info);
    CHECK(rp2350_hid_get_abi_info(&info) == RP2350_HID_STATUS_OK);
    CHECK(info.abi_major == 1);
    CHECK(info.abi_minor == 0);
    CHECK(info.options_size == sizeof(Rp2350HidOptions));
    CHECK((info.feature_flags & RP2350_HID_FEATURE_SHARED_SESSION) != 0);
    CHECK((info.feature_flags & RP2350_HID_FEATURE_PORT_DISCOVERY) != 0);

    char port[32]{};
    CHECK(
        rp2350_hid_find_port(0, 0, port, sizeof(port)) ==
        RP2350_HID_STATUS_OK);
    CHECK(port[0] == '\0');
}

void test_port_discovery_parsers() {
    CHECK(rp2350_hid_bridge::detail::matches_usb_vid_pid(
        "USB\\VID_CAFE&PID_2350\\ABC", 0xCAFE, 0x2350));
    CHECK(!rp2350_hid_bridge::detail::matches_usb_vid_pid(
        "USB\\VID_CAFE&PID_9999\\ABC", 0xCAFE, 0x2350));
    CHECK(
        rp2350_hid_bridge::detail::extract_com_port("USB Serial Device (COM4)") ==
        "COM4");
    CHECK(
        rp2350_hid_bridge::detail::extract_com_port("No serial suffix").empty());
}

void test_cpp_client_is_lazy_and_compatible() {
    HidSession session(test_options());
    CHECK(!session.is_open());
    session.close();
    session.close();

    HidBridge compatibility(test_options());
    CHECK(!compatibility.is_open());
}

void test_unopened_session_can_be_retained_and_released() {
    Rp2350HidOptions options{};
    options.struct_size = sizeof(options);
    options.port = "COM_TEST";
    options.baud = 115200;
    options.timeout_ms = 1000;
    options.retries = 2;
    options.heartbeat_interval_ms = 500;

    Rp2350HidSession* session = nullptr;
    CHECK(rp2350_hid_session_create(&options, &session) == RP2350_HID_STATUS_OK);
    CHECK(session != nullptr);
    CHECK(rp2350_hid_session_retain(session) == RP2350_HID_STATUS_OK);
    rp2350_hid_session_release(session);
    rp2350_hid_session_release(session);
}

void test_final_release_performs_one_orderly_shutdown() {
    auto transport = std::make_shared<FakeTransport>();
    transport->set_auto_ack(true);
    Rp2350HidSession* session = detail::make_test_session(test_options(), transport);

    CHECK(rp2350_hid_session_open(session) == RP2350_HID_STATUS_OK);
    std::int32_t is_open = 0;
    CHECK(rp2350_hid_session_is_open(session, &is_open) == RP2350_HID_STATUS_OK);
    CHECK(is_open == 1);
    CHECK(rp2350_hid_session_retain(session) == RP2350_HID_STATUS_OK);
    rp2350_hid_session_release(session);
    CHECK(!transport->closed());

    rp2350_hid_session_release(session);
    CHECK(transport->closed());
    CHECK(transport->dtr_history() == std::vector<bool>({true, false}));
    const auto events = transport->lifecycle_events();
    CHECK(
        std::count(events.begin(), events.end(), FakeTransport::LifecycleEvent::StopAllWritten) ==
        1);
}

void test_transport_failure_faults_session_without_automatic_retry() {
    auto transport = std::make_shared<FakeTransport>();
    transport->set_auto_ack(true);
    Rp2350HidSession* session = detail::make_test_session(test_options(), transport);
    CHECK(rp2350_hid_session_open(session) == RP2350_HID_STATUS_OK);

    transport->fail_next_write();
    CHECK(rp2350_hid_session_ping(session) == RP2350_HID_STATUS_ERROR);
    CHECK(detail::session_faulted(session));
    const int attempts_after_fault = transport->write_attempts();

    CHECK(rp2350_hid_session_ping(session) == RP2350_HID_STATUS_ERROR);
    CHECK(transport->write_attempts() == attempts_after_fault);
    CHECK(last_error().find("faulted") != std::string::npos);
    rp2350_hid_session_release(session);
}

void test_device_error_does_not_fault_session() {
    auto transport = std::make_shared<FakeTransport>();
    transport->set_scripted_responses({
        response_frame(1, CommandType::Nack, {0x07}),
    });
    Rp2350HidSession* session = detail::make_test_session(test_options(), transport);
    CHECK(rp2350_hid_session_open(session) == RP2350_HID_STATUS_OK);

    CHECK(rp2350_hid_session_ping(session) == RP2350_HID_STATUS_ERROR);
    CHECK(!detail::session_faulted(session));
    rp2350_hid_session_release(session);
}

void test_commands_forward_to_the_core() {
    auto transport = std::make_shared<FakeTransport>();
    transport->set_auto_ack(true, true);
    transport->set_scripted_responses({
        response_frame(1, CommandType::Ack),
        response_frame(2, CommandType::Status, {0x10, 0x11}),
        response_frame(3, CommandType::Status, {0x20, 0x21, 0x22}),
    });
    Rp2350HidSession* session = detail::make_test_session(test_options(), transport);
    CHECK(rp2350_hid_session_open(session) == RP2350_HID_STATUS_OK);

    std::array<std::uint8_t, 8> output{};
    std::uint32_t bytes_written = 0;
    CHECK(rp2350_hid_session_ping(session) == RP2350_HID_STATUS_OK);
    CHECK(
        rp2350_hid_session_info(session, output.data(), output.size(), &bytes_written) ==
        RP2350_HID_STATUS_OK);
    CHECK(bytes_written == 2);
    CHECK(output[0] == 0x10 && output[1] == 0x11);
    CHECK(
        rp2350_hid_session_caps(session, output.data(), output.size(), &bytes_written) ==
        RP2350_HID_STATUS_OK);
    CHECK(bytes_written == 3);
    CHECK(output[0] == 0x20 && output[1] == 0x21 && output[2] == 0x22);
    CHECK(rp2350_hid_session_type_text(session, "abc") == RP2350_HID_STATUS_OK);
    CHECK(rp2350_hid_session_key_tap(session, "ENTER") == RP2350_HID_STATUS_OK);
    CHECK(rp2350_hid_session_key_down(session, "W") == RP2350_HID_STATUS_OK);
    CHECK(rp2350_hid_session_key_up(session, "W") == RP2350_HID_STATUS_OK);
    CHECK(rp2350_hid_session_mouse_move(session, 12, -7) == RP2350_HID_STATUS_OK);
    CHECK(rp2350_hid_session_mouse_click(session, "left") == RP2350_HID_STATUS_OK);
    CHECK(rp2350_hid_session_mouse_down(session, "right") == RP2350_HID_STATUS_OK);
    CHECK(rp2350_hid_session_mouse_up(session, "right") == RP2350_HID_STATUS_OK);
    CHECK(rp2350_hid_session_mouse_wheel(session, -1) == RP2350_HID_STATUS_OK);
    CHECK(rp2350_hid_session_wait_ms(session, 1) == RP2350_HID_STATUS_OK);
    CHECK(rp2350_hid_session_stop_all(session) == RP2350_HID_STATUS_OK);
    CHECK(
        rp2350_hid_session_run_script(session, "mouse move 1 2\n") ==
        RP2350_HID_STATUS_OK);

    CHECK(transport->command_types() == std::vector<CommandType>({
        CommandType::Ping,
        CommandType::GetInfo,
        CommandType::GetCaps,
        CommandType::TypeAscii,
        CommandType::KeyTap,
        CommandType::KeyDown,
        CommandType::KeyUp,
        CommandType::MouseMoveRel,
        CommandType::MouseClick,
        CommandType::MouseButtonDown,
        CommandType::MouseButtonUp,
        CommandType::MouseWheel,
        CommandType::WaitMs,
        CommandType::StopAll,
        CommandType::BatchBegin,
        CommandType::MouseMoveRel,
        CommandType::BatchEnd,
    }));
    rp2350_hid_session_release(session);
}

void test_small_output_buffer_is_an_ordinary_error() {
    auto transport = std::make_shared<FakeTransport>();
    transport->set_scripted_responses({
        response_frame(1, CommandType::Status, {0x01, 0x02, 0x03}),
    });
    Rp2350HidSession* session = detail::make_test_session(test_options(), transport);
    CHECK(rp2350_hid_session_open(session) == RP2350_HID_STATUS_OK);

    std::array<std::uint8_t, 2> output{};
    std::uint32_t bytes_written = 0;
    CHECK(
        rp2350_hid_session_info(session, output.data(), output.size(), &bytes_written) ==
        RP2350_HID_STATUS_ERROR);
    CHECK(last_error().find("output buffer is too small") != std::string::npos);
    CHECK(!detail::session_faulted(session));
    rp2350_hid_session_release(session);
}

}  // namespace

int main() {
    try {
        test_abi_info();
        test_port_discovery_parsers();
        test_cpp_client_is_lazy_and_compatible();
        test_unopened_session_can_be_retained_and_released();
        test_final_release_performs_one_orderly_shutdown();
        test_transport_failure_faults_session_without_automatic_retry();
        test_device_error_does_not_fault_session();
        test_commands_forward_to_the_core();
        test_small_output_buffer_is_an_ordinary_error();
        std::cout << "C ABI shared HID session tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
