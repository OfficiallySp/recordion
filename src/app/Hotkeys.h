#pragma once

#include "core/Hotkey.h"
#include "core/Settings.h"

#include <windows.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace rec {

enum class HotkeyAction { SaveReplay, ToggleRecording, ToggleReplayBuffer, ToggleDesktopRecording, Count };

const char* hotkeyActionName(HotkeyAction action);

// Registers the configured global hotkeys on a window (WM_HOTKEY). Re-apply after settings change.
class HotkeyManager {
public:
    struct Failure {
        HotkeyAction action;
        std::string combo;
        std::string reason;
    };

    ~HotkeyManager() { clear(); }

    std::vector<Failure> apply(HWND hwnd, const HotkeySettings& settings);
    void clear();
    std::optional<HotkeyAction> actionFromId(WPARAM id) const;

private:
    HWND hwnd_ = nullptr;
    std::array<bool, static_cast<size_t>(HotkeyAction::Count)> registered_{};
};

} // namespace rec
