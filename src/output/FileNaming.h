#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace rec {

struct NamingContext {
    std::string game = "Desktop"; // folder and {game}
    std::string type = "Replay";  // "Replay" or "Recording"
    SYSTEMTIME localTime{};       // clip start (replays) or recording start, local time
    int64_t durationSeconds = 0;
};

// Makes a string safe as a single Windows path component: strips <>:"/\|?* and control
// characters, trailing dots/spaces, reserved device names (CON, NUL, COM1...), and clamps length.
// Never returns an empty string.
std::string sanitizePathComponent(std::string_view name);

// Expands {game} {date} {time} {type} {duration}. Date is YYYY-MM-DD, time HH-MM-SS (colons are not
// allowed in file names), duration like "1m30s".
std::string expandFileTemplate(std::string_view tmpl, const NamingContext& ctx);

// <root>\<game>\<expanded template>.mp4, with " (2)", " (3)"... appended if the file exists.
// Creates the folder.
std::filesystem::path makeOutputPath(const std::filesystem::path& root, std::string_view tmpl, const NamingContext& ctx);

} // namespace rec
