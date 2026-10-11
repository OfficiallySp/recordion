#include "output/FileNaming.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using rec::expandFileTemplate;
using rec::NamingContext;
using rec::sanitizePathComponent;

namespace {

NamingContext sampleContext() {
    NamingContext ctx;
    ctx.game = "Cyberpunk 2077";
    ctx.type = "Replay";
    ctx.localTime = {2026, 10, 0, 11, 21, 34, 7, 0};
    ctx.durationSeconds = 90;
    return ctx;
}

} // namespace

TEST_CASE("default template gives game, date, time and type") {
    CHECK(expandFileTemplate("{game} {date} {time} {type}", sampleContext()) ==
          "Cyberpunk 2077 2026-10-11 21-34-07 Replay");
}

TEST_CASE("template tokens") {
    const auto ctx = sampleContext();
    CHECK(expandFileTemplate("{type}_{duration}", ctx) == "Replay_1m30s");
    CHECK(expandFileTemplate("{unknown} {game}", ctx) == "{unknown} Cyberpunk 2077");
    CHECK(expandFileTemplate("no tokens", ctx) == "no tokens");
    auto c = ctx;
    c.durationSeconds = 3725;
    CHECK(expandFileTemplate("{duration}", c) == "1h02m05s");
    c.durationSeconds = 42;
    CHECK(expandFileTemplate("{duration}", c) == "42s");
}

TEST_CASE("sanitizing strips illegal characters and reserved names") {
    CHECK(sanitizePathComponent("Half-Life: Alyx") == "Half-Life Alyx");
    CHECK(sanitizePathComponent(R"(a<b>c"d/e\f|g?h*i)") == "a b c d e f g h i");
    CHECK(sanitizePathComponent("Game...  ") == "Game");
    CHECK(sanitizePathComponent("  ") == "Unknown");
    CHECK(sanitizePathComponent("") == "Unknown");
    CHECK(sanitizePathComponent("CON") == "_CON");
    CHECK(sanitizePathComponent("nul.txt") == "_nul.txt");
    CHECK(sanitizePathComponent("Console") == "Console");
    CHECK(sanitizePathComponent("tab\there") == "tab here");
    CHECK(sanitizePathComponent("ゼルダの伝説") == "ゼルダの伝説");
}

TEST_CASE("long names are clamped on a UTF-8 boundary") {
    std::string longName;
    for (int i = 0; i < 100; ++i) longName += "é"; // 2 bytes each
    const std::string s = sanitizePathComponent(longName);
    CHECK(s.size() <= 120);
    CHECK(s.size() % 2 == 0);
}

TEST_CASE("output path goes in a per-game folder and avoids collisions") {
    const auto root = std::filesystem::temp_directory_path() / "recordion_naming_test";
    std::filesystem::remove_all(root);
    auto ctx = sampleContext();
    ctx.game = "Half-Life: Alyx";

    const auto first = rec::makeOutputPath(root, "{game} {date} {time} {type}", ctx);
    CHECK(first == root / "Half-Life Alyx" / "Half-Life Alyx 2026-10-11 21-34-07 Replay.mp4");
    CHECK(std::filesystem::is_directory(root / "Half-Life Alyx"));

    std::ofstream(first) << "x";
    const auto second = rec::makeOutputPath(root, "{game} {date} {time} {type}", ctx);
    CHECK(second == root / "Half-Life Alyx" / "Half-Life Alyx 2026-10-11 21-34-07 Replay (2).mp4");
    std::filesystem::remove_all(root);
}
