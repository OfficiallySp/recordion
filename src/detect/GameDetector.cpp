#include "detect/GameDetector.h"

#include "core/Log.h"
#include "core/ThreadQos.h"
#include "core/Utf.h"
#include "detect/GameRules.h"

#include <dwmapi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <format>
#include <vector>

namespace rec {
namespace {

constexpr UINT kConfirmMs = 3000;  // a window must stay in front this long before it counts
constexpr UINT kRecheckMs = 10000; // catches games that go fullscreen later or were already running
constexpr DWORD kGpuSampleMs = 500;
constexpr UINT kMsgGameExited = WM_APP + 1;

thread_local GameDetector* t_detector = nullptr;

std::filesystem::path processPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD len = static_cast<DWORD>(std::size(buf));
    std::filesystem::path p;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) p = std::wstring(buf, len);
    CloseHandle(h);
    return p;
}

// Fullscreen, or a borderless window covering its whole display.
bool coversMonitor(HWND hwnd) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(hwnd, &r);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    const RECT& m = mi.rcMonitor;
    return r.left <= m.left && r.top <= m.top && r.right >= m.right && r.bottom >= m.bottom;
}

// The process's 3D engine utilisation (all GPUs and engines), sampled over kGpuSampleMs. Reads
// performance counters only; the process itself is never touched.
double gpu3dPercent(DWORD pid) {
    const std::wstring wildcard = std::format(L"\\GPU Engine(pid_{}_*engtype_3D)\\Utilization Percentage", pid);
    DWORD size = 0;
    if (PdhExpandWildCardPathW(nullptr, wildcard.c_str(), nullptr, &size, 0) != PDH_MORE_DATA || size == 0) return 0;
    std::vector<wchar_t> paths(size);
    if (PdhExpandWildCardPathW(nullptr, wildcard.c_str(), paths.data(), &size, 0) != ERROR_SUCCESS) return 0;

    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return 0;
    std::vector<PDH_HCOUNTER> counters;
    for (const wchar_t* p = paths.data(); *p; p += wcslen(p) + 1) {
        PDH_HCOUNTER c = nullptr;
        if (PdhAddCounterW(query, p, 0, &c) == ERROR_SUCCESS) counters.push_back(c);
    }
    double total = 0;
    if (!counters.empty() && PdhCollectQueryData(query) == ERROR_SUCCESS) {
        Sleep(kGpuSampleMs);
        if (PdhCollectQueryData(query) == ERROR_SUCCESS) {
            for (PDH_HCOUNTER c : counters) {
                PDH_FMT_COUNTERVALUE v{};
                if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) == ERROR_SUCCESS)
                    total += v.doubleValue;
            }
        }
    }
    PdhCloseQuery(query);
    return total;
}

std::string versionString(const std::filesystem::path& exe, const wchar_t* field) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
    if (!size) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) return {};
    struct LangCodePage {
        WORD language, codePage;
    }* translation = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translation), &len) ||
        len < sizeof(LangCodePage))
        return {};
    const std::wstring key =
        std::format(L"\\StringFileInfo\\{:04x}{:04x}\\{}", translation->language, translation->codePage, field);
    wchar_t* value = nullptr;
    if (!VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&value), &len) || len == 0) return {};
    return narrow(value);
}

std::string windowTitle(HWND hwnd) {
    wchar_t buf[256];
    const int n = GetWindowTextW(hwnd, buf, static_cast<int>(std::size(buf)));
    return n > 0 ? narrow(std::wstring_view(buf, static_cast<size_t>(n))) : std::string{};
}

// Entries may be full paths or bare file names.
bool listed(const std::vector<std::string>& list, const std::filesystem::path& exe) {
    const std::wstring full = lowerPath(exe);
    const std::wstring file = lowerPath(exe.filename());
    for (const auto& entry : list) {
        const std::wstring e = lowerPath(widen(entry));
        if (e == full || e == file) return true;
    }
    return false;
}

} // namespace

GameDetector::GameDetector(HWND window, UINT startedMessage, UINT endedMessage)
    : notifyWindow_(window), startedMessage_(startedMessage), endedMessage_(endedMessage) {}

GameDetector::~GameDetector() { stop(); }

void GameDetector::start(DetectionSettings settings) {
    if (thread_.joinable()) return;
    setSettings(std::move(settings));
    thread_ = std::thread([this] { run(); });
    while (!threadId_.load()) Sleep(1); // its message queue exists once the id is published
}

void GameDetector::stop() {
    if (!thread_.joinable()) return;
    PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    thread_.join();
    threadId_ = 0;
}

void GameDetector::setSettings(DetectionSettings settings) {
    std::scoped_lock lock(settingsMutex_);
    settings_ = std::move(settings);
}

void CALLBACK GameDetector::onWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (t_detector) t_detector->considerForeground(false);
}

void CALLBACK GameDetector::onProcessExit(void* context, BOOLEAN) {
    auto* self = static_cast<GameDetector*>(context);
    PostThreadMessageW(self->threadId_, kMsgGameExited, 0, 0);
}

void GameDetector::run() {
    qos::setThreadName(L"game detector");
    qos::setEcoQos();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // create the message queue
    t_detector = this;
    threadId_ = GetCurrentThreadId();

    library_.build();
    // Out of context: Windows calls us through our own message loop; nothing is injected anywhere.
    HWINEVENTHOOK hooks[] = {
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &GameDetector::onWinEvent, 0, 0,
                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS),
        SetWinEventHook(EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND, nullptr, &GameDetector::onWinEvent, 0, 0,
                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS),
    };
    UINT_PTR confirmTimer = 0;
    const UINT_PTR recheckTimer = SetTimer(nullptr, 0, kRecheckMs, nullptr);
    considerForeground(true); // a game may already be running

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == nullptr && msg.message == WM_TIMER) {
            if (msg.wParam == recheckTimer) {
                if (!gamePid_ && !candidate_) evaluate(GetForegroundWindow());
            } else if (msg.wParam == confirmTimer) {
                KillTimer(nullptr, msg.wParam);
                confirmTimer = 0;
                const HWND c = std::exchange(candidate_, nullptr);
                evaluate(c);
            }
            continue;
        }
        if (msg.hwnd == nullptr && msg.message == kMsgGameExited) {
            gameExited();
            considerForeground(true);
            continue;
        }
        if (msg.hwnd == nullptr && msg.message == WM_APP + 2) { // considerForeground: (re)arm the timer
            if (confirmTimer) KillTimer(nullptr, confirmTimer);
            confirmTimer = SetTimer(nullptr, 0, static_cast<UINT>(msg.wParam), nullptr);
            continue;
        }
        DispatchMessageW(&msg);
    }

    for (HWINEVENTHOOK h : hooks)
        if (h) UnhookWinEvent(h);
    KillTimer(nullptr, recheckTimer);
    if (confirmTimer) KillTimer(nullptr, confirmTimer);
    if (gameWait_) UnregisterWaitEx(gameWait_, INVALID_HANDLE_VALUE);
    gameWait_ = nullptr;
    if (gameProcess_) CloseHandle(gameProcess_);
    gameProcess_ = nullptr;
    t_detector = nullptr;
}

void GameDetector::considerForeground(bool immediate) {
    if (gamePid_) return; // one game at a time, until it exits
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    candidate_ = fg;
    PostThreadMessageW(threadId_, WM_APP + 2, immediate ? 1 : kConfirmMs, 0);
}

void GameDetector::evaluate(HWND hwnd) {
    if (!hwnd || gamePid_ || GetForegroundWindow() != hwnd || !IsWindowVisible(hwnd)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) return;
    const auto exe = processPath(pid);
    if (exe.empty()) return;

    DetectionSettings settings;
    {
        std::scoped_lock lock(settingsMutex_);
        settings = settings_;
    }
    if (!settings.enabled) return;

    const std::wstring key = lowerPath(exe);
    GameSignals s;
    s.userAlways = listed(settings.alwaysRecord, exe);
    s.userNever = listed(settings.neverRecord, exe);
    s.excluded = isExcludedExecutable(exe.filename().wstring());
    const auto match = library_.lookup(exe);
    s.inLibrary = match.inLibrary;
    s.knownToWindows = match.knownToWindows;
    s.coversMonitor = coversMonitor(hwnd);

    bool game = false;
    if (s.userNever) game = false;
    else if (confirmedGames_.contains(key) || s.userAlways) game = true;
    else if (!s.excluded) {
        // Sample the GPU only when it can change the answer.
        const bool store = s.inLibrary || s.knownToWindows;
        if (store != s.coversMonitor) s.gpu3dPercent = gpu3dPercent(pid);
        game = isGame(s);
    }
    log::debug("games: {} library={} windows={} fullscreen={} gpu={:.0f}% -> {}", narrow(exe.filename().wstring()),
               s.inLibrary, s.knownToWindows, s.coversMonitor, s.gpu3dPercent, game ? "game" : "not a game");
    if (!game) return;

    // SYNCHRONIZE is all we need to learn when it exits. No VM_READ: that would look like a cheat.
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return;
    HANDLE wait = nullptr;
    if (!RegisterWaitForSingleObject(&wait, process, &GameDetector::onProcessExit, this, INFINITE,
                                     WT_EXECUTEONLYONCE)) {
        CloseHandle(process);
        return;
    }
    confirmedGames_.insert(key);
    gamePid_ = pid;
    gameProcess_ = process;
    gameWait_ = wait;

    auto* g = new DetectedGame;
    g->pid = pid;
    g->window = hwnd;
    g->monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    g->exe = exe;
    g->name = match.name;
    if (g->name.empty()) g->name = usableProductName(versionString(exe, L"ProductName"), versionString(exe, L"FileDescription"));
    if (g->name.empty()) g->name = windowTitle(hwnd);
    if (g->name.empty()) g->name = narrow(exe.stem().wstring());
    log::info("games: detected \"{}\" ({}, pid {})", g->name, narrow(exe.filename().wstring()), pid);
    if (!PostMessageW(notifyWindow_, startedMessage_, 0, reinterpret_cast<LPARAM>(g))) delete g;
}

void GameDetector::gameExited() {
    if (!gamePid_) return;
    if (gameWait_) UnregisterWaitEx(gameWait_, INVALID_HANDLE_VALUE);
    if (gameProcess_) CloseHandle(gameProcess_);
    gameWait_ = gameProcess_ = nullptr;
    log::info("games: pid {} exited", gamePid_);
    PostMessageW(notifyWindow_, endedMessage_, gamePid_, 0);
    gamePid_ = 0;
}

} // namespace rec
