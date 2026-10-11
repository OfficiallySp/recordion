#include "app/Hotkeys.h"

#include "core/Log.h"

namespace rec {
namespace {

constexpr int kIdBase = 0x5200;

const std::string& comboFor(const HotkeySettings& s, HotkeyAction a) {
    switch (a) {
    case HotkeyAction::SaveReplay: return s.saveReplay;
    case HotkeyAction::ToggleRecording: return s.toggleRecording;
    case HotkeyAction::ToggleReplayBuffer: return s.toggleReplayBuffer;
    case HotkeyAction::ToggleDesktopRecording: return s.toggleDesktopRecording;
    case HotkeyAction::Count: break;
    }
    static const std::string empty;
    return empty;
}

} // namespace

const char* hotkeyActionName(HotkeyAction action) {
    switch (action) {
    case HotkeyAction::SaveReplay: return "Save replay";
    case HotkeyAction::ToggleRecording: return "Start/stop recording";
    case HotkeyAction::ToggleReplayBuffer: return "Toggle replay buffer";
    case HotkeyAction::ToggleDesktopRecording: return "Start/stop desktop recording";
    case HotkeyAction::Count: break;
    }
    return "?";
}

std::vector<HotkeyManager::Failure> HotkeyManager::apply(HWND hwnd, const HotkeySettings& settings) {
    clear();
    hwnd_ = hwnd;
    std::vector<Failure> failures;
    std::array<Hotkey, static_cast<size_t>(HotkeyAction::Count)> parsed{};

    for (size_t i = 0; i < parsed.size(); ++i) {
        const auto action = static_cast<HotkeyAction>(i);
        const std::string& combo = comboFor(settings, action);
        auto hk = parseHotkey(combo);
        if (!hk) {
            failures.push_back({action, combo, hk.error()});
            continue;
        }
        if (!hk->bound()) continue;
        for (size_t j = 0; j < i; ++j) {
            if (parsed[j] == *hk) {
                failures.push_back({action, combo, std::string("same keys as \"") +
                                                       hotkeyActionName(static_cast<HotkeyAction>(j)) + "\""});
                hk->vk = 0;
                break;
            }
        }
        if (!hk->bound()) continue;
        parsed[i] = *hk;

        if (!RegisterHotKey(hwnd, kIdBase + static_cast<int>(i), hk->modifiers | MOD_NOREPEAT, hk->vk)) {
            const DWORD err = GetLastError();
            failures.push_back({action, combo,
                                err == ERROR_HOTKEY_ALREADY_REGISTERED ? "already in use by another app"
                                                                       : "RegisterHotKey failed (" + std::to_string(err) + ")"});
            continue;
        }
        registered_[i] = true;
        log::info("hotkey: {} = {}", hotkeyActionName(action), formatHotkey(*hk));
    }
    for (const auto& f : failures)
        log::warn("hotkey: {} \"{}\" not registered: {}", hotkeyActionName(f.action), f.combo, f.reason);
    return failures;
}

void HotkeyManager::clear() {
    for (size_t i = 0; i < registered_.size(); ++i) {
        if (registered_[i]) UnregisterHotKey(hwnd_, kIdBase + static_cast<int>(i));
        registered_[i] = false;
    }
}

std::optional<HotkeyAction> HotkeyManager::actionFromId(WPARAM id) const {
    const auto index = static_cast<int64_t>(id) - kIdBase;
    if (index < 0 || index >= static_cast<int64_t>(HotkeyAction::Count)) return std::nullopt;
    return static_cast<HotkeyAction>(index);
}

} // namespace rec
