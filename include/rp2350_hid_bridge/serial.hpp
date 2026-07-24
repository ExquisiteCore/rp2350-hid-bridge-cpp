#pragma once

#include "rp2350_hid_bridge/keys.hpp"
#include "rp2350_hid_bridge/protocol.hpp"
#include "rp2350_hid_bridge/script.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rp2350_hid_bridge {

class TimeoutError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Injectable byte transport used by HidBridge. Tests can provide a fake;
/// production Windows callers use Win32SerialTransport.
class SerialTransport {
public:
    virtual ~SerialTransport() = default;
    virtual void open(const std::string& port, std::uint32_t baud, std::uint32_t timeout_ms) = 0;
    virtual void set_dtr(bool asserted) = 0;
    virtual void write(const std::vector<std::uint8_t>& bytes) = 0;
    virtual std::vector<std::uint8_t> read(std::size_t max_bytes, std::uint32_t timeout_ms) = 0;
    virtual void cancel_read() noexcept = 0;
    virtual void close() noexcept = 0;
};

#ifdef _WIN32
class Win32SerialTransport final : public SerialTransport {
public:
    ~Win32SerialTransport() override { close(); }

    void open(const std::string& port, std::uint32_t baud, std::uint32_t timeout_ms) override {
        close();
        const auto full_name = port.rfind("\\\\.\\", 0) == 0 ? port : "\\\\.\\" + port;
        handle_ = CreateFileA(
            full_name.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("failed to open serial port " + port);
        }

        try {
            DCB dcb{};
            dcb.DCBlength = sizeof(dcb);
            if (!GetCommState(handle_, &dcb)) {
                throw std::runtime_error("GetCommState failed");
            }
            dcb.BaudRate = baud;
            dcb.ByteSize = 8;
            dcb.Parity = NOPARITY;
            dcb.StopBits = ONESTOPBIT;
            if (!SetCommState(handle_, &dcb)) {
                throw std::runtime_error("SetCommState failed");
            }
            set_timeouts(timeout_ms);
        } catch (...) {
            close();
            throw;
        }
    }

    void set_dtr(bool asserted) override {
        require_open();
        if (!EscapeCommFunction(handle_, asserted ? SETDTR : CLRDTR)) {
            throw std::runtime_error("serial DTR control failed");
        }
    }

    void write(const std::vector<std::uint8_t>& bytes) override {
        require_open();
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD written = 0;
            const auto remaining = static_cast<DWORD>(bytes.size() - offset);
            if (!WriteFile(handle_, bytes.data() + offset, remaining, &written, nullptr) || written == 0) {
                throw std::runtime_error("serial write failed");
            }
            offset += written;
        }
    }

    std::vector<std::uint8_t> read(std::size_t max_bytes, std::uint32_t timeout_ms) override {
        require_open();
        set_timeouts(timeout_ms);
        std::vector<std::uint8_t> bytes(max_bytes);
        DWORD count = 0;
        if (!ReadFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) {
            const auto error = GetLastError();
            if (error == ERROR_OPERATION_ABORTED) {
                return {};
            }
            throw std::runtime_error("serial read failed");
        }
        bytes.resize(count);
        return bytes;
    }

    void cancel_read() noexcept override {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CancelIoEx(handle_, nullptr);
        }
    }

    void close() noexcept override {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    void require_open() const {
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("serial port is not open");
        }
    }

    void set_timeouts(std::uint32_t timeout_ms) {
        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = 20;
        timeouts.ReadTotalTimeoutConstant = timeout_ms;
        timeouts.WriteTotalTimeoutConstant = timeout_ms;
        if (!SetCommTimeouts(handle_, &timeouts)) {
            throw std::runtime_error("SetCommTimeouts failed");
        }
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#endif

struct HidBridgeOptions {
    std::string port;
    std::uint32_t baud = 115200;
    std::uint32_t timeout_ms = 1000;
    int retries = 2;
    std::uint32_t heartbeat_interval_ms = 500;
};

class HidBridge {
public:
    explicit HidBridge(
        HidBridgeOptions options,
        std::shared_ptr<SerialTransport> transport = nullptr)
        : options_(std::move(options)), transport_(std::move(transport)) {
        if (options_.retries < 0) {
            throw std::invalid_argument("retries must not be negative");
        }
        if (options_.heartbeat_interval_ms == 0) {
            throw std::invalid_argument("heartbeat interval must be positive");
        }
        if (!transport_) {
#ifdef _WIN32
            transport_ = std::make_shared<Win32SerialTransport>();
#else
            throw std::runtime_error("the default serial transport is available only on Windows");
#endif
        }
    }

    explicit HidBridge(
        std::string port,
        std::uint32_t baud = 115200,
        std::uint32_t timeout_ms = 1000,
        int retries = 2)
        : HidBridge(make_options(std::move(port), baud, timeout_ms, retries)) {}

    HidBridge(const HidBridge&) = delete;
    HidBridge& operator=(const HidBridge&) = delete;

    ~HidBridge() { close(); }

    void open() {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        if (opened_) {
            return;
        }
        if (closing_) {
            throw std::runtime_error("serial port is closing");
        }
        if (heartbeat_thread_.joinable()) {
            throw std::runtime_error("previous heartbeat worker is still stopping");
        }

        transport_->open(options_.port, options_.baud, options_.timeout_ms);
        try {
            transport_->set_dtr(true);
        } catch (...) {
            transport_->close();
            throw;
        }

        ++generation_;
        opened_ = true;
        closing_ = false;
        session_ = std::make_shared<SessionData>();
        {
            std::lock_guard<std::mutex> heartbeat_lock(heartbeat_mutex_);
            heartbeat_stop_ = false;
        }
        try {
            const auto generation = generation_;
            heartbeat_thread_ = std::thread([this, generation] { heartbeat_loop(generation); });
        } catch (...) {
            opened_ = false;
            session_.reset();
            try {
                transport_->set_dtr(false);
            } catch (...) {
            }
            transport_->close();
            throw;
        }
    }

    void close() noexcept {
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            if (!opened_ || closing_) {
                return;
            }
            closing_ = true;
            opened_ = false;
            ++generation_;
            session_.reset();
        }

        {
            std::lock_guard<std::mutex> heartbeat_lock(heartbeat_mutex_);
            heartbeat_stop_ = true;
        }
        heartbeat_changed_.notify_all();
        transport_->cancel_read();

        {
            std::lock_guard<std::mutex> write_lock(write_mutex_);
            try {
                transport_->write(encode_frame(next_sequence(), CommandType::StopAll));
            } catch (...) {
            }
            try {
                transport_->set_dtr(false);
            } catch (...) {
            }
            transport_->close();
        }

        if (heartbeat_thread_.joinable() && heartbeat_thread_.get_id() != std::this_thread::get_id()) {
            heartbeat_thread_.join();
        }
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            closing_ = false;
        }
    }

    Response send_command(CommandType command, const std::vector<std::uint8_t>& payload = {}) {
        return send_command_for_generation(command, payload, current_generation());
    }

    void ping() { (void)send_command(CommandType::Ping); }
    std::vector<std::uint8_t> info() { return send_command(CommandType::GetInfo).payload; }
    std::vector<std::uint8_t> caps() { return send_command(CommandType::GetCaps).payload; }
    void type_text(const std::string& text) { (void)send_command(CommandType::TypeAscii, ascii_payload(text)); }
    void key_tap(const std::string& combo) { (void)send_command(CommandType::KeyTap, key_payload(parse_combo(combo))); }
    void key_down(const std::string& combo) { (void)send_command(CommandType::KeyDown, key_payload(parse_combo(combo))); }
    void key_up(const std::string& combo) { (void)send_command(CommandType::KeyUp, key_payload(parse_combo(combo))); }
    void mouse_move(std::int16_t dx, std::int16_t dy) {
        (void)send_command(CommandType::MouseMoveRel, i16_pair_payload(dx, dy));
    }
    void mouse_click(const std::string& button = "left") {
        (void)send_command(CommandType::MouseClick, {mouse_button_mask(button)});
    }
    void mouse_down(const std::string& button = "left") {
        (void)send_command(CommandType::MouseButtonDown, {mouse_button_mask(button)});
    }
    void mouse_up(const std::string& button = "left") {
        (void)send_command(CommandType::MouseButtonUp, {mouse_button_mask(button)});
    }
    void mouse_wheel(std::int8_t delta) {
        (void)send_command(CommandType::MouseWheel, {static_cast<std::uint8_t>(delta)});
    }
    void wait_ms(std::uint32_t milliseconds) {
        (void)send_command(CommandType::WaitMs, u32_payload(milliseconds));
    }
    void stop_all() { (void)send_command(CommandType::StopAll); }

    void run_script(const std::string& script) {
        const auto generation = current_generation();
        std::lock_guard<std::recursive_mutex> transaction_lock(command_mutex_);
        ensure_generation(generation);
        const auto commands = parse_script(script);
        try {
            std::vector<ScriptCommand> segment;
            for (const auto& command : commands) {
                if (command.kind == ScriptKind::Stop) {
                    execute_script_batch(segment, generation);
                    segment.clear();
                    (void)send_command_for_generation(CommandType::StopAll, {}, generation);
                } else {
                    segment.push_back(command);
                }
            }
            execute_script_batch(segment, generation);
        } catch (...) {
            try {
                (void)send_command_for_generation(CommandType::StopAll, {}, generation);
            } catch (...) {
            }
            throw;
        }
    }

private:
    struct SessionData {
        std::vector<std::uint8_t> receive_buffer;
        std::optional<std::uint64_t> batch_duration_ms;
    };

    static HidBridgeOptions make_options(
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

    std::uint64_t current_generation() const {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        return generation_;
    }

    std::shared_ptr<SessionData> require_session(std::uint64_t generation) const {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        if (generation != generation_ || !opened_ || closing_ || !session_) {
            throw std::runtime_error("serial session changed or is closing");
        }
        return session_;
    }

    void ensure_generation(std::uint64_t generation) const {
        (void)require_session(generation);
    }

    Response send_command_for_generation(
        CommandType command,
        const std::vector<std::uint8_t>& payload,
        std::uint64_t generation) {
        std::lock_guard<std::recursive_mutex> transaction_lock(command_mutex_);
        auto session = require_session(generation);
        const auto sequence = next_sequence();
        const auto frame = encode_frame(sequence, command, payload);
        const auto timeout_ms = response_timeout(command, payload, *session);

        for (int attempt = 0; attempt <= options_.retries; ++attempt) {
            write_frame(frame, generation);
            Response response;
            try {
                response = read_response(sequence, timeout_ms, generation, *session);
            } catch (const TimeoutError&) {
                if (attempt >= options_.retries) {
                    throw;
                }
                continue;
            }

            if (response.command_type == CommandType::Busy) {
                const auto delay_ms = busy_delay_ms(response.payload);
                if (attempt >= options_.retries) {
                    throw std::runtime_error("device remained BUSY");
                }
                wait_for_retry(delay_ms, generation);
                continue;
            }
            if (response.command_type == CommandType::Nack) {
                throw nack_error(response.payload);
            }
            const auto expected = expected_response_type(command);
            if (response.command_type != expected) {
                throw std::runtime_error("unexpected response type");
            }

            ensure_generation(generation);
            record_completed_command(command, payload, *session);
            return response;
        }
        throw std::runtime_error("command failed");
    }

    void execute_script_batch(
        const std::vector<ScriptCommand>& commands,
        std::uint64_t generation) {
        if (commands.empty()) {
            return;
        }
        (void)send_command_for_generation(CommandType::BatchBegin, {}, generation);
        for (const auto& command : commands) {
            const auto packet = script_command_to_packet(command);
            (void)send_command_for_generation(packet.command_type, packet.payload, generation);
        }
        (void)send_command_for_generation(CommandType::BatchEnd, {}, generation);
    }

    std::uint16_t next_sequence() {
        std::lock_guard<std::mutex> sequence_lock(sequence_mutex_);
        const auto current = sequence_;
        sequence_ = static_cast<std::uint16_t>(sequence_ + 1);
        if (sequence_ == 0) {
            sequence_ = 1;
        }
        return current;
    }

    void write_frame(const std::vector<std::uint8_t>& frame, std::uint64_t generation) {
        std::lock_guard<std::mutex> write_lock(write_mutex_);
        ensure_generation(generation);
        transport_->write(frame);
    }

    Response read_response(
        std::uint16_t expected_sequence,
        std::uint32_t timeout_ms,
        std::uint64_t generation,
        SessionData& session) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (true) {
            ensure_generation(generation);
            if (auto response = try_decode_response(session.receive_buffer, expected_sequence)) {
                return *response;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                throw TimeoutError("timed out waiting for response");
            }
            const auto remaining = deadline - now;
            auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
            if (remaining_ms < remaining) {
                ++remaining_ms;
            }
            auto chunk = transport_->read(64, static_cast<std::uint32_t>(remaining_ms.count()));
            if (!chunk.empty()) {
                ensure_generation(generation);
                session.receive_buffer.insert(
                    session.receive_buffer.end(), chunk.begin(), chunk.end());
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    static std::optional<Response> try_decode_response(
        std::vector<std::uint8_t>& buffer,
        std::uint16_t expected_sequence) {
        while (buffer.size() >= 2) {
            if (buffer[0] != MAGIC0 || buffer[1] != MAGIC1) {
                buffer.erase(buffer.begin());
                continue;
            }
            if (buffer.size() < 9) {
                return std::nullopt;
            }
            const auto payload_length = read_u16(buffer, 7);
            if (payload_length > MAX_PAYLOAD_SIZE) {
                buffer.erase(buffer.begin());
                continue;
            }
            const auto frame_length = FRAME_OVERHEAD + payload_length;
            if (buffer.size() < frame_length) {
                return std::nullopt;
            }
            std::vector<std::uint8_t> frame_bytes(
                buffer.begin(),
                buffer.begin() + static_cast<std::ptrdiff_t>(frame_length));
            Frame frame;
            try {
                frame = decode_frame(frame_bytes);
            } catch (const DecodeError&) {
                buffer.erase(buffer.begin());
                continue;
            }
            buffer.erase(
                buffer.begin(),
                buffer.begin() + static_cast<std::ptrdiff_t>(frame_length));
            if (frame.sequence != expected_sequence) {
                continue;
            }
            Response response;
            response.command_type = frame.command_type;
            response.payload = std::move(frame.payload);
            response.sequence = frame.sequence;
            response.version = frame.version;
            response.flags = frame.flags;
            return response;
        }
        return std::nullopt;
    }

    static std::uint32_t busy_delay_ms(const std::vector<std::uint8_t>& payload) {
        if (payload.size() != 3) {
            throw std::runtime_error("malformed BUSY payload");
        }
        return static_cast<std::uint32_t>((payload[1] << 8) | payload[2]);
    }

    static std::runtime_error nack_error(const std::vector<std::uint8_t>& payload) {
        const auto code = payload.empty() ? 0 : payload[0];
        std::string name = "unknown error";
        switch (code) {
            case 1: name = "bad frame"; break;
            case 2: name = "bad command"; break;
            case 3: name = "unsupported ASCII"; break;
            case 7: name = "unsupported version"; break;
            case 8: name = "unsupported flags"; break;
            case 9: name = "invalid sequence"; break;
            case 10: name = "sequence conflict"; break;
            case 11: name = "invalid batch state"; break;
            case 12: name = "batch capacity exceeded"; break;
            case 13: name = "too many keys"; break;
            case 14: name = "wait too long"; break;
            case 15: name = "keyboard busy"; break;
            case 16: name = "cancelled"; break;
            default: break;
        }
        return std::runtime_error(
            "device returned NACK " + name + " (error code " + std::to_string(code) + ")");
    }

    void wait_for_retry(std::uint32_t delay_ms, std::uint64_t generation) const {
        auto remaining = delay_ms;
        while (remaining > 0) {
            ensure_generation(generation);
            const auto slice = std::min<std::uint32_t>(remaining, 50);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            remaining -= slice;
        }
        ensure_generation(generation);
    }

    static std::optional<std::uint64_t> known_duration_ms(
        CommandType command,
        const std::vector<std::uint8_t>& payload) {
        if (command == CommandType::WaitMs && payload.size() == 4) {
            return (static_cast<std::uint64_t>(payload[0]) << 24) |
                (static_cast<std::uint64_t>(payload[1]) << 16) |
                (static_cast<std::uint64_t>(payload[2]) << 8) |
                static_cast<std::uint64_t>(payload[3]);
        }
        if (command == CommandType::TypeAscii) {
            return payload.size() * 8;
        }
        if (command == CommandType::MouseMoveRel && payload.size() == 4) {
            const auto dx = static_cast<std::int16_t>(
                static_cast<std::uint16_t>((payload[0] << 8) | payload[1]));
            const auto dy = static_cast<std::int16_t>(
                static_cast<std::uint16_t>((payload[2] << 8) | payload[3]));
            const auto extent = std::max(
                std::abs(static_cast<int>(dx)),
                std::abs(static_cast<int>(dy)));
            return static_cast<std::uint64_t>((extent + 126) / 127);
        }
        if (command == CommandType::KeyTap) {
            return 8;
        }
        if (command == CommandType::MouseClick) {
            return 20;
        }
        return std::nullopt;
    }

    std::uint32_t response_timeout(
        CommandType command,
        const std::vector<std::uint8_t>& payload,
        const SessionData& session) const {
        std::uint64_t result = options_.timeout_ms;
        if (command == CommandType::BatchEnd && session.batch_duration_ms) {
            result = std::max(result, *session.batch_duration_ms + 1000);
        } else if (const auto duration = known_duration_ms(command, payload)) {
            result = std::max(result, *duration + 500);
        } else {
            result = std::max<std::uint64_t>(result, 1000);
        }
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(result, UINT32_MAX));
    }

    static void record_completed_command(
        CommandType command,
        const std::vector<std::uint8_t>& payload,
        SessionData& session) {
        if (command == CommandType::BatchBegin) {
            session.batch_duration_ms = 0;
            return;
        }
        if (command == CommandType::BatchEnd || command == CommandType::StopAll) {
            session.batch_duration_ms.reset();
            return;
        }
        if (session.batch_duration_ms) {
            if (const auto duration = known_duration_ms(command, payload)) {
                *session.batch_duration_ms += *duration;
            }
        }
    }

    void heartbeat_loop(std::uint64_t generation) noexcept {
        const auto heartbeat = encode_frame(0, CommandType::Heartbeat, {}, FLAG_NO_RESPONSE);
        std::unique_lock<std::mutex> heartbeat_lock(heartbeat_mutex_);
        while (!heartbeat_changed_.wait_for(
            heartbeat_lock,
            std::chrono::milliseconds(options_.heartbeat_interval_ms),
            [this] { return heartbeat_stop_; })) {
            heartbeat_lock.unlock();
            try {
                write_frame(heartbeat, generation);
            } catch (...) {
            }
            heartbeat_lock.lock();
        }
    }

    HidBridgeOptions options_;
    std::shared_ptr<SerialTransport> transport_;

    mutable std::mutex state_mutex_;
    bool opened_ = false;
    bool closing_ = false;
    std::uint64_t generation_ = 0;
    std::shared_ptr<SessionData> session_;

    std::recursive_mutex command_mutex_;
    std::mutex write_mutex_;
    std::mutex sequence_mutex_;
    std::uint16_t sequence_ = 1;

    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_changed_;
    bool heartbeat_stop_ = true;
    std::thread heartbeat_thread_;
};

}  // namespace rp2350_hid_bridge
