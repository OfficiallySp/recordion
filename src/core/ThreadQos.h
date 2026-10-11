#pragma once

#include <windows.h>

namespace rec::qos {

void setThreadName(const wchar_t* name);

// Lets the OS run this thread on efficiency cores / at low clocks. For UI and detection threads.
void setEcoQos();
// Opts this thread out of power throttling. For capture, encode and audio threads.
void setHighQos();

// Registers the calling thread with MMCSS for its lifetime ("Capture", "Pro Audio", ...).
class MmcssScope {
public:
    explicit MmcssScope(const wchar_t* task);
    ~MmcssScope();
    MmcssScope(const MmcssScope&) = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;

private:
    HANDLE handle_ = nullptr;
};

// Background I/O + memory priority for the calling thread for its lifetime (file writing).
class BackgroundModeScope {
public:
    BackgroundModeScope();
    ~BackgroundModeScope();
    BackgroundModeScope(const BackgroundModeScope&) = delete;
    BackgroundModeScope& operator=(const BackgroundModeScope&) = delete;

private:
    bool active_ = false;
};

} // namespace rec::qos
