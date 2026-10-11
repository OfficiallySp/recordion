#include "app/Notifier.h"

#include "core/Log.h"

#include <mmsystem.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>

namespace rec {
namespace {

constexpr wchar_t kClassName[] = L"Recordion.Toast";
constexpr UINT_PTR kHideTimer = 1;
constexpr UINT kVisibleMs = 2600;

// Layout in DIPs, scaled by the monitor DPI.
constexpr float kWidth = 320, kHeight = 64, kMargin = 20, kRadius = 10, kPad = 16, kDot = 10;

D2D1_COLOR_F accentFor(Notifier::Kind kind) {
    switch (kind) {
    case Notifier::Kind::Success: return D2D1::ColorF(0x3DDC84);
    case Notifier::Kind::Error: return D2D1::ColorF(0xFF5A5F);
    case Notifier::Kind::Info: break;
    }
    return D2D1::ColorF(0x5AA9FF);
}

} // namespace

Notifier::~Notifier() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool Notifier::create(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &Notifier::wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            kClassName, L"Recordion", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    // Never part of what we (or anyone else) capture.
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.put())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<::IUnknown**>(dwrite_.put())))) {
        log::warn("toast: Direct2D unavailable");
        return false;
    }
    return true;
}

LRESULT CALLBACK Notifier::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == kHideTimer) {
        KillTimer(hwnd, kHideTimer);
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Notifier::show(std::wstring_view title, std::wstring_view detail, Kind kind, bool sound) {
    if (!hwnd_ || !d2d_) return;
    title_ = title;
    detail_ = detail;
    kind_ = kind;
    // The display the user is looking at: where the foreground window (e.g. the game) is.
    HWND fg = GetForegroundWindow();
    render(fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(hwnd_, kHideTimer, kVisibleMs, nullptr);
    if (sound) {
        PlaySoundW(kind == Kind::Error ? L"SystemExclamation" : L"Notification.Default", nullptr,
                   SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
    }
}

void Notifier::render(HMONITOR monitor) {
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const float scale = dpiX / 96.0f;
    const int w = static_cast<int>(std::lround(kWidth * scale));
    const int h = static_cast<int>(std::lround(kHeight * scale));

    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const POINT pos{mi.rcWork.right - w - static_cast<int>(kMargin * scale),
                    mi.rcWork.top + static_cast<int>(kMargin * scale)};

    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);

    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(dpiX), static_cast<float>(dpiY));
    winrt::com_ptr<ID2D1DCRenderTarget> rt;
    winrt::com_ptr<IDWriteTextFormat> titleFormat, detailFormat;
    const RECT bounds{0, 0, w, h};
    if (SUCCEEDED(d2d_->CreateDCRenderTarget(&props, rt.put())) && SUCCEEDED(rt->BindDC(mem, &bounds)) &&
        SUCCEEDED(dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, 14.5f, L"", titleFormat.put())) &&
        SUCCEEDED(dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"", detailFormat.put()))) {
        for (auto* f : {titleFormat.get(), detailFormat.get()}) {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            winrt::com_ptr<IDWriteInlineObject> ellipsis;
            dwrite_->CreateEllipsisTrimmingSign(f, ellipsis.put());
            f->SetTrimming(&trim, ellipsis.get());
        }
        winrt::com_ptr<ID2D1SolidColorBrush> panel, accent, text, subtext;
        rt->CreateSolidColorBrush(D2D1::ColorF(0x18181C, 0.94f), panel.put());
        rt->CreateSolidColorBrush(accentFor(kind_), accent.put());
        rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), text.put());
        rt->CreateSolidColorBrush(D2D1::ColorF(0xB4B4BE), subtext.put());

        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0.0f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, kWidth - 0.5f, kHeight - 0.5f), kRadius, kRadius),
                                 panel.get());
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kPad + kDot / 2, kHeight / 2), kDot / 2, kDot / 2), accent.get());
        const float textLeft = kPad + kDot + 12;
        rt->DrawText(title_.c_str(), static_cast<UINT32>(title_.size()), titleFormat.get(),
                     D2D1::RectF(textLeft, 11, kWidth - kPad, 32), text.get());
        rt->DrawText(detail_.c_str(), static_cast<UINT32>(detail_.size()), detailFormat.get(),
                     D2D1::RectF(textLeft, 33, kWidth - kPad, kHeight - 8), subtext.get());
        rt->EndDraw();
    }

    const SIZE size{w, h};
    const POINT origin{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen, const_cast<POINT*>(&pos), const_cast<SIZE*>(&size), mem,
                        const_cast<POINT*>(&origin), 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

} // namespace rec
