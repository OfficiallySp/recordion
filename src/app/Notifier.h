#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <winrt/base.h>

#include <string>

namespace rec {

// Small "Replay saved" style toast in the top-right corner. Click-through, never takes focus,
// and excluded from screen capture so it never shows up in recordings.
class Notifier {
public:
    enum class Kind { Info, Success, Error };

    ~Notifier();
    bool create(HINSTANCE instance);
    void show(std::wstring_view title, std::wstring_view detail, Kind kind, bool sound);

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void render(HMONITOR monitor);

    HWND hwnd_ = nullptr;
    winrt::com_ptr<ID2D1Factory> d2d_;
    winrt::com_ptr<IDWriteFactory> dwrite_;
    std::wstring title_;
    std::wstring detail_;
    Kind kind_ = Kind::Info;
};

} // namespace rec
