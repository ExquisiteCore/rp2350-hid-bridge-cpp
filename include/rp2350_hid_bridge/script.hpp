#pragma once

#include "rp2350_hid_bridge/keys.hpp"
#include "rp2350_hid_bridge/protocol.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace rp2350_hid_bridge {

enum class KeyAction { Tap, Down, Up };
enum class MouseAction { Move, Click, Down, Up, Wheel };
enum class ScriptKind { Type, Key, Mouse, Wait, Stop };

struct ScriptCommand {
    ScriptKind kind = ScriptKind::Stop;
    KeyAction key_action = KeyAction::Tap;
    MouseAction mouse_action = MouseAction::Move;
    std::string text;
    KeyCombo combo;
    std::int16_t dx = 0;
    std::int16_t dy = 0;
    std::uint8_t button = 0;
    std::int8_t delta = 0;
    std::uint32_t ms = 0;
};

struct CommandPacket {
    CommandType command_type = CommandType::StopAll;
    std::vector<std::uint8_t> payload;
};

inline std::vector<std::string> split_words(const std::string& line) {
    std::vector<std::string> out;
    std::string current;
    bool in_quote = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') {
            if (in_quote) {
                out.push_back(current);
                current.clear();
                in_quote = false;
                while (i + 1 < line.size() && (line[i + 1] == ' ' || line[i + 1] == '\t')) {
                    ++i;
                }
            } else {
                if (!current.empty()) {
                    throw std::invalid_argument("quote must start a new token");
                }
                in_quote = true;
            }
        } else if (ch == '\\' && in_quote) {
            if (++i >= line.size()) {
                throw std::invalid_argument("trailing escape in quoted string");
            }
            const char escaped = line[i];
            current.push_back(escaped == 'n' ? '\n' : escaped == 'r' ? '\r' : escaped == 't' ? '\t' : escaped);
        } else if ((ch == ' ' || ch == '\t') && !in_quote) {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
        } else if (ch == '#' && !in_quote && current.empty()) {
            break;
        } else {
            current.push_back(ch);
        }
    }
    if (in_quote) {
        throw std::invalid_argument("unterminated quoted string");
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

inline long long parse_integer_token(const std::string& token, const std::string& name) {
    std::size_t parsed = 0;
    long long value = 0;
    try {
        value = std::stoll(token, &parsed, 10);
    } catch (const std::exception&) {
        throw std::invalid_argument(name + " must be an integer");
    }
    if (parsed != token.size()) {
        throw std::invalid_argument(name + " must be an integer");
    }
    return value;
}

template <typename T>
inline T parse_ranged_integer_token(const std::string& token, const std::string& name) {
    const long long value = parse_integer_token(token, name);
    const auto min_value = static_cast<long long>(std::numeric_limits<T>::min());
    const auto max_value = static_cast<long long>(std::numeric_limits<T>::max());
    if (value < min_value || value > max_value) {
        throw std::invalid_argument(name + " is out of range");
    }
    return static_cast<T>(value);
}

/// Parse one script line. Blank lines and comments set has_command to false.
inline ScriptCommand parse_script_line(const std::string& line, bool& has_command) {
    has_command = false;
    auto words = split_words(line);
    if (words.empty()) {
        return {};
    }
    has_command = true;
    auto head = upper_ascii(words[0]);
    ScriptCommand command;
    if (head == "TYPE" || head == "TEXT") {
        if (words.size() != 2) throw std::invalid_argument("type expects one string");
        command.kind = ScriptKind::Type;
        command.text = words[1];
    } else if (head == "KEY") {
        if (words.size() != 3) throw std::invalid_argument("key expects action and combo");
        command.kind = ScriptKind::Key;
        auto action = upper_ascii(words[1]);
        if (action == "TAP") {
            command.key_action = KeyAction::Tap;
        } else if (action == "DOWN") {
            command.key_action = KeyAction::Down;
        } else if (action == "UP") {
            command.key_action = KeyAction::Up;
        } else {
            throw std::invalid_argument("unknown key action " + words[1]);
        }
        command.combo = parse_combo(words[2]);
    } else if (head == "MOUSE") {
        if (words.size() < 2) throw std::invalid_argument("mouse expects action");
        command.kind = ScriptKind::Mouse;
        auto action = upper_ascii(words[1]);
        if (action == "MOVE") {
            if (words.size() != 4) throw std::invalid_argument("mouse move expects dx dy");
            command.mouse_action = MouseAction::Move;
            command.dx = parse_ranged_integer_token<std::int16_t>(words[2], "mouse dx");
            command.dy = parse_ranged_integer_token<std::int16_t>(words[3], "mouse dy");
        } else if (action == "CLICK" || action == "DOWN" || action == "UP") {
            if (words.size() != 3) throw std::invalid_argument("mouse button action expects button");
            command.mouse_action = action == "DOWN" ? MouseAction::Down : action == "UP" ? MouseAction::Up : MouseAction::Click;
            command.button = mouse_button_mask(words[2]);
        } else if (action == "WHEEL") {
            if (words.size() != 3) throw std::invalid_argument("mouse wheel expects delta");
            command.mouse_action = MouseAction::Wheel;
            command.delta = parse_ranged_integer_token<std::int8_t>(words[2], "mouse wheel delta");
        } else {
            throw std::invalid_argument("unknown mouse action");
        }
    } else if (head == "WAIT") {
        if (words.size() != 2) throw std::invalid_argument("wait expects milliseconds");
        command.kind = ScriptKind::Wait;
        command.ms = parse_ranged_integer_token<std::uint32_t>(words[1], "wait milliseconds");
    } else if (head == "STOP") {
        if (words.size() != 1) throw std::invalid_argument("stop takes no arguments");
        command.kind = ScriptKind::Stop;
    } else {
        throw std::invalid_argument("unknown script command");
    }
    return command;
}

/// Parse a multi-line script into executable commands.
inline std::vector<ScriptCommand> parse_script(const std::string& script) {
    std::vector<ScriptCommand> commands;
    std::size_t start = 0;
    while (start <= script.size()) {
        auto end = script.find('\n', start);
        auto line = script.substr(start, end == std::string::npos ? std::string::npos : end - start);
        bool has_command = false;
        auto command = parse_script_line(line, has_command);
        if (has_command) {
            commands.push_back(command);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return commands;
}

inline CommandPacket script_command_to_packet(const ScriptCommand& command) {
    switch (command.kind) {
        case ScriptKind::Type:
            return {CommandType::TypeAscii, ascii_payload(command.text)};
        case ScriptKind::Key:
            switch (command.key_action) {
                case KeyAction::Tap:
                    return {CommandType::KeyTap, key_payload(command.combo)};
                case KeyAction::Down:
                    return {CommandType::KeyDown, key_payload(command.combo)};
                case KeyAction::Up:
                    return {CommandType::KeyUp, key_payload(command.combo)};
            }
            break;
        case ScriptKind::Mouse:
            switch (command.mouse_action) {
                case MouseAction::Move:
                    return {CommandType::MouseMoveRel, i16_pair_payload(command.dx, command.dy)};
                case MouseAction::Click:
                    return {CommandType::MouseClick, {command.button}};
                case MouseAction::Down:
                    return {CommandType::MouseButtonDown, {command.button}};
                case MouseAction::Up:
                    return {CommandType::MouseButtonUp, {command.button}};
                case MouseAction::Wheel:
                    return {CommandType::MouseWheel, {static_cast<std::uint8_t>(command.delta)}};
            }
            break;
        case ScriptKind::Wait:
            return {CommandType::WaitMs, u32_payload(command.ms)};
        case ScriptKind::Stop:
            return {CommandType::StopAll, {}};
    }
    throw std::invalid_argument("unsupported script command");
}

}  // namespace rp2350_hid_bridge
