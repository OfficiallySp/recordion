#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace rec {

// A global key combination, e.g. "Ctrl+Shift+F10". Modifiers use the RegisterHotKey MOD_* bits.
struct Hotkey {
    uint32_t modifiers = 0; // MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN
    uint32_t vk = 0;        // virtual-key code; 0 = unbound

    bool bound() const { return vk != 0; }
    bool operator==(const Hotkey&) const = default;
};

// Parses "Alt+F10", "ctrl + shift + R", "Win+PrintScreen", "" (unbound). Case-insensitive.
// Rejects typing keys (letters, digits, space, punctuation) without Ctrl/Alt/Win, since a global
// hotkey would swallow them everywhere.
std::expected<Hotkey, std::string> parseHotkey(std::string_view text);

// Canonical form: "Ctrl+Alt+Shift+Win+Key"; "" when unbound.
std::string formatHotkey(const Hotkey& hotkey);

// Display name of a virtual key ("F10", "A", "PageUp", ...); empty if Recordion can't bind it.
std::string keyName(uint32_t vk);

} // namespace rec
