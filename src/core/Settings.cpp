#include "core/Settings.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"

#include <windows.h>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace rec {

NLOHMANN_JSON_SERIALIZE_ENUM(Codec, {{Codec::HEVC, "hevc"}, {Codec::H264, "h264"}, {Codec::AV1, "av1"}})
NLOHMANN_JSON_SERIALIZE_ENUM(RateControl, {{RateControl::VBR, "vbr"}, {RateControl::CBR, "cbr"}, {RateControl::CQP, "cqp"}})
NLOHMANN_JSON_SERIALIZE_ENUM(EncoderVendor, {{EncoderVendor::Auto, "auto"},
                                             {EncoderVendor::AMD, "amd"},
                                             {EncoderVendor::NVIDIA, "nvidia"},
                                             {EncoderVendor::Intel, "intel"}})
NLOHMANN_JSON_SERIALIZE_ENUM(QualityPreset, {{QualityPreset::Balanced, "balanced"},
                                             {QualityPreset::Speed, "speed"},
                                             {QualityPreset::Quality, "quality"},
                                             {QualityPreset::MaxQuality, "max_quality"}})
NLOHMANN_JSON_SERIALIZE_ENUM(CaptureMethod, {{CaptureMethod::Auto, "auto"},
                                             {CaptureMethod::WGC, "wgc"},
                                             {CaptureMethod::DDA, "dda"},
                                             {CaptureMethod::AmdDirect, "amd_direct"}})
NLOHMANN_JSON_SERIALIZE_ENUM(ConvertPath, {{ConvertPath::Auto, "auto"},
                                           {ConvertPath::VideoProcessor, "video_processor"},
                                           {ConvertPath::Compute, "compute"},
                                           {ConvertPath::EncoderNative, "encoder_native"}})
NLOHMANN_JSON_SERIALIZE_ENUM(BufferStorage, {{BufferStorage::RAM, "ram"}, {BufferStorage::Disk, "disk"}})

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(VideoSettings, vendor, codec, tenBit, rateControl, bitrateKbps,
                                                maxBitrateKbps, cqLevel, fps, outputHeight, gopSeconds, preset)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AudioSettings, captureSystem, gameAudioOnly, captureMic, micDeviceId,
                                                separateTracks, bitrateKbps)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ReplaySettings, enabledInGames, desktopAlwaysOn, seconds, storage)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CaptureSettings, method, convertPath, captureCursor, gameWindowOnly,
                                                highGpuPriority)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(OutputSettings, folder, fileTemplate)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(HotkeySettings, saveReplay, toggleRecording, toggleReplayBuffer,
                                                toggleDesktopRecording)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DetectionSettings, enabled, alwaysRecord, neverRecord)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(GeneralSettings, startWithWindows, notificationSound)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Settings, video, audio, replay, capture, output, hotkeys, detection,
                                                general)

std::filesystem::path Settings::outputRoot() const {
    return output.folder.empty() ? paths::defaultOutputDir() : std::filesystem::path(widen(output.folder));
}

Settings Settings::fromJson(std::string_view json) {
    try {
        return nlohmann::json::parse(json).get<Settings>();
    } catch (const nlohmann::json::exception& e) {
        log::warn("settings: invalid JSON, using defaults ({})", e.what());
        return {};
    }
}

Settings Settings::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return fromJson(ss.str());
}

std::string Settings::toJson() const { return nlohmann::json(*this).dump(2); }

bool Settings::save(const std::filesystem::path& file) const {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << toJson();
        if (!out.flush()) return false;
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log::error("settings: could not replace {} (error {})", narrow(file.wstring()), GetLastError());
        return false;
    }
    return true;
}

} // namespace rec
