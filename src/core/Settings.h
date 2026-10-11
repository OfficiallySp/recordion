#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rec {

enum class Codec { H264, HEVC, AV1 };
enum class RateControl { VBR, CBR, CQP };
enum class EncoderVendor { Auto, AMD, NVIDIA, Intel };
enum class QualityPreset { Speed, Balanced, Quality, MaxQuality }; // MaxQuality enables shader-assisted features
enum class CaptureMethod { Auto, WGC, DDA, AmdDirect };
enum class ConvertPath { Auto, VideoProcessor, Compute, EncoderNative };
enum class BufferStorage { RAM, Disk };

struct VideoSettings {
    EncoderVendor vendor = EncoderVendor::Auto;
    Codec codec = Codec::HEVC;
    bool tenBit = false;
    RateControl rateControl = RateControl::VBR;
    uint32_t bitrateKbps = 50'000;
    uint32_t maxBitrateKbps = 75'000;
    uint32_t cqLevel = 23;
    uint32_t fps = 60;
    uint32_t outputHeight = 0; // 0 = native
    uint32_t gopSeconds = 2;
    QualityPreset preset = QualityPreset::Balanced;
};

struct AudioSettings {
    bool captureSystem = true;
    bool gameAudioOnly = true; // per-process loopback during game sessions
    bool captureMic = false;
    std::string micDeviceId;   // empty = default communications device
    bool separateTracks = false;
    uint32_t bitrateKbps = 192;
};

struct ReplaySettings {
    bool enabledInGames = true;
    bool desktopAlwaysOn = false;
    uint32_t seconds = 120;
    BufferStorage storage = BufferStorage::RAM;
};

struct CaptureSettings {
    CaptureMethod method = CaptureMethod::Auto;
    ConvertPath convertPath = ConvertPath::Auto;
    bool captureCursor = true;
    bool gameWindowOnly = false;
    bool highGpuPriority = true;
};

struct OutputSettings {
    std::string folder; // empty = <Videos>\Recordion
    std::string fileTemplate = "{game} {date} {time} {type}";
};

// Key combos as text (see parseHotkey) so settings.json stays hand-editable.
struct HotkeySettings {
    std::string saveReplay = "Alt+F10";
    std::string toggleRecording = "Alt+F9";
    std::string toggleReplayBuffer = "Alt+Shift+F10";
    std::string toggleDesktopRecording = "Alt+F11";
};

struct DetectionSettings {
    bool enabled = true;
    std::vector<std::string> alwaysRecord; // exe paths
    std::vector<std::string> neverRecord;  // exe paths
};

struct GeneralSettings {
    bool startWithWindows = false;
    bool notificationSound = true;
};

struct Settings {
    VideoSettings video;
    AudioSettings audio;
    ReplaySettings replay;
    CaptureSettings capture;
    OutputSettings output;
    HotkeySettings hotkeys;
    DetectionSettings detection;
    GeneralSettings general;

    std::filesystem::path outputRoot() const;

    // Missing or unknown fields fall back to defaults, so old settings files keep working.
    static Settings load(const std::filesystem::path& file);
    static Settings fromJson(std::string_view json);
    std::string toJson() const;
    bool save(const std::filesystem::path& file) const; // atomic replace
};

} // namespace rec
