#pragma once

#include "core/Settings.h"
#include "detect/GameLibrary.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace rec {

struct DetectedGame {
    DWORD pid = 0;
    HWND window = nullptr;
    HMONITOR monitor = nullptr;
    std::filesystem::path exe;
    std::string name;
};

// Watches the foreground window and decides when a game is being played. Event driven (a WinEvent
// hook, out of context: nothing is injected) on its own low-priority thread; a game process is
// only ever opened with PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE.
// Posts `startedMessage` (lParam: DetectedGame*, receiver deletes) and `endedMessage` (wParam: pid).
class GameDetector {
public:
    GameDetector(HWND window, UINT startedMessage, UINT endedMessage);
    ~GameDetector();
    GameDetector(const GameDetector&) = delete;
    GameDetector& operator=(const GameDetector&) = delete;

    void start(DetectionSettings settings);
    void stop();
    void setSettings(DetectionSettings settings);

private:
    static void CALLBACK onWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static void CALLBACK onProcessExit(void* context, BOOLEAN);
    void run();
    void considerForeground(bool immediate);
    void evaluate(HWND hwnd);
    void gameExited();

    HWND notifyWindow_;
    UINT startedMessage_;
    UINT endedMessage_;
    std::thread thread_;
    std::atomic<DWORD> threadId_{0};

    std::mutex settingsMutex_;
    DetectionSettings settings_;

    // Detector-thread state.
    GameLibrary library_;
    HWND candidate_ = nullptr;
    std::unordered_set<std::wstring> confirmedGames_; // lower-case exe paths decided to be games
    DWORD gamePid_ = 0;
    HANDLE gameProcess_ = nullptr;
    HANDLE gameWait_ = nullptr;
};

} // namespace rec
