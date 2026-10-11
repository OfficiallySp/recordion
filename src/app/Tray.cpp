#include "app/Tray.h"

#include <shellapi.h>

#include <algorithm>

namespace rec {
namespace {

constexpr UINT kIconId = 1;

template <size_t N>
void copyTruncated(wchar_t (&dst)[N], std::wstring_view src) {
    const size_t n = std::min(src.size(), N - 1);
    std::copy_n(src.data(), n, dst);
    dst[n] = L'\0';
}

NOTIFYICONDATAW baseData(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kIconId;
    return nid;
}

} // namespace

bool Tray::create(HWND hwnd, UINT callbackMessage, HICON icon, std::wstring_view tooltip) {
    hwnd_ = hwnd;
    callbackMessage_ = callbackMessage;
    icon_ = icon;
    tooltip_ = tooltip;
    return add();
}

bool Tray::add() {
    NOTIFYICONDATAW nid = baseData(hwnd_);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = callbackMessage_;
    nid.hIcon = icon_;
    copyTruncated(nid.szTip, tooltip_);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return false;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    added_ = true;
    return true;
}

bool Tray::recreate() {
    added_ = false;
    return add();
}

void Tray::setTooltip(std::wstring_view tooltip) {
    tooltip_ = tooltip;
    if (!added_) return;
    NOTIFYICONDATAW nid = baseData(hwnd_);
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    copyTruncated(nid.szTip, tooltip_);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::notify(std::wstring_view title, std::wstring_view text, bool warning) {
    if (!added_) return;
    NOTIFYICONDATAW nid = baseData(hwnd_);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = (warning ? NIIF_WARNING : NIIF_INFO) | NIIF_RESPECT_QUIET_TIME;
    copyTruncated(nid.szInfoTitle, title);
    copyTruncated(nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::destroy() {
    if (!added_) return;
    NOTIFYICONDATAW nid = baseData(hwnd_);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}

} // namespace rec
