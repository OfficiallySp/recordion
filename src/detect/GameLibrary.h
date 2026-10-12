#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace rec {

// Key/value pairs of a Valve KeyValues (.vdf/.acf) text file, flattened: nested keys are joined
// with '/', e.g. "AppState/name", "libraryfolders/1/path". Keys are lower-cased.
std::multimap<std::string, std::string> parseVdf(std::string_view text);

// What the installed game launchers and Windows know about games on this PC. Built once on the
// detector thread; reading it is cheap.
class GameLibrary {
public:
    struct Match {
        bool inLibrary = false; // installed by a game store / launcher
        bool knownToWindows = false; // in Windows' game list (Game Bar)
        std::string name;       // from the store, when known
    };

    void build();
    Match lookup(const std::filesystem::path& exe) const;

    // For tests and diagnostics.
    void addInstall(const std::filesystem::path& dir, std::string name);
    void addRoot(const std::filesystem::path& dir);
    void addKnownExe(const std::filesystem::path& exe);
    size_t installCount() const { return installs_.size(); }
    size_t rootCount() const { return roots_.size(); }
    size_t knownExeCount() const { return knownExes_.size(); }
    std::vector<std::string> installNames() const {
        std::vector<std::string> names;
        for (const auto& i : installs_) names.push_back(i.name);
        return names;
    }

private:
    void scanSteam();
    void scanEpic();
    void scanGog();
    void scanUbisoft();
    void scanGameConfigStore();

    struct Install {
        std::wstring dir; // lower-case, with trailing backslash
        std::string name;
    };
    std::vector<Install> installs_;
    std::vector<std::wstring> roots_; // lower-case library roots, trailing backslash
    std::unordered_set<std::wstring> knownExes_;
};

std::wstring lowerPath(const std::filesystem::path& p);

} // namespace rec
