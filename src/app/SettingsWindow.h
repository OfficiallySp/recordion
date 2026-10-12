#pragma once

#include "core/Settings.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <winrt/base.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct ImFont;

namespace rec {

// What the settings window needs from the app.
class SettingsHost {
public:
    virtual ~SettingsHost() = default;
    virtual const Settings& currentSettings() const = 0;
    // Saves and applies. Returns a message for the user if something couldn't be applied.
    virtual std::string applySettings(const Settings& settings) = 0;
    // While the user presses keys to bind a hotkey, the global hotkeys must not fire.
    virtual void suspendHotkeys(bool suspend) = 0;
    // A few lines describing what is being captured right now, for the Status page.
    virtual std::vector<std::string> statusLines() const = 0;
    virtual std::vector<std::string> availableEncoders() const = 0;
};

// The settings window: Dear ImGui on its own small D3D11 swapchain. It exists only while open
// (no device, no swapchain, no rendering otherwise) and is excluded from screen capture.
class SettingsWindow {
public:
    explicit SettingsWindow(SettingsHost& host) : host_(host) {}
    ~SettingsWindow();

    void show(HINSTANCE instance, HICON icon) { open(instance, icon, true); }
    // Diagnostics: renders every page into <dir>/settings-<n>.png with the window hidden.
    bool screenshotPages(HINSTANCE instance, const std::filesystem::path& dir);
    bool isOpen() const { return hwnd_ != nullptr; }
    void close();

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void open(HINSTANCE instance, HICON icon, bool visible);
    bool createDevice();
    bool saveBackBuffer(const std::filesystem::path& file);
    void createRenderTarget();
    void applyDpi();
    void render();
    void drawUi();
    void drawGeneral();
    void drawVideo();
    void drawReplay();
    void drawAudio();
    void drawHotkeys();
    void drawGames();
    void drawStatus();
    void drawAbout();
    void loadSystemInfo();
    bool captureHotkey(UINT msg, WPARAM wp);
    void loadMicrophones();

    SettingsHost& host_;
    HWND hwnd_ = nullptr;
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<IDXGISwapChain1> swapChain_;
    winrt::com_ptr<ID3D11RenderTargetView> rtv_;
    float dpiScale_ = 1.0f;
    bool dpiChanged_ = false;
    ImFont* font_ = nullptr;

    Settings draft_;
    int page_ = 0;
    std::string message_; // shown in the footer after saving
    int capturingHotkey_ = -1;
    std::string hotkeyError_;
    std::vector<std::pair<std::string, std::string>> microphones_; // id, name
    std::vector<std::string> gpus_;
    std::string windowsBuild_;
};

} // namespace rec
