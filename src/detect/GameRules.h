#pragma once

#include <string>
#include <string_view>

namespace rec {

// Everything the detector learned about a foreground window, without touching the process
// beyond a limited query handle.
struct GameSignals {
    bool userAlways = false;     // on the user's "always record" list
    bool userNever = false;      // on the user's "never record" list
    bool excluded = false;       // browsers, media players, launchers, system UI...
    bool knownToWindows = false; // in Windows' game list (Game Bar)
    bool inLibrary = false;      // installed by a game store / launcher
    bool coversMonitor = false;  // fullscreen or borderless fullscreen
    double gpu3dPercent = 0;     // the process's 3D engine use while sampled
};

// Strong evidence (a game store or Windows says it's a game) plus behaviour (fullscreen or real 3D
// work), or both behaviours together. Users can always override.
bool isGame(const GameSignals& s);

// Apps that are never games, by executable file name (case-insensitive).
bool isExcludedExecutable(std::wstring_view fileName);

// Picks a display name from version info, ignoring engine/launcher boilerplate.
// Empty if nothing useful.
std::string usableProductName(std::string_view productName, std::string_view fileDescription);

} // namespace rec
