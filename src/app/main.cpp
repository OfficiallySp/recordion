#include "app/App.h"
#include "app/Bench.h"
#include "core/Log.h"
#include "core/Paths.h"

#include <windows.h>
#include <shellapi.h>

#include <wil/resource.h>
#include <winrt/base.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::vector<std::wstring> commandLineArgs() {
    int argc = 0;
    wil::unique_hlocal_ptr<PWSTR[]> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return args;
}

// GUI-subsystem exe: when run from a terminal, print to it (unless output is already redirected).
void attachParentConsole() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    const auto args = commandLineArgs();
#ifdef NDEBUG
    const auto logLevel = rec::log::Level::Info;
#else
    const auto logLevel = rec::log::Level::Debug;
#endif

    if (!args.empty() && args[0] == L"--bench") {
        attachParentConsole();
        rec::log::init(rec::paths::logDir(), logLevel);
        const int rc = rec::runBench(args);
        rec::log::shutdown();
        fflush(stdout);
        return rc;
    }

    wil::unique_mutex_nothrow singleInstance(CreateMutexW(nullptr, FALSE, L"Local\\Recordion.SingleInstance"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(rec::App::kWindowClass, nullptr))
            PostMessageW(existing, rec::App::kMsgActivate, 0, 0);
        return 0;
    }

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    rec::log::init(rec::paths::logDir(), logLevel);
    rec::App app;
    const int rc = app.run(instance);
    rec::log::shutdown();
    return rc;
}
