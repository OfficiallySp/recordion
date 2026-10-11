#pragma once

#include "audio/AacEncoder.h"
#include "audio/AudioRing.h"
#include "audio/AudioTimeline.h"
#include "audio/WasapiSource.h"
#include "mux/Mp4Writer.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <wil/resource.h>

namespace rec {

struct AudioConfig {
    bool captureSystem = true;  // game (or desktop) audio
    DWORD gamePid = 0;          // non-zero: capture only this process tree, if gameAudioOnly
    bool gameAudioOnly = true;
    bool captureMic = false;
    std::wstring micDeviceId;   // empty = default input device
    bool separateTracks = false; // also write "Game" and "Mic" as tracks 2 and 3
    uint32_t kbps = 192;
    std::string gameLabel = "Game"; // "Game" or "Desktop" in track names
};

// Captures, mixes and AAC-encodes audio on its own thread. Track 1 is everything mixed (what every
// player plays); with separateTracks, tracks 2 and 3 hold the game and the mic on their own.
class AudioPipeline {
public:
    // Called on the audio thread for every encoded frame. `track` is 1-based.
    using PacketFn = std::function<void(uint8_t track, int64_t pts100ns, std::span<const uint8_t> frame)>;

    ~AudioPipeline();
    // Starts capture; packets are timestamped on the QPC timeline. Returns false if no audio
    // source could be opened (the session then records video only).
    bool start(const AudioConfig& config, PacketFn onPacket);
    void stop(); // drains the encoders

    // Container descriptions of the tracks, in track order. Valid after start().
    const std::vector<Mp4AudioTrack>& tracks() const { return trackInfo_; }

private:
    struct Source {
        WasapiSource::Kind kind;
        WasapiSource wasapi;
        AudioRing ring;
        bool game = false;     // game/system audio rather than the mic
        int64_t retryAt = 0;   // when to reopen after the device went away
        bool active = false;
    };
    struct Track {
        bool game = false;
        bool mic = false;
        AacEncoder encoder;
        int64_t firstFrame = 0;
    };

    bool openSource(Source& s);
    void run();
    void mixAndEncode(int64_t upToSample);
    void emit(size_t trackIndex, uint64_t frameIndex, std::span<const uint8_t> frame);

    AudioConfig config_;
    PacketFn onPacket_;
    AudioTimeline timeline_;
    std::vector<std::unique_ptr<Source>> sources_;
    std::vector<std::unique_ptr<Track>> tracks_;
    std::vector<Mp4AudioTrack> trackInfo_;
    int64_t nextFrame_ = 0; // next sample position to mix
    std::vector<float> mix_;
    std::vector<int16_t> pcm_;
    wil::unique_event_nothrow stopEvent_;
    wil::unique_event_nothrow startedEvent_;
    bool startOk_ = false;
    std::thread thread_;
};

} // namespace rec
