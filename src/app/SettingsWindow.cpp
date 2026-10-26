#include "app/SettingsWindow.h"

#include "Version.h"

#include "buffer/ReplayBuffer.h"
#include "core/Hotkey.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"
#include "output/FileNaming.h"

#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <format>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

namespace rec {
namespace {

constexpr wchar_t kClassName[] = L"Recordion.Settings";
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT kFrameMs = 33; // ~30 fps while open; nothing at all when closed
constexpr float kWidth = 760, kHeight = 560;

// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h), defined here to avoid INITGUID games.
constexpr PROPERTYKEY kDeviceFriendlyName{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

const char* kPages[] = {"General", "Video", "Instant replay", "Audio", "Hotkeys", "Games", "Status", "About"};

struct Component {
    const char* name;
    const char* license;
    const char* use;
};
constexpr Component kComponents[] = {
    {"Dear ImGui", "MIT", "this settings window"},
    {"AMD AMF SDK headers", "MIT", "AMD hardware encoding and display capture"},
    {"NVIDIA Video Codec SDK headers (nv-codec-headers)", "MIT", "NVIDIA hardware encoding"},
    {"Intel oneVPL dispatcher (libvpl)", "MIT", "Intel Quick Sync encoding"},
    {"Windows Implementation Libraries (WIL)", "MIT", "Windows resource helpers"},
    {"nlohmann/json", "MIT", "settings and game manifests"},
};

void openShell(const std::wstring& target) {
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

struct HotkeyRow {
    const char* label;
    std::string HotkeySettings::*field;
};
constexpr std::array kHotkeyRows{
    HotkeyRow{"Save replay", &HotkeySettings::saveReplay},
    HotkeyRow{"Start / stop recording", &HotkeySettings::toggleRecording},
    HotkeyRow{"Instant replay on / off", &HotkeySettings::toggleReplayBuffer},
    HotkeyRow{"Start / stop desktop recording", &HotkeySettings::toggleDesktopRecording},
};

template <class E>
bool comboEnum(const char* label, E& value, std::initializer_list<std::pair<E, const char*>> items) {
    const char* current = "?";
    for (const auto& [v, name] : items)
        if (v == value) current = name;
    bool changed = false;
    if (ImGui::BeginCombo(label, current)) {
        for (const auto& [v, name] : items) {
            if (ImGui::Selectable(name, v == value)) {
                value = v;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool comboValue(const char* label, uint32_t& value, std::initializer_list<std::pair<uint32_t, const char*>> items) {
    return comboEnum<uint32_t>(label, value, items);
}

void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

ImFont* g_headingFont = nullptr; // Segoe UI Semibold, when installed

// Section header: a semibold label with a hairline rule under it.
void heading(const char* text) {
    ImGui::Dummy({0, ImGui::GetStyle().ItemSpacing.y});
    ImGui::PushFont(g_headingFont, ImGui::GetStyle().FontSizeBase * 1.1f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Dummy({0, ImGui::GetStyle().ItemSpacing.y * 0.25f});
}

std::string formatLength(uint32_t seconds) {
    if (seconds < 60) return std::format("{} s", seconds);
    if (seconds % 60 == 0) return std::format("{} min", seconds / 60);
    return std::format("{}:{:02}", seconds / 60, seconds % 60);
}

std::optional<std::wstring> pickPath(HWND owner, bool folder) {
    winrt::com_ptr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.put()))))
        return std::nullopt;
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    if (!folder) {
        const COMDLG_FILTERSPEC filter[] = {{L"Programs", L"*.exe"}};
        dlg->SetFileTypes(1, filter);
    }
    if (FAILED(dlg->Show(owner))) return std::nullopt;
    winrt::com_ptr<IShellItem> item;
    wchar_t* path = nullptr;
    if (FAILED(dlg->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    std::wstring result = path;
    CoTaskMemFree(path);
    return result;
}

void applyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 0;
    s.FrameRounding = 5;
    s.GrabRounding = 5;
    s.PopupRounding = 6;
    s.ChildRounding = 6;
    s.FramePadding = {9, 6};
    s.ItemSpacing = {10, 8};
    s.WindowPadding = {16, 14};
    s.ScrollbarSize = 12;
    ImVec4* c = s.Colors;
    const ImVec4 accent{0.90f, 0.16f, 0.20f, 1.0f}; // the icon's red
    c[ImGuiCol_WindowBg] = {0.086f, 0.086f, 0.098f, 1};
    c[ImGuiCol_ChildBg] = {0.11f, 0.11f, 0.125f, 1};
    c[ImGuiCol_PopupBg] = {0.12f, 0.12f, 0.135f, 1};
    c[ImGuiCol_FrameBg] = {0.16f, 0.16f, 0.18f, 1};
    c[ImGuiCol_FrameBgHovered] = {0.21f, 0.21f, 0.235f, 1};
    c[ImGuiCol_FrameBgActive] = {0.24f, 0.24f, 0.27f, 1};
    c[ImGuiCol_Button] = {0.19f, 0.19f, 0.215f, 1};
    c[ImGuiCol_ButtonHovered] = {0.25f, 0.25f, 0.28f, 1};
    c[ImGuiCol_ButtonActive] = {0.30f, 0.30f, 0.33f, 1};
    c[ImGuiCol_Header] = {0.20f, 0.20f, 0.225f, 1};
    c[ImGuiCol_HeaderHovered] = {0.24f, 0.24f, 0.27f, 1};
    c[ImGuiCol_HeaderActive] = {0.28f, 0.28f, 0.31f, 1};
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = {1.0f, 0.30f, 0.33f, 1};
    c[ImGuiCol_Separator] = {0.21f, 0.21f, 0.24f, 1};
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_TextSelectedBg] = {accent.x, accent.y, accent.z, 0.35f};
    s.ScaleAllSizes(scale);
}

} // namespace

SettingsWindow::~SettingsWindow() { close(); }

void SettingsWindow::open(HINSTANCE instance, HICON icon, bool visible) {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        return;
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &SettingsWindow::wndProc;
    wc.hInstance = instance;
    wc.hIcon = icon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    // Centre on the display under the cursor, sized for its DPI.
    POINT cursor;
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const float scale = dpiX / 96.0f;
    RECT r{0, 0, static_cast<LONG>(kWidth * scale), static_cast<LONG>(kHeight * scale)};
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX;
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpiX);
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;

    hwnd_ = CreateWindowExW(0, kClassName, L"Recordion Settings", style, x, y, w, h, nullptr, nullptr, instance, this);
    if (!hwnd_) return;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE); // never in a recording

    if (!createDevice()) {
        log::error("settings: could not create a D3D11 device for the window");
        DestroyWindow(hwnd_);
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing to persist; the window always opens the same way
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    font_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f);
    std::error_code ec;
    g_headingFont = std::filesystem::exists(L"C:\\Windows\\Fonts\\seguisb.ttf", ec)
                        ? io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 16.0f)
                        : nullptr;
    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_.get(), context_.get());
    applyDpi();

    draft_ = host_.currentSettings();
    message_.clear();
    loadMicrophones();
    loadSystemInfo();
    if (!visible) return;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    SetTimer(hwnd_, kFrameTimer, kFrameMs, nullptr);
}

bool SettingsWindow::screenshotPages(HINSTANCE instance, const std::filesystem::path& dir) {
    open(instance, nullptr, false);
    if (!hwnd_ || !ImGui::GetCurrentContext()) return false;
    bool ok = true;
    for (int page = 0; page < static_cast<int>(std::size(kPages)) && ok; ++page) {
        page_ = page;
        for (int frame = 0; frame < 3; ++frame) render(); // layout settles after a couple of frames
        ok = saveBackBuffer(dir / std::format(L"settings-{}.png", page));
    }
    close();
    return ok;
}

bool SettingsWindow::saveBackBuffer(const std::filesystem::path& file) {
    winrt::com_ptr<ID3D11Texture2D> back;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(back.put()));
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&d, nullptr, staging.put()))) return false;
    context_->CopyResource(staging.get(), back.get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) return false;

    winrt::com_ptr<IWICImagingFactory> wic;
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))) &&
              SUCCEEDED(wic->CreateStream(stream.put())) &&
              SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())) &&
              SUCCEEDED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(frame.put(), nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(d.Width, d.Height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    ok = ok && SUCCEEDED(frame->SetPixelFormat(&format)) &&
         SUCCEEDED(frame->WritePixels(d.Height, m.RowPitch, m.RowPitch * d.Height, static_cast<BYTE*>(m.pData))) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    context_->Unmap(staging.get(), 0);
    return ok;
}

void SettingsWindow::close() {
    if (hwnd_) DestroyWindow(hwnd_); // cleanup happens in WM_DESTROY
}

bool SettingsWindow::createDevice() {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
                                 D3D11_SDK_VERSION, device_.put(), nullptr, context_.put())))
        return false;
    winrt::com_ptr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))) ||
        FAILED(factory->CreateSwapChainForHwnd(device_.get(), hwnd_, &sd, nullptr, nullptr, swapChain_.put())))
        return false;
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    createRenderTarget();
    return true;
}

void SettingsWindow::createRenderTarget() {
    winrt::com_ptr<ID3D11Texture2D> back;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(back.put()));
    device_->CreateRenderTargetView(back.get(), nullptr, rtv_.put());
}

void SettingsWindow::applyDpi() {
    dpiScale_ = GetDpiForWindow(hwnd_) / 96.0f;
    applyTheme(dpiScale_);
    ImGui::GetStyle().FontSizeBase = 16.0f;
    ImGui::GetStyle().FontScaleDpi = dpiScale_;
    dpiChanged_ = false;
}

LRESULT CALLBACK SettingsWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handleMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SettingsWindow::handleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (capturingHotkey_ >= 0 && captureHotkey(msg, wp)) return 0;
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd_, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_TIMER:
        if (wp == kFrameTimer) render();
        return 0;
    case WM_SIZE:
        if (swapChain_ && wp != SIZE_MINIMIZED) {
            rtv_ = nullptr;
            swapChain_->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            createRenderTarget();
            render();
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        dpiChanged_ = true;
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {static_cast<LONG>(600 * dpiScale_), static_cast<LONG>(420 * dpiScale_)};
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd_, kFrameTimer);
        if (capturingHotkey_ >= 0) host_.suspendHotkeys(false);
        capturingHotkey_ = -1;
        if (ImGui::GetCurrentContext()) {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        rtv_ = nullptr;
        swapChain_ = nullptr;
        context_ = nullptr;
        device_ = nullptr;
        return 0;
    case WM_NCDESTROY: {
        const HWND hwnd = std::exchange(hwnd_, nullptr);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// While binding a hotkey, keystrokes go to the binder, not to ImGui.
bool SettingsWindow::captureHotkey(UINT msg, WPARAM wp) {
    if (msg == WM_CHAR || msg == WM_SYSCHAR || msg == WM_KEYUP || msg == WM_SYSKEYUP) return true;
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return false;
    const auto vk = static_cast<UINT>(wp);
    switch (vk) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LWIN: case VK_RWIN:
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
        return true; // wait for the actual key
    default:
        break;
    }
    Hotkey hk;
    hk.vk = vk;
    if (GetKeyState(VK_CONTROL) & 0x8000) hk.modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) hk.modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) hk.modifiers |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) hk.modifiers |= MOD_WIN;

    if (vk == VK_ESCAPE && hk.modifiers == 0) { // cancel
        capturingHotkey_ = -1;
        host_.suspendHotkeys(false);
        return true;
    }
    const std::string text = formatHotkey(hk);
    if (text.empty()) {
        hotkeyError_ = "That key can't be used for a hotkey";
        return true; // keep listening
    }
    if (const auto parsed = parseHotkey(text); !parsed) {
        hotkeyError_ = parsed.error();
        return true;
    }
    draft_.hotkeys.*kHotkeyRows[static_cast<size_t>(capturingHotkey_)].field = text;
    capturingHotkey_ = -1;
    hotkeyError_.clear();
    host_.suspendHotkeys(false);
    return true;
}

void SettingsWindow::render() {
    if (!ImGui::GetCurrentContext() || !rtv_) return;
    if (dpiChanged_) applyDpi();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    drawUi();
    ImGui::Render();
    const float clear[4] = {0.086f, 0.086f, 0.098f, 1};
    ID3D11RenderTargetView* rtv = rtv_.get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(1, 0);
}

void SettingsWindow::drawUi() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
    const float sidebar = 170 * dpiScale_;

    ImGui::BeginChild("##nav", {sidebar, -footer}, ImGuiChildFlags_None);
    ImGui::Spacing();
    for (int i = 0; i < static_cast<int>(std::size(kPages)); ++i)
        if (ImGui::Selectable(kPages[i], page_ == i, 0, {0, ImGui::GetFrameHeight()})) page_ = i;
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##page", {0, -footer}, ImGuiChildFlags_None);
    ImGui::PushItemWidth(-220 * dpiScale_);
    switch (page_) {
    case 0: drawGeneral(); break;
    case 1: drawVideo(); break;
    case 2: drawReplay(); break;
    case 3: drawAudio(); break;
    case 4: drawHotkeys(); break;
    case 5: drawGames(); break;
    case 6: drawStatus(); break;
    case 7: drawAbout(); break;
    }
    ImGui::PopItemWidth();
    ImGui::EndChild();

    // Footer: Save applies everything at once.
    ImGui::Separator();
    const bool dirty = draft_.toJson() != host_.currentSettings().toJson();
    if (!dirty) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, {0.78f, 0.13f, 0.17f, 1});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.88f, 0.20f, 0.24f, 1});
    if (ImGui::Button("Save", {110 * dpiScale_, 0})) message_ = host_.applySettings(draft_);
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button("Revert", {110 * dpiScale_, 0})) draft_ = host_.currentSettings();
    if (!dirty) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (dirty) ImGui::TextDisabled("Unsaved changes");
    else if (!message_.empty()) ImGui::TextDisabled("%s", message_.c_str());
    ImGui::End();
}

void SettingsWindow::drawGeneral() {
    heading("Output");
    std::string folder = draft_.output.folder.empty() ? narrow(draft_.outputRoot().wstring()) : draft_.output.folder;
    ImGui::SetNextItemWidth(-110 * dpiScale_);
    if (ImGui::InputText("##folder", &folder)) draft_.output.folder = folder;
    ImGui::SameLine();
    if (ImGui::Button("Browse...", {-1, 0}))
        if (auto picked = pickPath(hwnd_, true)) draft_.output.folder = narrow(*picked);
    hint("Each game gets its own folder inside this one.");

    ImGui::InputText("File name", &draft_.output.fileTemplate);
    NamingContext example;
    example.game = "Cyberpunk 2077";
    GetLocalTime(&example.localTime);
    example.durationSeconds = 90;
    const std::string preview = sanitizePathComponent(expandFileTemplate(draft_.output.fileTemplate, example));
    hint(std::format("Tokens: {{game}} {{date}} {{time}} {{type}} {{duration}}.  Example: {}.mp4", preview).c_str());

    heading("Startup and notifications");
    ImGui::Checkbox("Start with Windows", &draft_.general.startWithWindows);
    ImGui::Checkbox("Play a sound with notifications", &draft_.general.notificationSound);
}

void SettingsWindow::drawVideo() {
    auto& v = draft_.video;
    heading("Encoder");
    const auto encoders = host_.availableEncoders();
    std::string encoderText;
    for (const auto& e : encoders) encoderText += (encoderText.empty() ? "" : ", ") + e;
    comboEnum("Hardware encoder", v.vendor,
              {{EncoderVendor::Auto, "Automatic"}, {EncoderVendor::AMD, "AMD (AMF)"}, {EncoderVendor::NVIDIA, "NVIDIA (NVENC)"},
               {EncoderVendor::Intel, "Intel (Quick Sync)"}});
    hint(std::format("Available on this PC: {}.", encoderText.empty() ? "none" : encoderText).c_str());

    comboEnum("Codec", v.codec, {{Codec::H264, "H.264 (most compatible)"}, {Codec::HEVC, "HEVC / H.265"}, {Codec::AV1, "AV1"}});
    if (v.codec == Codec::H264) {
        v.tenBit = false;
    } else {
        ImGui::Checkbox("10-bit colour", &v.tenBit);
        if (v.tenBit)
            hint("Games draw in 8-bit, so 10-bit mainly smooths banding in gradients. It needs a small colour "
                 "conversion on your game's GPU (about 0.1 ms a frame); 8-bit goes straight into the encoder.");
    }
    if (v.codec != Codec::H264)
        hint("HEVC and AV1 give better quality per megabyte. Windows' own player needs the free HEVC/AV1 "
             "extensions from the Microsoft Store; browsers, VLC and editors play them as they are.");

    heading("Quality");
    comboEnum("Rate control", v.rateControl,
              {{RateControl::VBR, "Variable bitrate (recommended)"}, {RateControl::CBR, "Constant bitrate"},
               {RateControl::CQP, "Constant quality"}});
    if (v.rateControl == RateControl::CQP) {
        int cq = static_cast<int>(v.cqLevel);
        if (ImGui::SliderInt("Quality level", &cq, 10, 40, "%d (lower is better)")) v.cqLevel = static_cast<uint32_t>(cq);
    } else {
        int mbps = static_cast<int>(v.bitrateKbps / 1000);
        if (ImGui::SliderInt("Bitrate", &mbps, 5, 200, "%d Mbps")) {
            v.bitrateKbps = static_cast<uint32_t>(mbps) * 1000;
            v.maxBitrateKbps = std::max(v.maxBitrateKbps, v.bitrateKbps);
        }
        if (v.rateControl == RateControl::VBR) {
            int peak = static_cast<int>(v.maxBitrateKbps / 1000);
            if (ImGui::SliderInt("Peak bitrate", &peak, mbps, 300, "%d Mbps")) v.maxBitrateKbps = static_cast<uint32_t>(peak) * 1000;
        }
    }
    comboEnum("Encoder speed", v.preset,
              {{QualityPreset::Speed, "Speed"}, {QualityPreset::Balanced, "Balanced"}, {QualityPreset::Quality, "Quality"},
               {QualityPreset::MaxQuality, "Max quality (uses some GPU shader time)"}});
    if (v.preset == QualityPreset::MaxQuality)
        hint("Max quality turns on pre-analysis, which runs on the same shader cores as your game and can cost a "
             "few fps. Every other setting keeps encoding on the dedicated encoder chip.");

    heading("Frame");
    comboValue("Frame rate", v.fps, {{30, "30 fps"}, {60, "60 fps"}, {90, "90 fps"}, {120, "120 fps"}, {144, "144 fps"},
                                     {165, "165 fps"}, {240, "240 fps"}});
    comboValue("Resolution", v.outputHeight,
               {{0, "Native (no scaling, cheapest)"}, {2160, "2160p"}, {1440, "1440p"}, {1080, "1080p"}, {720, "720p"}});
    comboValue("Keyframe every", v.gopSeconds, {{1, "1 second"}, {2, "2 seconds"}, {4, "4 seconds"}});
    hint("Native resolution lets the capture go straight into the encoder without a scaling pass.");
    ImGui::Checkbox("Constant frame rate", &v.constantFrameRate);
    hint(v.constantFrameRate
             ? "Repeats the last frame whenever nothing changed. A repeat costs the encoder as much as a new "
               "frame; only needed for editors that mishandle variable frame rate."
             : "When nothing changes on screen (menus, loading, a game running below the frame rate), the frame "
               "isn't encoded again. Players and most editors handle this; turn it on for editors that don't.");

    heading("Capture");
    auto& c = draft_.capture;
    if (c.method != CaptureMethod::AmdDirect) c.method = CaptureMethod::Auto;
    comboEnum("Capture method", c.method,
              {{CaptureMethod::Auto, "Windows Graphics Capture"}, {CaptureMethod::AmdDirect, "AMD Direct Capture (experimental)"}});
    if (c.method == CaptureMethod::AmdDirect)
        hint("Takes frames straight from the AMD display driver instead of through Windows' compositor. In a "
             "GPU-heavy game on a Radeon 8060S it cost the game more than Windows Graphics Capture (about 2% of the "
             "3D engine vs under 1%), and it hands over every frame whether or not anything changed, so the encoder "
             "can't skip unchanged ones. Whole displays only, no mouse cursor. Falls back to Windows Graphics "
             "Capture on other GPUs.");
    ImGui::Checkbox("Include the mouse cursor", &c.captureCursor);
}

void SettingsWindow::drawReplay() {
    auto& r = draft_.replay;
    heading("Instant replay");
    ImGui::Checkbox("Turn on automatically when a game starts", &r.enabledInGames);
    ImGui::Checkbox("Keep a replay of the desktop when no game is running", &r.desktopAlwaysOn);

    static constexpr uint32_t kLengths[] = {15, 30, 60, 90, 120, 180, 300, 600, 900, 1200};
    int index = 0;
    for (int i = 0; i < static_cast<int>(std::size(kLengths)); ++i)
        if (kLengths[i] <= r.seconds) index = i;
    if (ImGui::SliderInt("Replay length", &index, 0, static_cast<int>(std::size(kLengths)) - 1,
                         formatLength(kLengths[index]).c_str()))
        r.seconds = kLengths[index];

    comboEnum("Keep the replay in", r.storage,
              {{BufferStorage::RAM, "Memory (fastest)"}, {BufferStorage::Disk, "A temporary file (for long replays)"}});
    const auto& v = draft_.video;
    const size_t most = replayBufferBytes(r.seconds, v.gopSeconds, std::max(v.bitrateKbps, v.maxBitrateKbps),
                                          draft_.audio.bitrateKbps * 3);
    const double typical = (r.seconds + v.gopSeconds + 1.0) * (v.bitrateKbps + draft_.audio.bitrateKbps) * 1000 / 8;
    if (r.storage == BufferStorage::RAM)
        hint(std::format("Uses up to about {:.0f} MB of memory at this bitrate (never more than {:.0f} MB), and less when the "
                         "picture is simple. Only encoded video is kept, never raw frames.",
                         typical / 1048576.0, most / 1048576.0)
                 .c_str());
    else
        hint(std::format("Uses a temporary file of up to {:.0f} MB; Windows keeps it in memory while memory is free.",
                         most / 1048576.0)
                 .c_str());
}

void SettingsWindow::drawAudio() {
    auto& a = draft_.audio;
    heading("Sources");
    ImGui::Checkbox("Record game audio", &a.captureSystem);
    if (a.captureSystem) {
        ImGui::Indent();
        ImGui::Checkbox("Only the game (not Discord, music or notifications)", &a.gameAudioOnly);
        ImGui::Unindent();
    }
    ImGui::Checkbox("Record microphone", &a.captureMic);
    if (a.captureMic) {
        const char* current = "Default microphone";
        for (const auto& [id, name] : microphones_)
            if (id == a.micDeviceId) current = name.c_str();
        if (ImGui::BeginCombo("Microphone", current)) {
            if (ImGui::Selectable("Default microphone", a.micDeviceId.empty())) a.micDeviceId.clear();
            for (const auto& [id, name] : microphones_)
                if (ImGui::Selectable(name.c_str(), id == a.micDeviceId)) a.micDeviceId = id;
            ImGui::EndCombo();
        }
    }
    heading("Tracks");
    ImGui::Checkbox("Also save the game and the microphone as separate tracks", &a.separateTracks);
    hint("Track 1 always has everything mixed, so every player plays it. Separate tracks let you adjust them later "
         "in an editor.");
    comboValue("Audio quality", a.bitrateKbps, {{96, "96 kbps"}, {128, "128 kbps"}, {160, "160 kbps"}, {192, "192 kbps"}});
}

void SettingsWindow::drawHotkeys() {
    heading("Hotkeys");
    hint("Click a hotkey, then press the keys you want. Esc cancels.");
    ImGui::Spacing();
    for (size_t i = 0; i < kHotkeyRows.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        std::string& value = draft_.hotkeys.*kHotkeyRows[i].field;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(kHotkeyRows[i].label);
        ImGui::SameLine(260 * dpiScale_);
        const bool capturing = capturingHotkey_ == static_cast<int>(i);
        const std::string label = capturing ? "Press keys..." : value.empty() ? "Not set" : value;
        if (capturing) ImGui::PushStyleColor(ImGuiCol_Button, {0.78f, 0.13f, 0.17f, 1});
        if (ImGui::Button(label.c_str(), {200 * dpiScale_, 0}) && !capturing) {
            capturingHotkey_ = static_cast<int>(i);
            hotkeyError_.clear();
            host_.suspendHotkeys(true); // so pressing the current combo doesn't trigger it
        }
        if (capturing) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Clear")) value.clear();
        // The same combo on two actions would only register once.
        for (size_t j = 0; j < kHotkeyRows.size(); ++j) {
            if (j != i && !value.empty() && value == draft_.hotkeys.*kHotkeyRows[j].field) {
                ImGui::SameLine();
                ImGui::TextColored({1, 0.45f, 0.4f, 1}, "same as \"%s\"", kHotkeyRows[j].label);
                break;
            }
        }
        ImGui::PopID();
    }
    if (!hotkeyError_.empty()) ImGui::TextColored({1, 0.45f, 0.4f, 1}, "%s", hotkeyError_.c_str());
    ImGui::Spacing();
    hint("If another app (GeForce Experience, AMD Adrenalin, Game Bar) already uses a combo, Windows won't let "
         "Recordion have it; you'll get a notification after saving.");
}

void SettingsWindow::drawGames() {
    auto& d = draft_.detection;
    heading("Game detection");
    ImGui::Checkbox("Detect games automatically", &d.enabled);
    hint("Games from Steam, Epic, GOG, Ubisoft and Xbox, and anything Windows lists as a game, are recognised "
         "when they run fullscreen or use the GPU. Nothing is ever injected into a game.");

    auto list = [&](const char* title, const char* id, std::vector<std::string>& items) {
        heading(title);
        ImGui::PushID(id);
        for (size_t i = 0; i < items.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Remove")) {
                items.erase(items.begin() + static_cast<ptrdiff_t>(i));
                ImGui::PopID();
                break;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(items[i].c_str());
            ImGui::PopID();
        }
        if (ImGui::Button("Add a program..."))
            if (auto picked = pickPath(hwnd_, false)) items.push_back(narrow(*picked));
        ImGui::PopID();
    };
    list("Always treat as a game", "always", d.alwaysRecord);
    list("Never treat as a game", "never", d.neverRecord);
}

void SettingsWindow::drawStatus() {
    heading("Right now");
    for (const auto& line : host_.statusLines()) ImGui::TextUnformatted(line.c_str());
    heading("Files");
    hint(std::format("Settings: {}", narrow((paths::configDir() / L"settings.json").wstring())).c_str());
    hint(std::format("Logs: {}", narrow(paths::logDir().wstring())).c_str());
}

void SettingsWindow::drawAbout() {
    ImGui::Spacing();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.9f);
    ImGui::TextUnformatted("Recordion");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("version %s", RECORDION_VERSION_STRING);
    ImGui::TextWrapped("A lightweight game and desktop recorder. Instant replay, recording and automatic game detection, "
                       "encoded on your GPU's dedicated video hardware so your games keep their performance.");

    heading("This PC");
    for (const auto& gpu : gpus_) ImGui::BulletText("%s", gpu.c_str());
    const auto encoders = host_.availableEncoders();
    std::string encoderText;
    for (const auto& e : encoders) encoderText += (encoderText.empty() ? "" : ", ") + e;
    ImGui::BulletText("Hardware encoders: %s", encoderText.empty() ? "none found" : encoderText.c_str());
    if (!windowsBuild_.empty()) ImGui::BulletText("Windows build %s", windowsBuild_.c_str());

    heading("Build");
    ImGui::BulletText("64-bit, built %s with MSVC %d.%02d.%d", __DATE__, _MSC_FULL_VER / 10000000,
                      (_MSC_FULL_VER / 100000) % 100, _MSC_FULL_VER % 100000);

    heading("Folders and links");
    if (ImGui::Button("Videos")) {
        const auto root = draft_.outputRoot();
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        openShell(root.wstring());
    }
    ImGui::SameLine();
    if (ImGui::Button("Settings")) openShell(paths::configDir().wstring());
    ImGui::SameLine();
    if (ImGui::Button("Logs")) openShell(paths::logDir().wstring());
    ImGui::SameLine();
    if (ImGui::Button("Project page")) openShell(widen(RECORDION_REPOSITORY_URL));

    heading("Open-source components");
    for (const auto& c : kComponents) ImGui::BulletText("%s (%s): %s", c.name, c.license, c.use);
    hint("Full license texts are in THIRD_PARTY_NOTICES.txt next to recordion.exe.");
    ImGui::Spacing();
    ImGui::TextDisabled("Copyright (C) 2026 Shane");
}

void SettingsWindow::loadSystemInfo() {
    gpus_.clear();
    winrt::com_ptr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                gpus_.push_back(std::format("{} ({:.0f} GB)", narrow(d.Description),
                                            (d.DedicatedVideoMemory + d.SharedSystemMemory) / 1073741824.0));
            adapter = nullptr;
        }
    }
    wchar_t build[32] = {};
    DWORD size = sizeof(build), ubr = 0, ubrSize = sizeof(ubr);
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"CurrentBuild", RRF_RT_REG_SZ, nullptr, build, &size) == ERROR_SUCCESS) {
        windowsBuild_ = narrow(build);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &ubrSize) == ERROR_SUCCESS)
            windowsBuild_ += std::format(".{}", ubr);
    }
}

void SettingsWindow::loadMicrophones() {
    microphones_.clear();
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    winrt::com_ptr<IMMDeviceCollection> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put()))) ||
        FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, devices.put())))
        return;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        winrt::com_ptr<IMMDevice> device;
        winrt::com_ptr<IPropertyStore> props;
        wchar_t* id = nullptr;
        if (FAILED(devices->Item(i, device.put())) || FAILED(device->GetId(&id))) continue;
        PROPVARIANT name;
        PropVariantInit(&name);
        std::string friendly = "Microphone";
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.put())) &&
            SUCCEEDED(props->GetValue(kDeviceFriendlyName, &name)) && name.vt == VT_LPWSTR)
            friendly = narrow(name.pwszVal);
        PropVariantClear(&name);
        microphones_.emplace_back(narrow(id), friendly);
        CoTaskMemFree(id);
    }
}

} // namespace rec
