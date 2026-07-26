#include "rp2350_hid_bridge/c_api.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "rp2350_hid_bridge/port_discovery.hpp"
#include "session_state.hpp"

namespace {

thread_local std::string last_error;

template <typename Function>
std::int32_t call_api(Function&& function) noexcept {
    try {
        last_error.clear();
        function();
        return RP2350_HID_STATUS_OK;
    } catch (const rp2350_hid_bridge::TimeoutError& error) {
        last_error = error.what();
        return RP2350_HID_STATUS_TIMEOUT;
    } catch (const std::exception& error) {
        last_error = error.what();
        return RP2350_HID_STATUS_ERROR;
    } catch (...) {
        last_error = "unknown RP2350 HID error";
        return RP2350_HID_STATUS_ERROR;
    }
}

Rp2350HidSession& require_session(Rp2350HidSession* session) {
    if (session == nullptr) {
        throw std::runtime_error("HID session handle is null");
    }
    return *session;
}

const char* require_text(const char* value, const char* name) {
    if (value == nullptr) {
        throw std::invalid_argument(std::string(name) + " is null");
    }
    return value;
}

rp2350_hid_bridge::HidBridgeOptions parse_options(const Rp2350HidOptions* options) {
    if (options == nullptr) {
        throw std::invalid_argument("HID options are null");
    }
    if (options->struct_size != sizeof(Rp2350HidOptions)) {
        throw std::invalid_argument("HID options struct_size is incompatible");
    }
    if (options->port == nullptr || options->port[0] == '\0') {
        throw std::invalid_argument("HID port must not be empty");
    }
    if (options->baud == 0) {
        throw std::invalid_argument("HID baud must be positive");
    }
    if (options->timeout_ms == 0) {
        throw std::invalid_argument("HID timeout must be positive");
    }
    if (options->retries < 0) {
        throw std::invalid_argument("HID retries must not be negative");
    }
    if (options->heartbeat_interval_ms == 0) {
        throw std::invalid_argument("HID heartbeat interval must be positive");
    }

    rp2350_hid_bridge::HidBridgeOptions result;
    result.port = options->port;
    result.baud = options->baud;
    result.timeout_ms = options->timeout_ms;
    result.retries = options->retries;
    result.heartbeat_interval_ms = options->heartbeat_interval_ms;
    return result;
}

template <typename Function>
std::int32_t call_session(Rp2350HidSession* session, Function&& function) noexcept {
    return call_api([&] {
        Rp2350HidSession& value = require_session(session);
        if (value.faulted.load(std::memory_order_acquire)) {
            throw std::runtime_error("HID session is faulted");
        }
        try {
            function(value.core);
        } catch (const rp2350_hid_bridge::TimeoutError&) {
            value.faulted.store(true, std::memory_order_release);
            value.core.close();
            throw;
        } catch (const rp2350_hid_bridge::TransportError&) {
            value.faulted.store(true, std::memory_order_release);
            value.core.close();
            throw;
        }
    });
}

void copy_payload(
    const std::vector<std::uint8_t>& payload,
    std::uint8_t* output,
    std::uint32_t output_size,
    std::uint32_t* bytes_written) {
    if (bytes_written == nullptr) {
        throw std::invalid_argument("bytes_written is null");
    }
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("HID response is too large");
    }
    *bytes_written = static_cast<std::uint32_t>(payload.size());
    if (output_size < payload.size()) {
        throw std::runtime_error("output buffer is too small");
    }
    if (!payload.empty() && output == nullptr) {
        throw std::invalid_argument("output buffer is null");
    }
    if (!payload.empty()) {
        std::copy(payload.begin(), payload.end(), output);
    }
}

}  // namespace

extern "C" {

std::int32_t rp2350_hid_get_abi_info(Rp2350HidAbiInfo* info) {
    return call_api([&] {
        if (info == nullptr) {
            throw std::invalid_argument("ABI info is null");
        }
        if (info->struct_size != sizeof(Rp2350HidAbiInfo)) {
            throw std::invalid_argument("ABI info struct_size is incompatible");
        }
        info->abi_major = RP2350_HID_ABI_MAJOR;
        info->abi_minor = RP2350_HID_ABI_MINOR;
        info->options_size = sizeof(Rp2350HidOptions);
        info->feature_flags =
            RP2350_HID_FEATURE_SHARED_SESSION | RP2350_HID_FEATURE_PORT_DISCOVERY;
    });
}

const char* rp2350_hid_last_error(void) {
    return last_error.c_str();
}

std::int32_t rp2350_hid_find_port(
    std::uint16_t vid,
    std::uint16_t pid,
    char* output,
    std::uint32_t output_size) {
    std::int32_t result = RP2350_HID_STATUS_OK;
    const std::int32_t status = call_api([&] {
        const std::string port = rp2350_hid_bridge::detail::find_windows_com_port(vid, pid);
        if (port.empty()) {
            return;
        }
        if (output == nullptr) {
            throw std::invalid_argument("port output buffer is null");
        }
        if (output_size < port.size() + 1) {
            throw std::runtime_error("port output buffer is too small");
        }
        std::memcpy(output, port.c_str(), port.size() + 1);
        result = RP2350_HID_STATUS_FOUND;
    });
    return status == RP2350_HID_STATUS_OK ? result : status;
}

std::int32_t rp2350_hid_session_create(
    const Rp2350HidOptions* options,
    Rp2350HidSession** session) {
    return call_api([&] {
        if (session == nullptr) {
            throw std::invalid_argument("HID session output is null");
        }
        *session = nullptr;
        *session = new Rp2350HidSession(parse_options(options));
    });
}

std::int32_t rp2350_hid_session_retain(Rp2350HidSession* session) {
    return call_api([&] {
        Rp2350HidSession& value = require_session(session);
        value.references.fetch_add(1, std::memory_order_relaxed);
    });
}

void rp2350_hid_session_release(Rp2350HidSession* session) {
    if (session == nullptr) {
        return;
    }
    if (session->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete session;
    }
}

std::int32_t rp2350_hid_session_open(Rp2350HidSession* session) {
    return call_session(session, [](auto& core) { core.open(); });
}

std::int32_t rp2350_hid_session_is_open(
    Rp2350HidSession* session,
    std::int32_t* is_open) {
    return call_session(session, [&](auto& core) {
        if (is_open == nullptr) {
            throw std::invalid_argument("is_open output is null");
        }
        *is_open = core.is_open() ? 1 : 0;
    });
}

std::int32_t rp2350_hid_session_ping(Rp2350HidSession* session) {
    return call_session(session, [](auto& core) { core.ping(); });
}

std::int32_t rp2350_hid_session_info(
    Rp2350HidSession* session,
    std::uint8_t* output,
    std::uint32_t output_size,
    std::uint32_t* bytes_written) {
    return call_session(session, [&](auto& core) {
        if (bytes_written == nullptr) {
            throw std::invalid_argument("bytes_written is null");
        }
        const auto payload = core.info();
        copy_payload(payload, output, output_size, bytes_written);
    });
}

std::int32_t rp2350_hid_session_caps(
    Rp2350HidSession* session,
    std::uint8_t* output,
    std::uint32_t output_size,
    std::uint32_t* bytes_written) {
    return call_session(session, [&](auto& core) {
        if (bytes_written == nullptr) {
            throw std::invalid_argument("bytes_written is null");
        }
        const auto payload = core.caps();
        copy_payload(payload, output, output_size, bytes_written);
    });
}

std::int32_t rp2350_hid_session_type_text(
    Rp2350HidSession* session,
    const char* text) {
    return call_session(session, [&](auto& core) {
        core.type_text(require_text(text, "text"));
    });
}

std::int32_t rp2350_hid_session_key_tap(
    Rp2350HidSession* session,
    const char* combo) {
    return call_session(session, [&](auto& core) {
        core.key_tap(require_text(combo, "combo"));
    });
}

std::int32_t rp2350_hid_session_key_down(
    Rp2350HidSession* session,
    const char* combo) {
    return call_session(session, [&](auto& core) {
        core.key_down(require_text(combo, "combo"));
    });
}

std::int32_t rp2350_hid_session_key_up(
    Rp2350HidSession* session,
    const char* combo) {
    return call_session(session, [&](auto& core) {
        core.key_up(require_text(combo, "combo"));
    });
}

std::int32_t rp2350_hid_session_mouse_move(
    Rp2350HidSession* session,
    std::int16_t dx,
    std::int16_t dy) {
    return call_session(session, [&](auto& core) { core.mouse_move(dx, dy); });
}

std::int32_t rp2350_hid_session_mouse_click(
    Rp2350HidSession* session,
    const char* button) {
    return call_session(session, [&](auto& core) {
        core.mouse_click(require_text(button, "button"));
    });
}

std::int32_t rp2350_hid_session_mouse_down(
    Rp2350HidSession* session,
    const char* button) {
    return call_session(session, [&](auto& core) {
        core.mouse_down(require_text(button, "button"));
    });
}

std::int32_t rp2350_hid_session_mouse_up(
    Rp2350HidSession* session,
    const char* button) {
    return call_session(session, [&](auto& core) {
        core.mouse_up(require_text(button, "button"));
    });
}

std::int32_t rp2350_hid_session_mouse_wheel(
    Rp2350HidSession* session,
    std::int8_t delta) {
    return call_session(session, [&](auto& core) { core.mouse_wheel(delta); });
}

std::int32_t rp2350_hid_session_wait_ms(
    Rp2350HidSession* session,
    std::uint32_t milliseconds) {
    return call_session(session, [&](auto& core) { core.wait_ms(milliseconds); });
}

std::int32_t rp2350_hid_session_stop_all(Rp2350HidSession* session) {
    return call_session(session, [](auto& core) { core.stop_all(); });
}

std::int32_t rp2350_hid_session_run_script(
    Rp2350HidSession* session,
    const char* script) {
    return call_session(session, [&](auto& core) {
        core.run_script(require_text(script, "script"));
    });
}

}  // extern "C"
