#pragma once

#include <filesystem>

namespace rec::paths {

std::filesystem::path videosDir();        // FOLDERID_Videos
std::filesystem::path defaultOutputDir(); // <Videos>\Recordion
std::filesystem::path configDir();        // %APPDATA%\Recordion (settings, game list)
std::filesystem::path logDir();           // %LOCALAPPDATA%\Recordion\logs
std::filesystem::path executablePath();

} // namespace rec::paths
