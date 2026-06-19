#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace rp2350_hid_bridge {

inline constexpr std::uint8_t PROTOCOL_VERSION = 1;
inline constexpr std::uint8_t MAGIC0 = 0xA5;
inline constexpr std::uint8_t MAGIC1 = 0x5A;
inline constexpr std::size_t MAX_PAYLOAD_SIZE = 240;
inline constexpr std::size_t FRAME_OVERHEAD = 11;
inline constexpr std::size_t MAX_FRAME_SIZE = FRAME_OVERHEAD + MAX_PAYLOAD_SIZE;

/// Command and response type values used by the CDC framing protocol.
enum class CommandType : std::uint8_t {
    Ping = 0x01,
    GetInfo = 0x02,
    GetCaps = 0x03,
    KeyDown = 0x10,
    KeyUp = 0x11,
    KeyTap = 0x12,
    TypeAscii = 0x13,
    MouseMoveRel = 0x20,
    MouseButtonDown = 0x21,
    MouseButtonUp = 0x22,
    MouseClick = 0x23,
    MouseWheel = 0x24,
    WaitMs = 0x30,
    BatchBegin = 0x40,
    BatchEnd = 0x41,
    StopAll = 0x7F,
    Ack = 0x80,
    Nack = 0x81,
    Status = 0x82,
    Busy = 0x83,
};

struct DecodeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Frame {
    std::uint8_t version = 0;
    std::uint8_t flags = 0;
    std::uint16_t sequence = 0;
    CommandType command_type = CommandType::Ping;
    std::vector<std::uint8_t> payload;
};

struct Response {
    CommandType command_type = CommandType::Ack;
    std::vector<std::uint8_t> payload;
    std::uint16_t sequence = 0;
};

inline std::uint16_t crc16_ccitt_false(
    const std::vector<std::uint8_t>& data,
    std::size_t offset,
    std::size_t len) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = offset; i < offset + len; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) != 0
                ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021)
                : static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

inline void write_u16(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value) {
    out[offset] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[offset + 1] = static_cast<std::uint8_t>(value & 0xFF);
}

inline std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}

/// Encode one command frame. Throws std::invalid_argument if payload exceeds 240 bytes.
inline std::vector<std::uint8_t> encode_frame(
    std::uint16_t sequence,
    CommandType command_type,
    const std::vector<std::uint8_t>& payload = {}) {
    if (payload.size() > MAX_PAYLOAD_SIZE) {
        throw std::invalid_argument("payload too long");
    }

    std::vector<std::uint8_t> out(FRAME_OVERHEAD + payload.size());
    out[0] = MAGIC0;
    out[1] = MAGIC1;
    out[2] = PROTOCOL_VERSION;
    out[3] = 0;
    write_u16(out, 4, sequence);
    out[6] = static_cast<std::uint8_t>(command_type);
    write_u16(out, 7, static_cast<std::uint16_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), out.begin() + 9);

    const auto crc = crc16_ccitt_false(out, 2, 7 + payload.size());
    write_u16(out, 9 + payload.size(), crc);
    return out;
}

/// Decode and validate one complete frame.
inline Frame decode_frame(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < FRAME_OVERHEAD) {
        throw DecodeError("frame too short");
    }
    if (frame[0] != MAGIC0 || frame[1] != MAGIC1) {
        throw DecodeError("bad magic");
    }

    const auto payload_len = read_u16(frame, 7);
    if (payload_len > MAX_PAYLOAD_SIZE) {
        throw DecodeError("payload too long");
    }
    if (frame.size() != FRAME_OVERHEAD + payload_len) {
        throw DecodeError("length mismatch");
    }

    const auto crc_offset = 9 + payload_len;
    const auto expected_crc = read_u16(frame, crc_offset);
    const auto actual_crc = crc16_ccitt_false(frame, 2, 7 + payload_len);
    if (expected_crc != actual_crc) {
        throw DecodeError("bad crc");
    }

    Frame decoded;
    decoded.version = frame[2];
    decoded.flags = frame[3];
    decoded.sequence = read_u16(frame, 4);
    decoded.command_type = static_cast<CommandType>(frame[6]);
    decoded.payload.assign(frame.begin() + 9, frame.begin() + static_cast<std::ptrdiff_t>(crc_offset));
    return decoded;
}

inline CommandType expected_response_type(CommandType command_type) {
    if (command_type == CommandType::GetInfo || command_type == CommandType::GetCaps) {
        return CommandType::Status;
    }
    return CommandType::Ack;
}

inline std::vector<std::uint8_t> ascii_payload(const std::string& text) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        if (ch > 0x7F) {
            throw std::invalid_argument("TYPE_ASCII only accepts ASCII text");
        }
        out.push_back(ch);
    }
    return out;
}

inline std::vector<std::uint8_t> i16_pair_payload(std::int16_t dx, std::int16_t dy) {
    const auto ux = static_cast<std::uint16_t>(dx);
    const auto uy = static_cast<std::uint16_t>(dy);
    return {
        static_cast<std::uint8_t>((ux >> 8) & 0xFF),
        static_cast<std::uint8_t>(ux & 0xFF),
        static_cast<std::uint8_t>((uy >> 8) & 0xFF),
        static_cast<std::uint8_t>(uy & 0xFF),
    };
}

inline std::vector<std::uint8_t> u32_payload(std::uint32_t value) {
    return {
        static_cast<std::uint8_t>((value >> 24) & 0xFF),
        static_cast<std::uint8_t>((value >> 16) & 0xFF),
        static_cast<std::uint8_t>((value >> 8) & 0xFF),
        static_cast<std::uint8_t>(value & 0xFF),
    };
}

}  // namespace rp2350_hid_bridge
