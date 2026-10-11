#include "output/FileNaming.h"

#include "core/Utf.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>

namespace rec {
namespace {

constexpr size_t kMaxComponentBytes = 120; // leaves room for the folder, suffixes and extension

bool reservedDeviceName(std::string_view s) {
    // Reserved regardless of extension: "CON", "con.txt", "COM1" ...
    const std::string_view stem = s.substr(0, s.find('.'));
    static constexpr std::array kNames{"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6",
                                       "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
                                       "LPT7", "LPT8", "LPT9"};
    return std::ranges::any_of(kNames, [&](std::string_view n) {
        return stem.size() == n.size() && std::ranges::equal(stem, n, [](char a, char b) {
                   return std::toupper(static_cast<unsigned char>(a)) == b;
               });
    });
}

// Cuts at a UTF-8 character boundary.
std::string truncateUtf8(std::string s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
    return s;
}

std::string formatDuration(int64_t seconds) {
    if (seconds < 60) return std::format("{}s", seconds);
    if (seconds < 3600) return std::format("{}m{:02}s", seconds / 60, seconds % 60);
    return std::format("{}h{:02}m{:02}s", seconds / 3600, (seconds / 60) % 60, seconds % 60);
}

} // namespace

std::string sanitizePathComponent(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || std::string_view(R"(<>:"/\|?*)").find(c) != std::string_view::npos) {
            if (!out.empty() && out.back() != ' ') out += ' ';
            continue;
        }
        out += c;
    }
    // Collapse runs of spaces left by removed characters.
    out.erase(std::unique(out.begin(), out.end(), [](char a, char b) { return a == ' ' && b == ' '; }), out.end());
    out = truncateUtf8(std::move(out), kMaxComponentBytes);
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (out.empty()) return "Unknown";
    if (reservedDeviceName(out)) out = "_" + out;
    return out;
}

std::string expandFileTemplate(std::string_view tmpl, const NamingContext& ctx) {
    const auto& t = ctx.localTime;
    std::string out;
    for (size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == '{') {
            const size_t close = tmpl.find('}', i);
            if (close != std::string_view::npos) {
                const std::string_view token = tmpl.substr(i + 1, close - i - 1);
                if (token == "game") out += ctx.game;
                else if (token == "type") out += ctx.type;
                else if (token == "date") out += std::format("{:04}-{:02}-{:02}", t.wYear, t.wMonth, t.wDay);
                else if (token == "time") out += std::format("{:02}-{:02}-{:02}", t.wHour, t.wMinute, t.wSecond);
                else if (token == "duration") out += formatDuration(ctx.durationSeconds);
                else out += tmpl.substr(i, close - i + 1); // unknown token: keep literally
                i = close + 1;
                continue;
            }
        }
        out += tmpl[i++];
    }
    return out;
}

std::filesystem::path makeOutputPath(const std::filesystem::path& root, std::string_view tmpl, const NamingContext& ctx) {
    const std::filesystem::path folder = root / widen(sanitizePathComponent(ctx.game));
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);

    std::string base = expandFileTemplate(tmpl.empty() ? "{game} {date} {time} {type}" : tmpl, ctx);
    base = sanitizePathComponent(base);
    std::filesystem::path path = folder / widen(base + ".mp4");
    for (int n = 2; std::filesystem::exists(path, ec) && n < 1000; ++n)
        path = folder / widen(std::format("{} ({}).mp4", base, n));
    return path;
}

} // namespace rec
