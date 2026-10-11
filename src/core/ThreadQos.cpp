#include "core/ThreadQos.h"

#include "core/Log.h"

#include <avrt.h>

namespace rec::qos {
namespace {

void setPowerThrottling(bool eco) {
    THREAD_POWER_THROTTLING_STATE state{};
    state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = eco ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
}

} // namespace

void setThreadName(const wchar_t* name) { SetThreadDescription(GetCurrentThread(), name); }

void setEcoQos() { setPowerThrottling(true); }

void setHighQos() { setPowerThrottling(false); }

MmcssScope::MmcssScope(const wchar_t* task) {
    DWORD taskIndex = 0;
    handle_ = AvSetMmThreadCharacteristicsW(task, &taskIndex);
    if (!handle_) log::warn("MMCSS registration failed (error {})", GetLastError());
}

MmcssScope::~MmcssScope() {
    if (handle_) AvRevertMmThreadCharacteristics(handle_);
}

BackgroundModeScope::BackgroundModeScope() {
    active_ = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
}

BackgroundModeScope::~BackgroundModeScope() {
    if (active_) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

} // namespace rec::qos
