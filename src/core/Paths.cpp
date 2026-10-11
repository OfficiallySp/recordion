#include "core/Paths.h"

#include <windows.h>
#include <shlobj.h>

#include <wil/resource.h>

namespace rec::paths {
namespace {

std::filesystem::path knownFolder(REFKNOWNFOLDERID id) {
    wil::unique_cotaskmem_string path;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path))) return {};
    return path.get();
}

} // namespace

std::filesystem::path videosDir() { return knownFolder(FOLDERID_Videos); }

std::filesystem::path defaultOutputDir() { return videosDir() / L"Recordion"; }

std::filesystem::path configDir() { return knownFolder(FOLDERID_RoamingAppData) / L"Recordion"; }

std::filesystem::path logDir() { return knownFolder(FOLDERID_LocalAppData) / L"Recordion" / L"logs"; }

std::filesystem::path executablePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

} // namespace rec::paths
