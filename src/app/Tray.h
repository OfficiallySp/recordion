#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace rec {

class Tray {
public:
    ~Tray() { destroy(); }

    bool create(HWND hwnd, UINT callbackMessage, HICON icon, std::wstring_view tooltip);
    bool recreate(); // after Explorer restarts (TaskbarCreated)
    void setTooltip(std::wstring_view tooltip);
    // System notification; used for errors that must be seen even when no game is running.
    void notify(std::wstring_view title, std::wstring_view text, bool warning);
    void destroy();

private:
    bool add();

    HWND hwnd_ = nullptr;
    UINT callbackMessage_ = 0;
    HICON icon_ = nullptr;
    std::wstring tooltip_;
    bool added_ = false;
};

} // namespace rec
