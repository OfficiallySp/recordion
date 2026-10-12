#include "detect/GameLibrary.h"
#include "detect/GameRules.h"

#include <doctest/doctest.h>

using rec::GameSignals;
using rec::isGame;

TEST_CASE("store-listed games count when fullscreen or busy on the GPU") {
    CHECK(isGame({.inLibrary = true, .coversMonitor = true}));
    CHECK(isGame({.inLibrary = true, .gpu3dPercent = 35}));     // windowed game
    CHECK(isGame({.knownToWindows = true, .coversMonitor = true}));
    CHECK_FALSE(isGame({.inLibrary = true, .gpu3dPercent = 2})); // e.g. a game's settings launcher
}

TEST_CASE("unknown apps need both fullscreen and real GPU work") {
    CHECK(isGame({.coversMonitor = true, .gpu3dPercent = 60}));      // a game from an unknown store
    CHECK_FALSE(isGame({.coversMonitor = true, .gpu3dPercent = 1})); // fullscreen slideshow
    CHECK_FALSE(isGame({.gpu3dPercent = 90}));                       // windowed 3D app (Blender)
}

TEST_CASE("user lists and exclusions win") {
    CHECK(isGame({.userAlways = true}));
    CHECK_FALSE(isGame({.userNever = true, .inLibrary = true, .coversMonitor = true, .gpu3dPercent = 90}));
    CHECK_FALSE(isGame({.excluded = true, .coversMonitor = true, .gpu3dPercent = 90})); // fullscreen browser video
    CHECK(isGame({.userAlways = true, .excluded = true}));
}

TEST_CASE("excluded executables are matched case-insensitively") {
    CHECK(rec::isExcludedExecutable(L"chrome.exe"));
    CHECK(rec::isExcludedExecutable(L"VLC.EXE"));
    CHECK(rec::isExcludedExecutable(L"steamwebhelper.exe"));
    CHECK_FALSE(rec::isExcludedExecutable(L"Cyberpunk2077.exe"));
}

TEST_CASE("product names skip engine boilerplate") {
    CHECK(rec::usableProductName("Cyberpunk 2077", "") == "Cyberpunk 2077");
    CHECK(rec::usableProductName("Unreal Engine", "Fortnite") == "Fortnite");
    CHECK(rec::usableProductName("BootstrapPackagedGame", "") == "");
    CHECK(rec::usableProductName("  Hades II  ", "") == "Hades II");
    CHECK(rec::usableProductName("", "Unity Player") == "");
}

TEST_CASE("VDF parsing flattens nested keys") {
    const auto kv = rec::parseVdf(R"(
"libraryfolders"
{
    "0"
    {
        "path"      "C:\\Program Files (x86)\\Steam"
        "apps" { "730" "123" }
    }
    "1" { "path" "D:\\SteamLibrary" }
}
// comment
"AppState" { "name" "Counter-Strike 2" "installdir" "Counter-Strike Global Offensive" }
)");
    const auto first = kv.find("libraryfolders/0/path");
    REQUIRE(first != kv.end());
    // VDF escapes backslashes; the parser unescapes them.
    CHECK(first->second == R"(C:\Program Files (x86)\Steam)");
    CHECK(kv.find("libraryfolders/1/path")->second == R"(D:\SteamLibrary)");
    CHECK(kv.find("libraryfolders/0/apps/730")->second == "123");
    CHECK(kv.find("appstate/name")->second == "Counter-Strike 2");
}

TEST_CASE("library lookup matches the deepest install folder, case-insensitively") {
    rec::GameLibrary lib;
    lib.addRoot(LR"(D:\SteamLibrary\steamapps\common)");
    lib.addInstall(LR"(D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive)", "Counter-Strike 2");
    lib.addKnownExe(LR"(C:\Games\Indie\game.exe)");

    auto m = lib.lookup(LR"(d:\steamlibrary\steamapps\common\counter-strike global offensive\game\bin\win64\cs2.exe)");
    CHECK(m.inLibrary);
    CHECK(m.name == "Counter-Strike 2");
    m = lib.lookup(LR"(D:\SteamLibrary\steamapps\common\Other Game\other.exe)");
    CHECK(m.inLibrary);
    CHECK(m.name.empty());
    m = lib.lookup(LR"(C:\Games\Indie\game.exe)");
    CHECK(m.knownToWindows);
    CHECK_FALSE(m.inLibrary);
    CHECK_FALSE(lib.lookup(LR"(C:\Windows\notepad.exe)").inLibrary);
}
