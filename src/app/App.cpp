#include "app/App.h"

#include "app/resource.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/ThreadQos.h"
#include "core/Utf.h"
#include "core/Clock.h"
#include "output/ClipSaver.h"
#include "output/Recorder.h"
#include "pipeline/CaptureSession.h"

#include <shellapi.h>

#include <format>

namespace rec {
namespace {

enum MenuId : UINT {
    kMenuToggleReplay = 100,
    kMenuSaveReplay,
    kMenuToggleRecording,
    kMenuOpenFolder,
    kMenuEditSettings,
    kMenuReloadSettings,
    kMenuExit,
};

std::wstring formatClipLength(double seconds) {
    const int s = static_cast<int>(seconds + 0.5);
    return s < 60 ? std::format(L"{} s", s) : std::format(L"{}:{:02}", s / 60, s % 60);
}

} // namespace

App::App() = default;
App::~App() = default;

int App::run(HINSTANCE instance) {
    instance_ = instance;
    qos::setEcoQos(); // the UI thread is never latency-critical
    loadSettings();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &App::wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return 1;

    // Hidden top-level window (not HWND_MESSAGE): it must receive the TaskbarCreated broadcast.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"Recordion", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            instance, this);
    if (!hwnd_) return 1;

    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    icon_ = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                          GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    tray_.create(hwnd_, kMsgTray, icon_, L"Recordion");
    notifier_.create(instance);
    saver_ = std::make_unique<ClipSaver>(hwnd_, kMsgSaved);
    recorder_ = std::make_unique<Recorder>(hwnd_, kMsgSaved);
    applyHotkeys();
    log::info("Recordion started; output folder {}", narrow(settings_.outputRoot().wstring()));
    if (settings_.replay.desktopAlwaysOn) startReplay();
    updateTray();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (recording()) recorder_->stop(clock::now100ns());
    recorder_.reset(); // finishes the recording's file
    stopReplay();
    saver_.reset(); // finishes the save in progress
    hotkeys_.clear();
    tray_.destroy();
    if (icon_) DestroyIcon(icon_);
    log::info("Recordion exited");
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
        return self->handleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::handleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_ != 0) {
        tray_.recreate();
        return 0;
    }
    switch (msg) {
    case WM_HOTKEY:
        if (auto action = hotkeys_.actionFromId(wp)) onHotkey(*action);
        return 0;
    case kMsgTray:
        // NOTIFYICON_VERSION_4: LOWORD(lp) is the event.
        if (LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == NIN_SELECT) showTrayMenu();
        return 0;
    case kMsgActivate:
        showTrayMenu();
        return 0;
    case kMsgSaved:
        onSaved(lp);
        return 0;
    case kMsgSession:
        if (session_) {
            const bool failed = wp == kSessionFailed;
            if (recording()) recorder_->stop(clock::now100ns()); // keeps what was captured
            stopReplay();
            toast(failed ? L"Instant replay stopped" : L"Capture ended",
                  failed ? L"The encoder stopped working. Check the log for details." : L"The captured display went away.",
                  failed ? Notifier::Kind::Error : Notifier::Kind::Info);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kMenuToggleReplay: onHotkey(HotkeyAction::ToggleReplayBuffer); break;
        case kMenuSaveReplay: onHotkey(HotkeyAction::SaveReplay); break;
        case kMenuToggleRecording: onHotkey(HotkeyAction::ToggleRecording); break;
        case kMenuOpenFolder: openOutputFolder(); break;
        case kMenuEditSettings: editSettingsFile(); break;
        case kMenuReloadSettings: {
            const bool wasOn = replayOn();
            stopReplay();
            loadSettings();
            applyHotkeys();
            if (wasOn) startReplay();
            break;
        }
        case kMenuExit: DestroyWindow(hwnd_); break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void App::loadSettings() {
    settingsFile_ = paths::configDir() / L"settings.json";
    settings_ = Settings::load(settingsFile_);
    // Write back so new fields appear in the file for hand-editing.
    settings_.save(settingsFile_);
}

void App::applyHotkeys() {
    const auto failures = hotkeys_.apply(hwnd_, settings_.hotkeys);
    if (failures.empty()) return;
    std::wstring text;
    for (const auto& f : failures)
        text += widen(std::string(hotkeyActionName(f.action)) + " (" + f.combo + "): " + f.reason + "\n");
    tray_.notify(L"Some hotkeys could not be registered", text, true);
}

void App::onHotkey(HotkeyAction action) {
    switch (action) {
    case HotkeyAction::SaveReplay:
        saveReplay();
        break;
    case HotkeyAction::ToggleReplayBuffer:
        if (replayOn() && recording() && !sessionOnlyForRecording_) {
            sessionOnlyForRecording_ = true; // the recording keeps the capture running until it stops
            toast(L"Instant replay off", L"Recording continues", Notifier::Kind::Info);
        } else if (replayOn() && sessionOnlyForRecording_) {
            sessionOnlyForRecording_ = false; // already capturing for a recording: just keep it
            toast(L"Instant replay on", L"", Notifier::Kind::Info);
        } else if (replayOn()) {
            stopReplay();
            toast(L"Instant replay off", L"", Notifier::Kind::Info);
        } else {
            startReplay();
            if (replayOn()) {
                toast(L"Instant replay on",
                      std::format(L"Press {} to save the last {}", widen(settings_.hotkeys.saveReplay),
                                  formatClipLength(settings_.replay.seconds)),
                      Notifier::Kind::Info);
            }
        }
        break;
    case HotkeyAction::ToggleRecording:
    case HotkeyAction::ToggleDesktopRecording: // the same until game sessions exist
        if (recording()) stopRecording();
        else startRecording();
        break;
    case HotkeyAction::Count:
        break;
    }
    updateTray();
}

bool App::replayOn() const { return session_ && session_->running(); }

void App::startReplay() {
    if (replayOn()) return;
    SessionConfig cfg;
    // Game sessions arrive with automatic game detection; until then this is the desktop.
    cfg.target.monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    cfg.name = "Desktop";
    cfg.settings = settings_;
    cfg.notifyWindow = hwnd_;
    cfg.notifyMessage = kMsgSession;
    session_ = std::make_unique<CaptureSession>(std::move(cfg));
    if (!session_->start()) {
        session_.reset();
        toast(L"Instant replay couldn't start", L"No supported GPU encoder was found. See the log.",
              Notifier::Kind::Error);
    }
    updateTray();
}

void App::stopReplay() {
    if (!session_) return;
    session_->stop();
    session_.reset();
    sessionOnlyForRecording_ = false;
    updateTray();
}

bool App::recording() const { return recorder_ && recorder_->active(); }

void App::startRecording() {
    if (!replayOn()) {
        startReplay(); // shows its own error toast if capture can't start
        if (!replayOn()) return;
        sessionOnlyForRecording_ = true;
    }
    if (!session_->startRecording(*recorder_, 0)) return;
    toast(L"Recording started", std::format(L"Press {} to stop", widen(settings_.hotkeys.toggleRecording)),
          Notifier::Kind::Info);
    updateTray();
}

void App::stopRecording() {
    recorder_->stop(clock::now100ns()); // the file is finished on the recorder thread; onSaved reports it
    updateTray();
}

void App::saveReplay() {
    if (!replayOn()) {
        toast(L"Instant replay is off", std::format(L"Turn it on with {}", widen(settings_.hotkeys.toggleReplayBuffer)),
              Notifier::Kind::Info);
        return;
    }
    session_->saveReplay(*saver_, settings_.replay.seconds);
}

void App::onSaved(LPARAM lp) {
    std::unique_ptr<SaveResult> r(reinterpret_cast<SaveResult*>(lp));
    const std::wstring what = widen(r->type);
    if (r->ok && r->error.empty()) {
        toast(what + L" saved", std::format(L"{} · {}", widen(r->game), formatClipLength(r->seconds)),
              Notifier::Kind::Success);
    } else if (r->ok) {
        toast(what + L" saved, but cut short", widen(r->error), Notifier::Kind::Error);
    } else {
        toast(what + L" not saved", widen(r->error), Notifier::Kind::Error);
    }
    if (r->type == "Recording" && !recording() && sessionOnlyForRecording_) stopReplay();
    updateTray();
}

void App::toast(std::wstring_view title, std::wstring_view detail, Notifier::Kind kind) {
    notifier_.show(title, detail, kind, settings_.general.notificationSound);
}

void App::updateTray() {
    tray_.setTooltip(recording()                                  ? L"Recordion – recording"
                     : replayOn() && !sessionOnlyForRecording_ ? L"Recordion – instant replay on"
                                                                : L"Recordion");
}

void App::showTrayMenu() {
    const bool on = replayOn() && !sessionOnlyForRecording_;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (on ? MF_CHECKED : 0), kMenuToggleReplay, L"Instant replay");
    AppendMenuW(menu, MF_STRING | (on ? 0 : MF_GRAYED), kMenuSaveReplay, L"Save replay");
    const std::wstring recordItem = recording() ? std::format(L"Stop recording ({})", formatClipLength(recorder_->seconds()))
                                                : std::wstring(L"Start recording");
    AppendMenuW(menu, MF_STRING, kMenuToggleRecording, recordItem.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuOpenFolder, L"Open videos folder");
    AppendMenuW(menu, MF_STRING, kMenuEditSettings, L"Edit settings file");
    AppendMenuW(menu, MF_STRING, kMenuReloadSettings, L"Reload settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_); // required so the menu closes when clicking elsewhere
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void App::openOutputFolder() {
    const auto dir = settings_.outputRoot();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::editSettingsFile() {
    ShellExecuteW(nullptr, L"open", L"notepad.exe", settingsFile_.c_str(), nullptr, SW_SHOWNORMAL);
}

} // namespace rec
