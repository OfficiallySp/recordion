#pragma once

#include "app/Hotkeys.h"
#include "app/Notifier.h"
#include "app/Tray.h"
#include "core/Settings.h"
#include "detect/GameDetector.h"

#include <windows.h>

#include <filesystem>
#include <memory>
#include <optional>

namespace rec {

class CaptureSession;
class ClipSaver;
class Recorder;

class App {
public:
    static constexpr const wchar_t* kWindowClass = L"Recordion.MainWindow";
    static constexpr UINT kMsgTray = WM_APP + 1;
    static constexpr UINT kMsgActivate = WM_APP + 2; // a second instance was launched
    static constexpr UINT kMsgSaved = WM_APP + 3;    // lParam: SaveResult*
    static constexpr UINT kMsgSession = WM_APP + 4;  // wParam: kSessionFailed / kSessionEnded
    static constexpr UINT kMsgGameStarted = WM_APP + 5; // lParam: DetectedGame*
    static constexpr UINT kMsgGameEnded = WM_APP + 6;   // wParam: pid

    App();
    ~App();
    int run(HINSTANCE instance);

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void loadSettings();
    void applyHotkeys();
    void onHotkey(HotkeyAction action);
    void showTrayMenu();
    void openOutputFolder();
    void editSettingsFile();

    bool replayOn() const;
    void startReplay();
    void stopReplay();
    void saveReplay();
    bool recording() const;
    void startRecording();
    void stopRecording();
    void onSaved(LPARAM result);
    void onGameStarted(LPARAM game);
    void onGameEnded(DWORD pid);
    void updateTray();
    void toast(std::wstring_view title, std::wstring_view detail, Notifier::Kind kind);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    UINT taskbarCreatedMsg_ = 0;
    std::filesystem::path settingsFile_;
    Settings settings_;
    Tray tray_;
    HotkeyManager hotkeys_;
    Notifier notifier_;
    std::unique_ptr<ClipSaver> saver_;
    std::unique_ptr<Recorder> recorder_;
    std::unique_ptr<CaptureSession> session_;
    std::unique_ptr<GameDetector> detector_;
    std::optional<DetectedGame> game_; // the game being played, if any
    // The session was started only for a recording (instant replay is off): stop it afterwards.
    bool sessionOnlyForRecording_ = false;
};

} // namespace rec
