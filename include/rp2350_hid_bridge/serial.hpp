#pragma once

#include "rp2350_hid_bridge/keys.hpp"
#include "rp2350_hid_bridge/protocol.hpp"
#include "rp2350_hid_bridge/script.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
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

#ifdef _WIN32
/// Windows serial client for the RP2350 CDC command interface.
class HidBridge {
public:
    explicit HidBridge(std::string port, DWORD baud = CBR_115200, DWORD timeout_ms = 1000, int retries = 2)
        : port_name_(std::move(port)), baud_(baud), timeout_ms_(timeout_ms), retries_(retries) {}

    ~HidBridge() { close(); }

    void open() {
        auto full_name = port_name_.rfind("\\\\.\\", 0) == 0 ? port_name_ : "\\\\.\\" + port_name_;
        handle_ = CreateFileA(full_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("failed to open serial port " + port_name_);
        }

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle_, &dcb)) throw std::runtime_error("GetCommState failed");
        dcb.BaudRate = baud_;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        if (!SetCommState(handle_, &dcb)) throw std::runtime_error("SetCommState failed");

        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = 20;
        timeouts.ReadTotalTimeoutConstant = timeout_ms_;
        timeouts.WriteTotalTimeoutConstant = timeout_ms_;
        if (!SetCommTimeouts(handle_, &timeouts)) throw std::runtime_error("SetCommTimeouts failed");
    }

    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    Response send_command(CommandType command, const std::vector<std::uint8_t>& payload = {}) {
        auto sequence = next_sequence();
        auto frame = encode_frame(sequence, command, payload);
        std::exception_ptr last_error;
        for (int attempt = 0; attempt <= retries_; ++attempt) {
            try {
                PurgeComm(handle_, PURGE_RXCLEAR);
                write_all(frame);
                auto response = read_response(sequence);
                if (response.command_type == CommandType::Busy && attempt < retries_) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                if (response.command_type == CommandType::Nack) {
                    throw std::runtime_error("device returned NACK");
                }
                if (response.command_type != expected_response_type(command)) {
                    throw std::runtime_error("unexpected response type");
                }
                return response;
            } catch (...) {
                last_error = std::current_exception();
                if (attempt >= retries_) std::rethrow_exception(last_error);
            }
        }
        std::rethrow_exception(last_error);
    }

    void ping() { send_command(CommandType::Ping); }
    std::vector<std::uint8_t> info() { return send_command(CommandType::GetInfo).payload; }
    std::vector<std::uint8_t> caps() { return send_command(CommandType::GetCaps).payload; }
    void type_text(const std::string& text) { send_command(CommandType::TypeAscii, ascii_payload(text)); }
    void key_tap(const std::string& combo) { send_command(CommandType::KeyTap, key_payload(parse_combo(combo))); }
    void key_down(const std::string& combo) { send_command(CommandType::KeyDown, key_payload(parse_combo(combo))); }
    void key_up(const std::string& combo) { send_command(CommandType::KeyUp, key_payload(parse_combo(combo))); }
    void mouse_move(std::int16_t dx, std::int16_t dy) { send_command(CommandType::MouseMoveRel, i16_pair_payload(dx, dy)); }
    void mouse_click(const std::string& button = "left") { send_command(CommandType::MouseClick, {mouse_button_mask(button)}); }
    void mouse_down(const std::string& button = "left") { send_command(CommandType::MouseButtonDown, {mouse_button_mask(button)}); }
    void mouse_up(const std::string& button = "left") { send_command(CommandType::MouseButtonUp, {mouse_button_mask(button)}); }
    void mouse_wheel(std::int8_t delta) { send_command(CommandType::MouseWheel, {static_cast<std::uint8_t>(delta)}); }
    void wait_ms(std::uint32_t ms) { send_command(CommandType::WaitMs, u32_payload(ms)); }
    void stop_all() { send_command(CommandType::StopAll); }
    void run_script(const std::string& script) {
        send_command(CommandType::BatchBegin);
        try {
            for (const auto& command : parse_script(script)) {
                auto packet = script_command_to_packet(command);
                send_command(packet.command_type, packet.payload);
            }
            send_command(CommandType::BatchEnd);
        } catch (...) {
            try {
                stop_all();
            } catch (...) {
            }
            throw;
        }
    }

private:
    std::uint16_t next_sequence() {
        auto current = sequence_;
        sequence_ = static_cast<std::uint16_t>(sequence_ + 1);
        if (sequence_ == 0) sequence_ = 1;
        return current;
    }

    void write_all(const std::vector<std::uint8_t>& bytes) {
        DWORD written = 0;
        if (!WriteFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size()) {
            throw std::runtime_error("serial write failed");
        }
    }

    Response read_response(std::uint16_t expected_sequence) {
        std::vector<std::uint8_t> buffer;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
        while (std::chrono::steady_clock::now() < deadline) {
            std::uint8_t chunk[64]{};
            DWORD read = 0;
            if (!ReadFile(handle_, chunk, sizeof(chunk), &read, nullptr)) {
                throw std::runtime_error("serial read failed");
            }
            buffer.insert(buffer.end(), chunk, chunk + read);
            if (auto response = try_decode_response(buffer, expected_sequence); response.sequence == expected_sequence) {
                return response;
            }
        }
        throw std::runtime_error("timed out waiting for response");
    }

    Response try_decode_response(std::vector<std::uint8_t>& buffer, std::uint16_t expected_sequence) {
        while (buffer.size() >= 2) {
            if (buffer[0] != MAGIC0 || buffer[1] != MAGIC1) {
                buffer.erase(buffer.begin());
                continue;
            }
            if (buffer.size() < 9) return {};
            auto payload_len = read_u16(buffer, 7);
            auto frame_len = FRAME_OVERHEAD + payload_len;
            if (buffer.size() < frame_len) return {};
            std::vector<std::uint8_t> frame_bytes(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(frame_len));
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(frame_len));
            auto frame = decode_frame(frame_bytes);
            if (frame.sequence != expected_sequence) continue;
            return Response{frame.command_type, frame.payload, frame.sequence};
        }
        return {};
    }

    std::string port_name_;
    DWORD baud_;
    DWORD timeout_ms_;
    int retries_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::uint16_t sequence_ = 1;
};
#endif

}  // namespace rp2350_hid_bridge
