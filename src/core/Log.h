#pragma once

#include <filesystem>
#include <format>
#include <string_view>

namespace rec::log {

enum class Level { Debug, Info, Warn, Error };

// Opens <dir>/recordion.log (rotated to recordion.old.log past 4 MB). Safe to skip in tests:
// without init() messages only go to the debugger.
void init(const std::filesystem::path& dir, Level minLevel);
void shutdown();
void write(Level level, std::string_view message);

template <class... A>
void debug(std::format_string<A...> fmt, A&&... args) { write(Level::Debug, std::format(fmt, std::forward<A>(args)...)); }
template <class... A>
void info(std::format_string<A...> fmt, A&&... args) { write(Level::Info, std::format(fmt, std::forward<A>(args)...)); }
template <class... A>
void warn(std::format_string<A...> fmt, A&&... args) { write(Level::Warn, std::format(fmt, std::forward<A>(args)...)); }
template <class... A>
void error(std::format_string<A...> fmt, A&&... args) { write(Level::Error, std::format(fmt, std::forward<A>(args)...)); }

} // namespace rec::log
