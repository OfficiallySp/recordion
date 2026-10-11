#include "audio/WasapiSource.h"

#include "core/Clock.h"
#include "core/Log.h"

#include <audioclientactivationparams.h>
#include <ksmedia.h>

#include <wil/resource.h>

#include <cstdlib>

namespace rec {
namespace {

constexpr REFERENCE_TIME kBufferDuration = 2'000'000; // 200 ms device buffer
constexpr int64_t kResyncFrames = 960;                // >20 ms off: jump (gap, discontinuity)
constexpr int64_t kNudgeFrames = 48;                  // >1 ms off: drift, correct one sample at a time

WAVEFORMATEXTENSIBLE floatStereo48k() {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = 2;
    f.Format.nSamplesPerSec = AudioTimeline::kRate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = 8;
    f.Format.nAvgBytesPerSec = AudioTimeline::kRate * 8;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return f;
}

// Completion handler for ActivateAudioInterfaceAsync. winrt::implements makes it agile, which the
// API requires.
struct ActivationHandler : winrt::implements<ActivationHandler, IActivateAudioInterfaceCompletionHandler> {
    wil::unique_event_nothrow done;
    ActivationHandler() { done.create(wil::EventOptions::ManualReset); }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        done.SetEvent();
        return S_OK;
    }
};

winrt::com_ptr<IAudioClient> activateProcessLoopback(DWORD pid) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto handler = winrt::make_self<ActivationHandler>();
    winrt::com_ptr<IActivateAudioInterfaceAsyncOperation> op;
    if (FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv,
                                           handler.get(), op.put())))
        return nullptr;
    if (WaitForSingleObject(handler->done.get(), 5000) != WAIT_OBJECT_0) return nullptr;
    HRESULT hr = E_FAIL;
    winrt::com_ptr<::IUnknown> unknown;
    if (FAILED(op->GetActivateResult(&hr, unknown.put())) || FAILED(hr) || !unknown) return nullptr;
    return unknown.as<IAudioClient>();
}

winrt::com_ptr<IAudioClient> activateEndpoint(EDataFlow flow, const std::wstring& deviceId) {
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put()))))
        return nullptr;
    winrt::com_ptr<IMMDevice> device;
    const HRESULT hr = deviceId.empty() ? enumerator->GetDefaultAudioEndpoint(flow, eConsole, device.put())
                                        : enumerator->GetDevice(deviceId.c_str(), device.put());
    if (FAILED(hr)) return nullptr;
    winrt::com_ptr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()))) return nullptr;
    return client;
}

const char* kindName(WasapiSource::Kind k) {
    switch (k) {
    case WasapiSource::Kind::SystemLoopback: return "system audio";
    case WasapiSource::Kind::ProcessLoopback: return "game audio";
    case WasapiSource::Kind::Microphone: return "microphone";
    }
    return "?";
}

} // namespace

WasapiSource::~WasapiSource() { stop(); }

bool WasapiSource::start(Kind kind, DWORD pid, const std::wstring& deviceId) {
    stop();
    kind_ = kind;
    switch (kind) {
    case Kind::SystemLoopback: client_ = activateEndpoint(eRender, {}); break;
    case Kind::ProcessLoopback: client_ = activateProcessLoopback(pid); break;
    case Kind::Microphone: client_ = activateEndpoint(eCapture, deviceId); break;
    }
    if (!client_ || !initClient(kind != Kind::Microphone)) {
        log::warn("audio: {} unavailable", kindName(kind));
        stop();
        return false;
    }
    log::info("audio: capturing {}", kindName(kind));
    return true;
}

bool WasapiSource::initClient(bool loopback) {
    auto format = floatStereo48k();
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0,
                                     reinterpret_cast<WAVEFORMATEX*>(&format), nullptr);
    if (FAILED(hr)) {
        log::warn("audio: Initialize failed (0x{:08X})", static_cast<uint32_t>(hr));
        return false;
    }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(client_->SetEventHandle(event_)) || FAILED(client_->GetService(IID_PPV_ARGS(capture_.put()))) ||
        FAILED(client_->Start()))
        return false;
    havePosition_ = false;
    return true;
}

void WasapiSource::stop() {
    if (client_) client_->Stop();
    capture_ = nullptr;
    client_ = nullptr;
    if (event_) {
        CloseHandle(event_);
        event_ = nullptr;
    }
}

bool WasapiSource::read(const AudioTimeline& timeline, AudioRing& ring) {
    if (!capture_) return false;
    for (;;) {
        UINT32 packet = 0;
        HRESULT hr = capture_->GetNextPacketSize(&packet);
        if (FAILED(hr)) return false; // device invalidated (unplugged, default changed, ...)
        if (packet == 0) return true;

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 devicePosition = 0, qpcPosition = 0;
        hr = capture_->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) return true;
        if (FAILED(hr)) return false;

        // qpcPosition: when the first frame of this packet was recorded (100 ns, QPC timeline).
        const int64_t measured = qpcPosition ? timeline.toSample(static_cast<int64_t>(qpcPosition))
                                             : timeline.toSample(clock::now100ns()) - frames;
        int64_t pos = measured;
        if (havePosition_ && !(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) {
            const int64_t error = measured - nextPos_;
            if (std::llabs(error) <= kResyncFrames) {
                // Contiguous: ignore timestamp jitter, but follow slow clock drift a sample at a time.
                pos = nextPos_ + (error > kNudgeFrames ? 1 : error < -kNudgeFrames ? -1 : 0);
            }
            // else: a real gap (loopback is silent while nothing plays) or a stall: resync.
        }

        const float* samples = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : reinterpret_cast<const float*>(data);
        size_t count = frames;
        if (pos < 0) { // recorded before the session's timeline began
            const size_t skip = std::min<size_t>(count, static_cast<size_t>(-pos));
            if (samples) samples += skip * 2;
            count -= skip;
            pos += static_cast<int64_t>(skip);
        }
        ring.write(pos, samples, count);
        nextPos_ = pos + static_cast<int64_t>(count);
        havePosition_ = true;
        capture_->ReleaseBuffer(frames);
    }
}

} // namespace rec
