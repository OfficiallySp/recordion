#include "core/Hotkey.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>

namespace rec {
namespace {

struct NamedKey {
    std::string_view name;
    uint32_t vk;
};

// Non-alphanumeric keys that can be bound. Letters, digits and F1-F24 are handled separately.
constexpr std::array kNamedKeys{
    NamedKey{"Space", VK_SPACE},        NamedKey{"Tab", VK_TAB},
    NamedKey{"Enter", VK_RETURN},       NamedKey{"Backspace", VK_BACK},
    NamedKey{"Insert", VK_INSERT},      NamedKey{"Delete", VK_DELETE},
    NamedKey{"Home", VK_HOME},          NamedKey{"End", VK_END},
    NamedKey{"PageUp", VK_PRIOR},       NamedKey{"PageDown", VK_NEXT},
    NamedKey{"Up", VK_UP},              NamedKey{"Down", VK_DOWN},
    NamedKey{"Left", VK_LEFT},          NamedKey{"Right", VK_RIGHT},
    NamedKey{"PrintScreen", VK_SNAPSHOT}, NamedKey{"Pause", VK_PAUSE},
    NamedKey{"ScrollLock", VK_SCROLL},  NamedKey{"Num0", VK_NUMPAD0},
    NamedKey{"Num1", VK_NUMPAD1},       NamedKey{"Num2", VK_NUMPAD2},
    NamedKey{"Num3", VK_NUMPAD3},       NamedKey{"Num4", VK_NUMPAD4},
    NamedKey{"Num5", VK_NUMPAD5},       NamedKey{"Num6", VK_NUMPAD6},
    NamedKey{"Num7", VK_NUMPAD7},       NamedKey{"Num8", VK_NUMPAD8},
    NamedKey{"Num9", VK_NUMPAD9},       NamedKey{"NumMultiply", VK_MULTIPLY},
    NamedKey{"NumAdd", VK_ADD},         NamedKey{"NumSubtract", VK_SUBTRACT},
    NamedKey{"NumDecimal", VK_DECIMAL}, NamedKey{"NumDivide", VK_DIVIDE},
    NamedKey{"`", VK_OEM_3},            NamedKey{"-", VK_OEM_MINUS},
    NamedKey{"=", VK_OEM_PLUS},         NamedKey{"[", VK_OEM_4},
    NamedKey{"]", VK_OEM_6},            NamedKey{"\\", VK_OEM_5},
    NamedKey{";", VK_OEM_1},            NamedKey{"'", VK_OEM_7},
    NamedKey{",", VK_OEM_COMMA},        NamedKey{".", VK_OEM_PERIOD},
    NamedKey{"/", VK_OEM_2},
};

bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

uint32_t keyFromName(std::string_view name) {
    if (name.size() == 1) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<uint32_t>(c);
    }
    if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f')) {
        int n = 0;
        for (char c : name.substr(1)) {
            if (c < '0' || c > '9') { n = -1; break; }
            n = n * 10 + (c - '0');
        }
        if (n >= 1 && n <= 24) return static_cast<uint32_t>(VK_F1 + n - 1);
    }
    for (const auto& k : kNamedKeys)
        if (iequals(k.name, name)) return k.vk;
    if (iequals(name, "Return")) return VK_RETURN;
    if (iequals(name, "PrtSc")) return VK_SNAPSHOT;
    if (iequals(name, "Del")) return VK_DELETE;
    if (iequals(name, "Ins")) return VK_INSERT;
    return 0;
}

uint32_t modifierFromName(std::string_view name) {
    if (iequals(name, "Ctrl") || iequals(name, "Control")) return MOD_CONTROL;
    if (iequals(name, "Alt")) return MOD_ALT;
    if (iequals(name, "Shift")) return MOD_SHIFT;
    if (iequals(name, "Win") || iequals(name, "Windows") || iequals(name, "Meta")) return MOD_WIN;
    return 0;
}

// Keys that produce text: binding them without Ctrl/Alt/Win would break typing system-wide.
bool isTypingKey(uint32_t vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return true;
    if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) return true;
    switch (vk) {
    case VK_SPACE: case VK_TAB: case VK_RETURN: case VK_BACK:
    case VK_OEM_1: case VK_OEM_2: case VK_OEM_3: case VK_OEM_4: case VK_OEM_5: case VK_OEM_6:
    case VK_OEM_7: case VK_OEM_PLUS: case VK_OEM_MINUS: case VK_OEM_COMMA: case VK_OEM_PERIOD:
        return true;
    default:
        return false;
    }
}

} // namespace

std::string keyName(uint32_t vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    for (const auto& k : kNamedKeys)
        if (k.vk == vk) return std::string(k.name);
    return {};
}

std::expected<Hotkey, std::string> parseHotkey(std::string_view text) {
    text = trim(text);
    Hotkey hk;
    if (text.empty()) return hk;

    // Split on '+'. The physical +/= key is spelled "=".
    size_t start = 0;
    while (start <= text.size()) {
        const size_t plus = text.find('+', start);
        const std::string_view token = trim(text.substr(start, plus == std::string_view::npos ? text.npos : plus - start));
        start = plus == std::string_view::npos ? text.size() + 1 : plus + 1;
        if (token.empty()) return std::unexpected("empty key in \"" + std::string(text) + "\"");

        if (const uint32_t mod = modifierFromName(token)) {
            if (hk.vk) return std::unexpected("modifier \"" + std::string(token) + "\" must come before the key");
            hk.modifiers |= mod;
            continue;
        }
        const uint32_t vk = keyFromName(token);
        if (!vk) return std::unexpected("unknown key \"" + std::string(token) + "\"");
        if (hk.vk) return std::unexpected("only one non-modifier key is allowed");
        hk.vk = vk;
    }

    if (!hk.vk) return std::unexpected("a hotkey needs a key, not only modifiers");
    if (isTypingKey(hk.vk) && !(hk.modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN)))
        return std::unexpected("\"" + keyName(hk.vk) + "\" needs Ctrl, Alt or Win, or it would block typing");
    return hk;
}

std::string formatHotkey(const Hotkey& hk) {
    if (!hk.bound()) return {};
    std::string s;
    if (hk.modifiers & MOD_CONTROL) s += "Ctrl+";
    if (hk.modifiers & MOD_ALT) s += "Alt+";
    if (hk.modifiers & MOD_SHIFT) s += "Shift+";
    if (hk.modifiers & MOD_WIN) s += "Win+";
    return s + keyName(hk.vk);
}

} // namespace rec
