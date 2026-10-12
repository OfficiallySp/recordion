#include "app/App.h"

#include "app/resource.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/ThreadQos.h"
#include "core/Utf.h"
#include "encode/EncoderFactory.h"
#include "core/Clock.h"
#include "output/ClipSaver.h"
#include "output/Recorder.h"
#include "pipeline/CaptureSession.h"

#include <shellapi.h>

#include <format>

namespace rec {
namespace {

enum MenuId : UINT {
    kMenuSettings = 99,
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
    bigIcon_ = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    tray_.create(hwnd_, kMsgTray, icon_, L"Recordion");
    applyStartWithWindows();
    notifier_.create(instance);
    saver_ = std::make_unique<ClipSaver>(hwnd_, kMsgSaved);
    recorder_ = std::make_unique<Recorder>(hwnd_, kMsgSaved);
    detector_ = std::make_unique<GameDetector>(hwnd_, kMsgGameStarted, kMsgGameEnded);
    detector_->start(settings_.detection);
    applyHotkeys();
    log::info("Recordion started; output folder {}", narrow(settings_.outputRoot().wstring()));
    if (settings_.replay.desktopAlwaysOn) startReplay();
    updateTray();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    detector_.reset();
    if (recording()) recorder_->stop(clock::now100ns());
    recorder_.reset(); // finishes the recording's file
    stopReplay();
    saver_.reset(); // finishes the save in progress
    hotkeys_.clear();
    settingsWindow_.close();
    tray_.destroy();
    if (icon_) DestroyIcon(icon_);
    if (bigIcon_) DestroyIcon(bigIcon_);
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
        // NOTIFYICON_VERSION_4: LOWORD(lp) is the event. Left click opens settings, right click the menu.
        if (LOWORD(lp) == WM_CONTEXTMENU) showTrayMenu();
        else if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT) settingsWindow_.show(instance_, bigIcon_);
        return 0;
    case kMsgActivate: // launched again while running: show the settings
        settingsWindow_.show(instance_, bigIcon_);
        return 0;
    case kMsgSaved:
        onSaved(lp);
        return 0;
    case kMsgGameStarted:
        onGameStarted(lp);
        return 0;
    case kMsgGameEnded:
        onGameEnded(static_cast<DWORD>(wp));
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
        case kMenuSettings: settingsWindow_.show(instance_, bigIcon_); break;
        case kMenuOpenFolder: openOutputFolder(); break;
        case kMenuEditSettings: editSettingsFile(); break;
        case kMenuReloadSettings: {
            const bool wasOn = replayOn();
            stopReplay();
            loadSettings();
            applyHotkeys();
            detector_->setSettings(settings_.detection);
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
    if (game_) {
        // The game's display by default (survives the game recreating its window); the window
        // itself if the user asked for that.
        if (settings_.capture.gameWindowOnly) cfg.target.window = game_->window;
        else cfg.target.monitor = game_->monitor;
        cfg.name = game_->name;
        cfg.processId = game_->pid;
    } else {
        cfg.target.monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        cfg.name = "Desktop";
    }
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

void App::onGameStarted(LPARAM lp) {
    std::unique_ptr<DetectedGame> g(reinterpret_cast<DetectedGame*>(lp));
    game_ = *g;
    if (!settings_.replay.enabledInGames) {
        updateTray();
        return;
    }
    if (recording()) {
        // Don't cut a recording in progress; the game gets its own session when it stops.
        toast(widen(game_->name), L"Detected. Your recording continues.", Notifier::Kind::Info);
        updateTray();
        return;
    }
    stopReplay(); // the desktop session, if one was running
    startReplay();
    if (replayOn()) {
        toast(widen(game_->name),
              std::format(L"Instant replay on · {} saves the last {}", widen(settings_.hotkeys.saveReplay),
                          formatClipLength(settings_.replay.seconds)),
              Notifier::Kind::Info);
    }
}

void App::onGameEnded(DWORD pid) {
    if (!game_ || game_->pid != pid) return;
    game_.reset();
    if (session_ && session_->config().processId == pid) {
        if (recording()) recorder_->stop(clock::now100ns()); // the file keeps everything up to now
        stopReplay();
        if (settings_.replay.desktopAlwaysOn) startReplay();
    }
    updateTray();
}

void App::toast(std::wstring_view title, std::wstring_view detail, Notifier::Kind kind) {
    notifier_.show(title, detail, kind, settings_.general.notificationSound);
}

void App::updateTray() {
    std::wstring tip = L"Recordion";
    if (game_) tip += L" – " + widen(game_->name);
    if (recording()) tip += L" – recording";
    else if (replayOn() && !sessionOnlyForRecording_) tip += L" – instant replay on";
    tray_.setTooltip(tip);
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
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings...");
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

void App::applyStartWithWindows() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    if (settings_.general.startWithWindows) {
        const std::wstring command = L"\"" + paths::executablePath().wstring() + L"\"";
        RegSetValueExW(key, L"Recordion", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"Recordion");
    }
    RegCloseKey(key);
}

std::string App::applySettings(const Settings& s) {
    const auto sessionPart = [](const Settings& x) {
        // What a running capture session was built from.
        Settings only;
        only.video = x.video;
        only.audio = x.audio;
        only.capture = x.capture;
        only.replay.seconds = x.replay.seconds;
        only.replay.storage = x.replay.storage;
        return only.toJson();
    };
    const bool sessionChanged = sessionPart(s) != sessionPart(settings_);
    settings_ = s;
    std::string message = settings_.save(settingsFile_) ? "Saved." : "Couldn't write the settings file.";
    applyHotkeys();
    detector_->setSettings(settings_.detection);
    applyStartWithWindows();

    if (sessionChanged && session_) {
        if (recording()) {
            message += " Video and audio changes apply after the current recording.";
        } else {
            const bool onlyForRecording = sessionOnlyForRecording_;
            stopReplay();
            if (!onlyForRecording) startReplay();
            message += " Capture restarted with the new settings.";
        }
    } else if (!session_ && !game_ && settings_.replay.desktopAlwaysOn) {
        startReplay();
    } else if (session_ && !game_ && !settings_.replay.desktopAlwaysOn && !recording() && !sessionOnlyForRecording_) {
        // Desktop replay was switched off (it may also have been turned on by hand; keep it simple).
    }
    updateTray();
    return message;
}

void App::suspendHotkeys(bool suspend) {
    if (suspend) hotkeys_.clear();
    else applyHotkeys();
}

std::vector<std::string> App::statusLines() const {
    std::vector<std::string> lines;
    lines.push_back(game_ ? std::format("Game: {}", game_->name) : std::string("No game detected"));
    if (!session_ || !session_->running()) {
        lines.push_back("Not capturing.");
        return lines;
    }
    const auto st = session_->stats();
    const auto& v = session_->config().settings.video;
    const char* codec = v.codec == Codec::H264 ? "H.264" : v.codec == Codec::HEVC ? "HEVC" : "AV1";
    lines.push_back(std::format("Capturing \"{}\" at {}x{}, {} fps, {}{}", session_->config().name, st.outputSize.width,
                                st.outputSize.height, v.fps, codec, v.tenBit ? " 10-bit" : ""));
    lines.push_back(std::format("Path: {}", st.zeroCopy ? "zero-copy (capture goes straight to the encoder)"
                                                       : "video processor (scaling or colour conversion)"));
    lines.push_back(std::format("Frames: {} captured, {} encoded, {} repeated, {} dropped", st.framesCaptured,
                                st.framesEncoded, st.duplicates, st.droppedBusy + st.encodeErrors));
    const double held = st.buffer.oldestVideoKeyframePts ? (st.buffer.newestPts[0] - st.buffer.oldestVideoKeyframePts) / 1e7 : 0;
    lines.push_back(std::format("Replay buffer: {:.0f} s held, {:.0f} of {:.0f} MB", held, st.buffer.usedBytes / 1048576.0,
                                st.buffer.capacityBytes / 1048576.0));
    lines.push_back(std::format("Audio tracks: {}", st.audioTracks));
    if (recording()) lines.push_back(std::format("Recording: {:.0f} s", recorder_->seconds()));
    return lines;
}

std::vector<std::string> App::availableEncoders() const { return availableEncoderNames(); }

void App::editSettingsFile() {
    ShellExecuteW(nullptr, L"open", L"notepad.exe", settingsFile_.c_str(), nullptr, SW_SHOWNORMAL);
}

} // namespace rec
