#pragma once

#include "audio/AudioRing.h"
#include "audio/AudioTimeline.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <winrt/base.h>

#include <string>

namespace rec {

// One WASAPI capture stream (48 kHz float stereo) placed on the shared timeline.
class WasapiSource {
public:
    enum class Kind {
        SystemLoopback,  // everything playing on the default output device
        ProcessLoopback, // only this process tree (the game)
        Microphone,
    };

    ~WasapiSource();
    // `pid` for ProcessLoopback; `deviceId` (empty = default) for Microphone.
    bool start(Kind kind, DWORD pid = 0, const std::wstring& deviceId = {});
    void stop();

    // Moves all available packets into `ring`. Returns false if the device was lost.
    bool read(const AudioTimeline& timeline, AudioRing& ring);
    Kind kind() const { return kind_; }

private:
    bool initClient(bool loopback);

    Kind kind_ = Kind::SystemLoopback;
    winrt::com_ptr<IAudioClient> client_;
    winrt::com_ptr<IAudioCaptureClient> capture_;
    HANDLE event_ = nullptr;
    bool havePosition_ = false;
    int64_t nextPos_ = 0; // where the next packet goes if it is contiguous
};

} // namespace rec
