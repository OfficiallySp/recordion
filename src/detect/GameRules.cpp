#include "detect/GameRules.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>

namespace rec {
namespace {

constexpr double kActiveGpuPercent = 10.0;

bool iequals(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

bool isGame(const GameSignals& s) {
    if (s.userNever) return false;
    if (s.userAlways) return true;
    if (s.excluded) return false;
    const bool gpuActive = s.gpu3dPercent >= kActiveGpuPercent;
    const bool storeSaysGame = s.knownToWindows || s.inLibrary;
    if (storeSaysGame) return s.coversMonitor || gpuActive;
    return s.coversMonitor && gpuActive;
}

bool isExcludedExecutable(std::wstring_view fileName) {
    static constexpr std::array<std::wstring_view, 52> kNames{
        // Windows shell and system UI
        L"explorer.exe", L"applicationframehost.exe", L"searchhost.exe", L"startmenuexperiencehost.exe",
        L"shellexperiencehost.exe", L"lockapp.exe", L"taskmgr.exe", L"systemsettings.exe", L"textinputhost.exe",
        L"dwm.exe", L"mstsc.exe", L"msrdc.exe",
        // Browsers
        L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe", L"opera.exe", L"opera_gx.exe", L"vivaldi.exe",
        L"arc.exe",
        // Video players and streaming
        L"vlc.exe", L"mpv.exe", L"mpc-hc64.exe", L"mpc-be64.exe", L"potplayermini64.exe", L"wmplayer.exe",
        L"video.ui.exe", L"microsoft.media.player.exe", L"obs64.exe", L"plex.exe", L"netflix.exe",
        // Game launchers (the launcher itself, not its games)
        L"steam.exe", L"steamwebhelper.exe", L"epicgameslauncher.exe", L"eadesktop.exe", L"battle.net.exe",
        L"upc.exe", L"riotclientservices.exe", L"riotclientux.exe", L"galaxyclient.exe", L"xboxpcapp.exe",
        L"playnite.desktopapp.exe",
        // Everyday apps that can go fullscreen and use the GPU
        L"discord.exe", L"spotify.exe", L"teams.exe", L"ms-teams.exe", L"zoom.exe", L"slack.exe", L"code.exe",
        L"devenv.exe", L"windowsterminal.exe", L"recordion.exe"};
    return std::ranges::any_of(kNames, [&](std::wstring_view n) { return iequals(n, fileName); });
}

std::string usableProductName(std::string_view productName, std::string_view fileDescription) {
    static constexpr std::array<std::string_view, 12> kBoilerplate{
        "unreal engine", "unity", "bootstrappackagedgame", "launcher", "game", "directx",
        "microsoft® windows® operating system", "win64 shipping", "shipping", "client", "application", "frostbite"};
    for (std::string_view candidate : {productName, fileDescription}) {
        while (!candidate.empty() && std::isspace(static_cast<unsigned char>(candidate.back()))) candidate.remove_suffix(1);
        while (!candidate.empty() && std::isspace(static_cast<unsigned char>(candidate.front()))) candidate.remove_prefix(1);
        if (candidate.size() < 2) continue;
        const std::string l = lower(candidate);
        if (std::ranges::find(kBoilerplate, l) != kBoilerplate.end()) continue;
        if (l.find("unreal engine") != std::string::npos || l.find("unity player") != std::string::npos) continue;
        return std::string(candidate);
    }
    return {};
}

} // namespace rec
