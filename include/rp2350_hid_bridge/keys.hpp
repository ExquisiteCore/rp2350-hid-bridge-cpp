#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace rp2350_hid_bridge {

struct KeyCombo {
    std::uint8_t modifier = 0;
    std::uint8_t keycode = 0;
};

inline std::string upper_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

/// Parse combinations such as CTRL+C, SHIFT+F5, WIN+R.
inline KeyCombo parse_combo(const std::string& input) {
    static const std::map<std::string, std::uint8_t> keycodes = {
        {"0", 0x27}, {"ENTER", 0x28}, {"RETURN", 0x28}, {"ESC", 0x29}, {"ESCAPE", 0x29},
        {"BACKSPACE", 0x2A}, {"BKSP", 0x2A}, {"TAB", 0x2B}, {"SPACE", 0x2C},
        {"MINUS", 0x2D}, {"-", 0x2D}, {"EQUAL", 0x2E}, {"=", 0x2E},
        {"LBRACKET", 0x2F}, {"[", 0x2F}, {"RBRACKET", 0x30}, {"]", 0x30},
        {"BACKSLASH", 0x31}, {"\\", 0x31}, {"SEMICOLON", 0x33}, {";", 0x33},
        {"QUOTE", 0x34}, {"'", 0x34}, {"GRAVE", 0x35}, {"`", 0x35},
        {"COMMA", 0x36}, {",", 0x36}, {"DOT", 0x37}, {"PERIOD", 0x37}, {".", 0x37},
        {"SLASH", 0x38}, {"/", 0x38}, {"CAPSLOCK", 0x39},
        {"F1", 0x3A}, {"F2", 0x3B}, {"F3", 0x3C}, {"F4", 0x3D}, {"F5", 0x3E},
        {"F6", 0x3F}, {"F7", 0x40}, {"F8", 0x41}, {"F9", 0x42}, {"F10", 0x43},
        {"F11", 0x44}, {"F12", 0x45}, {"PRINTSCREEN", 0x46}, {"PRTSCR", 0x46},
        {"SCROLLLOCK", 0x47}, {"PAUSE", 0x48}, {"INSERT", 0x49}, {"HOME", 0x4A},
        {"PAGEUP", 0x4B}, {"PGUP", 0x4B}, {"DELETE", 0x4C}, {"DEL", 0x4C},
        {"END", 0x4D}, {"PAGEDOWN", 0x4E}, {"PGDN", 0x4E}, {"RIGHT", 0x4F},
        {"LEFT", 0x50}, {"DOWN", 0x51}, {"UP", 0x52},
    };

    KeyCombo combo;
    bool has_key = false;
    std::size_t start = 0;
    while (start <= input.size()) {
        auto end = input.find('+', start);
        auto token = upper_ascii(input.substr(start, end == std::string::npos ? std::string::npos : end - start));
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char ch) { return std::isspace(ch); }), token.end());
        if (token.empty()) {
            throw std::invalid_argument("empty key token");
        }

        if (token == "CTRL" || token == "CONTROL") {
            combo.modifier |= 0x01;
        } else if (token == "SHIFT") {
            combo.modifier |= 0x02;
        } else if (token == "ALT") {
            combo.modifier |= 0x04;
        } else if (token == "GUI" || token == "WIN" || token == "META") {
            combo.modifier |= 0x08;
        } else {
            if (has_key) {
                throw std::invalid_argument("combo contains more than one non-modifier key");
            }
            if (token.size() == 1 && token[0] >= 'A' && token[0] <= 'Z') {
                combo.keycode = static_cast<std::uint8_t>(0x04 + (token[0] - 'A'));
            } else if (token.size() == 1 && token[0] >= '1' && token[0] <= '9') {
                combo.keycode = static_cast<std::uint8_t>(0x1E + (token[0] - '1'));
            } else {
                auto found = keycodes.find(token);
                if (found == keycodes.end()) {
                    throw std::invalid_argument("unknown key " + token);
                }
                combo.keycode = found->second;
            }
            has_key = true;
        }

        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    if (!has_key) {
        throw std::invalid_argument("combo has no key");
    }
    return combo;
}

inline std::vector<std::uint8_t> key_payload(KeyCombo combo) {
    return {combo.modifier, combo.keycode};
}

/// Convert left/right/middle mouse button names to HID button masks.
inline std::uint8_t mouse_button_mask(const std::string& button) {
    auto lowered = upper_ascii(button);
    if (lowered == "LEFT" || lowered == "L") {
        return 0x01;
    }
    if (lowered == "RIGHT" || lowered == "R") {
        return 0x02;
    }
    if (lowered == "MIDDLE" || lowered == "M") {
        return 0x04;
    }
    throw std::invalid_argument("unknown mouse button " + button);
}

}  // namespace rp2350_hid_bridge
