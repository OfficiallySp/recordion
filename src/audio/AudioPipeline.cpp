#include "audio/AudioPipeline.h"

#include "core/Clock.h"
#include "core/Log.h"
#include "core/ThreadQos.h"

#include <mmdeviceapi.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>

#include <immintrin.h>

namespace rec {
namespace {

constexpr int64_t kFrame = AacEncoder::kFrameSamples;
// Mix this far behind real time so every source has delivered (device buffers are ~10-20 ms;
// loopback can lag a little more under load).
constexpr int64_t kMixLatencyFrames = AudioTimeline::kRate / 10; // 100 ms
// Sources are read on a timer rather than on each device event (every ~10 ms per device): mixing
// runs 100 ms behind and device buffers hold 200 ms, so this halves the thread's wake-ups or better
// at no cost in latency.
constexpr DWORD kPollMs = 20;
constexpr int64_t kRetry100ns = 10'000'000; // reopen a lost device after 1 s

// Tells the audio thread when the user picks a different default output or input device, so
// "system audio" and "default microphone" follow it right away.
struct DefaultDeviceWatcher : winrt::implements<DefaultDeviceWatcher, IMMNotificationClient> {
    std::atomic<bool> outputChanged{false};
    std::atomic<bool> inputChanged{false};
    HANDLE wake = nullptr;

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (role != eConsole) return S_OK;
        (flow == eRender ? outputChanged : inputChanged).store(true);
        if (wake) SetEvent(wake);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

// Clamp to [-1, 1], scale and round to nearest, eight samples at a time (std::lrintf is a CRT call
// per sample).
void toPcm16(const float* in, int16_t* out, size_t n) {
    const __m128 lo = _mm_set1_ps(-1.0f), hi = _mm_set1_ps(1.0f), scale = _mm_set1_ps(32767.0f);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m128 a = _mm_mul_ps(_mm_min_ps(_mm_max_ps(_mm_loadu_ps(in + i), lo), hi), scale);
        const __m128 b = _mm_mul_ps(_mm_min_ps(_mm_max_ps(_mm_loadu_ps(in + i + 4), lo), hi), scale);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), _mm_packs_epi32(_mm_cvtps_epi32(a), _mm_cvtps_epi32(b)));
    }
    for (; i < n; ++i) out[i] = static_cast<int16_t>(std::lrintf(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f));
}

} // namespace

AudioPipeline::~AudioPipeline() { stop(); }

bool AudioPipeline::start(const AudioConfig& config, PacketFn onPacket) {
    stop();
    config_ = config;
    onPacket_ = std::move(onPacket);
    stopEvent_.create(wil::EventOptions::ManualReset);
    startedEvent_.create(wil::EventOptions::ManualReset);
    thread_ = std::thread([this] { run(); });
    WaitForSingleObject(startedEvent_.get(), INFINITE);
    if (!startOk_) stop();
    return startOk_;
}

void AudioPipeline::stop() {
    if (stopEvent_) stopEvent_.SetEvent();
    if (thread_.joinable()) thread_.join();
}

bool AudioPipeline::openSource(Source& s) {
    const bool ok = s.kind == WasapiSource::Kind::ProcessLoopback
                        ? s.wasapi.start(s.kind, config_.gamePid)
                        : s.wasapi.start(s.kind, 0, s.kind == WasapiSource::Kind::Microphone ? config_.micDeviceId
                                                                                             : std::wstring{});
    s.active = ok;
    if (!ok) s.retryAt = clock::now100ns() + kRetry100ns;
    return ok;
}

void AudioPipeline::run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    qos::setThreadName(L"audio");
    qos::setHighQos();
    qos::MmcssScope mmcss(L"Audio");

    timeline_.origin100ns = clock::now100ns();
    if (config_.captureSystem) {
        auto s = std::make_unique<Source>();
        s->game = true;
        s->kind = (config_.gameAudioOnly && config_.gamePid) ? WasapiSource::Kind::ProcessLoopback
                                                             : WasapiSource::Kind::SystemLoopback;
        if (!openSource(*s) && s->kind == WasapiSource::Kind::ProcessLoopback) {
            s->kind = WasapiSource::Kind::SystemLoopback; // e.g. an elevated game: fall back to everything
            openSource(*s);
        }
        sources_.push_back(std::move(s));
    }
    if (config_.captureMic) {
        auto s = std::make_unique<Source>();
        s->kind = WasapiSource::Kind::Microphone;
        openSource(*s); // may be retried later (mic plugged in after start)
        sources_.push_back(std::move(s));
    }

    const bool haveGame = config_.captureSystem;
    const bool haveMic = config_.captureMic;
    auto addTrack = [&](bool game, bool mic, std::string name, bool enabled) {
        auto t = std::make_unique<Track>();
        t->game = game;
        t->mic = mic;
        if (!t->encoder.init(config_.kbps)) return false;
        trackInfo_.push_back({.name = std::move(name), .enabled = enabled, .sampleRate = AacEncoder::kSampleRate,
                              .channels = AacEncoder::kChannels, .bitrate = t->encoder.bitrate(),
                              .primingSamples = 0, .audioSpecificConfig = t->encoder.audioSpecificConfig()});
        tracks_.push_back(std::move(t));
        return true;
    };
    startOk_ = !sources_.empty();
    if (startOk_) {
        const std::string label = config_.gameLabel;
        startOk_ = addTrack(haveGame, haveMic, haveGame && haveMic ? label + " + Mic" : haveGame ? label : "Mic", true);
        if (startOk_ && config_.separateTracks && haveGame && haveMic)
            startOk_ = addTrack(true, false, label, false) && addTrack(false, true, "Mic", false);
    }
    nextFrame_ = 0;
    for (auto& t : tracks_) t->firstFrame = nextFrame_;
    mix_.assign(kFrame * 2, 0.0f);
    pcm_.assign(kFrame * 2, 0);
    SetEvent(startedEvent_.get());
    if (!startOk_) {
        sources_.clear();
        tracks_.clear();
        return;
    }

    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    auto watcher = winrt::make_self<DefaultDeviceWatcher>();
    watcher->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put()))))
        enumerator->RegisterEndpointNotificationCallback(watcher.get());

    std::vector<HANDLE> waits{stopEvent_.get()};
    if (watcher->wake) waits.push_back(watcher->wake); // a null handle would fail every wait
    for (;;) {
        const DWORD r = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, kPollMs);
        if (r == WAIT_OBJECT_0) break;

        const int64_t now = clock::now100ns();
        const bool outputChanged = watcher->outputChanged.exchange(false);
        const bool inputChanged = watcher->inputChanged.exchange(false) && config_.micDeviceId.empty();
        for (auto& s : sources_) {
            const bool follow = (outputChanged && s->kind == WasapiSource::Kind::SystemLoopback) ||
                                (inputChanged && s->kind == WasapiSource::Kind::Microphone);
            if (follow && s->active) {
                log::info("audio: default device changed; switching");
                s->wasapi.stop();
                openSource(*s);
                continue;
            }
            if (s->active && !s->wasapi.read(timeline_, s->ring)) {
                log::info("audio: device lost; reopening");
                s->wasapi.stop();
                s->active = false;
                s->retryAt = now + kRetry100ns;
            } else if (!s->active && now >= s->retryAt) {
                openSource(*s); // picks up the new default device
            }
        }
        mixAndEncode(timeline_.toSample(now) - kMixLatencyFrames);
    }

    if (enumerator) enumerator->UnregisterEndpointNotificationCallback(watcher.get());
    if (watcher->wake) CloseHandle(watcher->wake);
    for (size_t ti = 0; ti < tracks_.size(); ++ti)
        tracks_[ti]->encoder.drain([&](std::span<const uint8_t> frame, uint64_t k) { emit(ti, k, frame); });
    for (auto& s : sources_) s->wasapi.stop();
    sources_.clear();
    tracks_.clear();
}

void AudioPipeline::emit(size_t trackIndex, uint64_t frameIndex, std::span<const uint8_t> frame) {
    // Frame k decodes to the input that started at sample firstFrame + k * 1024 - priming.
    const Track& t = *tracks_[trackIndex];
    const int64_t sample = t.firstFrame + static_cast<int64_t>(frameIndex) * kFrame - t.encoder.primingSamples();
    onPacket_(static_cast<uint8_t>(trackIndex + 1), timeline_.toTime(sample), frame);
}

void AudioPipeline::mixAndEncode(int64_t upTo) {
    while (nextFrame_ + kFrame <= upTo) {
        for (size_t ti = 0; ti < tracks_.size(); ++ti) {
            Track& t = *tracks_[ti];
            std::fill(mix_.begin(), mix_.end(), 0.0f);
            for (auto& s : sources_)
                if ((s->game && t.game) || (!s->game && t.mic)) s->ring.mixInto(nextFrame_, kFrame, 1.0f, mix_.data());
            toPcm16(mix_.data(), pcm_.data(), mix_.size());

            t.encoder.encode(pcm_, [&](std::span<const uint8_t> frame, uint64_t k) { emit(ti, k, frame); });
        }
        nextFrame_ += kFrame;
    }
}

} // namespace rec
