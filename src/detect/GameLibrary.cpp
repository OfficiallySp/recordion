#include "detect/GameLibrary.h"

#include "core/Log.h"
#include "core/Utf.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace rec {
namespace {

std::string readText(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::wstring withSlash(std::wstring s) {
    if (!s.empty() && s.back() != L'\\') s += L'\\';
    return s;
}

std::wstring regString(HKEY root, const wchar_t* path, const wchar_t* value) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, path, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

// Calls fn(subkeyName) for every subkey of root\path.
template <class F>
void forEachSubkey(HKEY root, const wchar_t* path, F&& fn) {
    HKEY key;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        fn(std::wstring(name));
    }
    RegCloseKey(key);
}

} // namespace

std::wstring lowerPath(const std::filesystem::path& p) {
    std::wstring s = p.lexically_normal().wstring();
    std::ranges::replace(s, L'/', L'\\');
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::multimap<std::string, std::string> parseVdf(std::string_view text) {
    std::multimap<std::string, std::string> out;
    std::vector<std::string> path;
    size_t i = 0;
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::string pendingKey;
    bool haveKey = false;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '"') {
            std::string token;
            for (++i; i < text.size() && text[i] != '"'; ++i) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    ++i;
                    token += text[i] == 'n' ? '\n' : text[i] == 't' ? '\t' : text[i];
                } else {
                    token += text[i];
                }
            }
            ++i;
            if (!haveKey) {
                pendingKey = lower(token);
                haveKey = true;
            } else {
                std::string full;
                for (const auto& p : path) full += p + "/";
                out.emplace(full + pendingKey, token);
                haveKey = false;
            }
        } else if (c == '{') {
            if (haveKey) path.push_back(pendingKey);
            haveKey = false;
            ++i;
        } else if (c == '}') {
            if (!path.empty()) path.pop_back();
            haveKey = false;
            ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
        } else {
            ++i;
        }
    }
    return out;
}

void GameLibrary::addInstall(const std::filesystem::path& dir, std::string name) {
    if (dir.empty()) return;
    installs_.push_back({withSlash(lowerPath(dir)), std::move(name)});
}

void GameLibrary::addRoot(const std::filesystem::path& dir) {
    if (!dir.empty()) roots_.push_back(withSlash(lowerPath(dir)));
}

void GameLibrary::addKnownExe(const std::filesystem::path& exe) {
    if (!exe.empty()) knownExes_.insert(lowerPath(exe));
}

void GameLibrary::build() {
    installs_.clear();
    roots_.clear();
    knownExes_.clear();
    scanSteam();
    scanEpic();
    scanGog();
    scanUbisoft();
    scanGameConfigStore();
    // Folders that only ever hold games.
    for (const wchar_t* drive : {L"C:\\", L"D:\\", L"E:\\", L"F:\\"}) {
        std::error_code ec;
        if (std::filesystem::exists(std::wstring(drive) + L"XboxGames", ec)) addRoot(std::wstring(drive) + L"XboxGames");
    }
    log::info("games: {} installed titles, {} library folders, {} known to Windows", installs_.size(), roots_.size(),
              knownExes_.size());
}

void GameLibrary::scanSteam() {
    const std::wstring steam = regString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) return;
    std::vector<std::filesystem::path> libraries{std::filesystem::path(steam)};
    const auto folders = parseVdf(readText(std::filesystem::path(steam) / L"steamapps" / L"libraryfolders.vdf"));
    for (const auto& [key, value] : folders)
        if (key.ends_with("/path")) libraries.emplace_back(widen(value));

    for (const auto& lib : libraries) {
        const auto apps = lib / L"steamapps";
        addRoot(apps / L"common");
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(apps, ec)) {
            const auto file = entry.path().filename().wstring();
            if (!file.starts_with(L"appmanifest_") || entry.path().extension() != L".acf") continue;
            const auto kv = parseVdf(readText(entry.path()));
            const auto name = kv.find("appstate/name");
            const auto dir = kv.find("appstate/installdir");
            if (name != kv.end() && dir != kv.end()) addInstall(apps / L"common" / widen(dir->second), name->second);
        }
    }
}

void GameLibrary::scanEpic() {
    const std::filesystem::path manifests = L"C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(manifests, ec)) {
        if (entry.path().extension() != L".item") continue;
        try {
            const auto j = nlohmann::json::parse(readText(entry.path()));
            const std::string location = j.value("InstallLocation", "");
            const std::string name = j.value("DisplayName", "");
            if (!location.empty()) addInstall(widen(location), name);
        } catch (const nlohmann::json::exception&) {
        }
    }
}

void GameLibrary::scanGog() {
    const wchar_t* base = L"SOFTWARE\\WOW6432Node\\GOG.com\\Games";
    forEachSubkey(HKEY_LOCAL_MACHINE, base, [&](const std::wstring& id) {
        const std::wstring key = std::wstring(base) + L"\\" + id;
        const std::wstring path = regString(HKEY_LOCAL_MACHINE, key.c_str(), L"path");
        const std::wstring name = regString(HKEY_LOCAL_MACHINE, key.c_str(), L"gameName");
        if (!path.empty()) addInstall(path, narrow(name));
    });
}

void GameLibrary::scanUbisoft() {
    const wchar_t* base = L"SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs";
    forEachSubkey(HKEY_LOCAL_MACHINE, base, [&](const std::wstring& id) {
        const std::wstring key = std::wstring(base) + L"\\" + id;
        const std::wstring dir = regString(HKEY_LOCAL_MACHINE, key.c_str(), L"InstallDir");
        // Ubisoft keeps no display name here; the folder is named after the game.
        std::wstring trimmed = dir;
        while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
        if (!trimmed.empty()) addInstall(trimmed, narrow(std::filesystem::path(trimmed).filename().wstring()));
    });
}

void GameLibrary::scanGameConfigStore() {
    // Windows' own game list (what Game Bar uses), keyed by executable.
    const wchar_t* base = L"System\\GameConfigStore\\Children";
    forEachSubkey(HKEY_CURRENT_USER, base, [&](const std::wstring& id) {
        const std::wstring key = std::wstring(base) + L"\\" + id;
        const std::wstring exe = regString(HKEY_CURRENT_USER, key.c_str(), L"MatchedExeFullPath");
        if (!exe.empty()) addKnownExe(exe);
    });
}

GameLibrary::Match GameLibrary::lookup(const std::filesystem::path& exe) const {
    Match m;
    const std::wstring p = lowerPath(exe);
    m.knownToWindows = knownExes_.contains(p);
    size_t best = 0;
    for (const auto& i : installs_) {
        if (p.starts_with(i.dir) && i.dir.size() > best) {
            best = i.dir.size();
            m.inLibrary = true;
            m.name = i.name;
        }
    }
    if (!m.inLibrary)
        m.inLibrary = std::ranges::any_of(roots_, [&](const std::wstring& r) { return p.starts_with(r); });
    return m;
}

} // namespace rec
