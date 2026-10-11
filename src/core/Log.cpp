#include "core/Log.h"

#include "core/Utf.h"

#include <windows.h>

#include <cstdio>
#include <mutex>

namespace rec::log {
namespace {

constexpr uintmax_t kMaxLogBytes = 4 * 1024 * 1024;

std::mutex g_mutex;
FILE* g_file = nullptr;
Level g_minLevel = Level::Info;

const char* levelName(Level l) {
    switch (l) {
    case Level::Debug: return "DBG";
    case Level::Info: return "INF";
    case Level::Warn: return "WRN";
    case Level::Error: return "ERR";
    }
    return "???";
}

} // namespace

void init(const std::filesystem::path& dir, Level minLevel) {
    std::scoped_lock lock(g_mutex);
    g_minLevel = minLevel;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / L"recordion.log";
    if (std::filesystem::file_size(path, ec) > kMaxLogBytes && !ec)
        std::filesystem::rename(path, dir / L"recordion.old.log", ec);
    g_file = _wfsopen(path.c_str(), L"ab", _SH_DENYWR);
}

void shutdown() {
    std::scoped_lock lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void write(Level level, std::string_view message) {
    if (level < g_minLevel) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::string line = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{}] {:5} {}\n", t.wYear, t.wMonth,
                                         t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, levelName(level),
                                         GetCurrentThreadId(), message);
#ifndef NDEBUG
    OutputDebugStringW(widen(line).c_str());
#endif
    std::scoped_lock lock(g_mutex);
    if (g_file) {
        // Logging is rare (a few lines a minute), so flush every line: a crash must not lose the
        // messages that explain it.
        fwrite(line.data(), 1, line.size(), g_file);
        fflush(g_file);
    }
}

} // namespace rec::log
