#include "pipeline/CaptureSession.h"

#include "capture/FakeCapture.h"
#include "capture/WgcCapture.h"
#include "core/Clock.h"
#include "core/FrameTimer.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/ThreadQos.h"
#include "core/Utf.h"
#include "encode/AmfEncoder.h"
#include "mux/Bitstream.h"
#include "output/ClipSaver.h"
#include "output/Recorder.h"

#include <winrt/base.h>

namespace rec {
namespace {

constexpr uint32_t kZeroCopyPoolBuffers = 6; // encoder in flight + newest + held for duplicates + DWM
constexpr uint32_t kMaxConsecutiveErrors = 30;

const char* codecName(Codec c) { return c == Codec::H264 ? "H.264" : c == Codec::HEVC ? "HEVC" : "AV1"; }

} // namespace

// Turns encoder output into sample-format packets in the replay buffer.
class CaptureSession::BufferSink final : public PacketSink {
public:
    BufferSink(ReplayBuffer& buffer, Codec codec, uint32_t segment, uint32_t fps, std::vector<Mp4AudioTrack> audio,
               std::atomic<uint64_t>& encoded)
        : buffer_(buffer), codec_(codec), segment_(segment), fps_(fps), audio_(std::move(audio)), encoded_(encoded) {
        scratch_.reserve(1 << 20);
    }

    void onPacket(const EncodedPacket& p) override {
        if (!configured_) {
            if (!p.keyframe) return; // nothing before the first keyframe is decodable
            const auto info = bitstream::parseVideoConfig(codec_, p.data);
            if (!info) {
                log::error("session: no parameter sets in the first keyframe");
                return;
            }
            auto segment = std::make_shared<SegmentInfo>();
            // The bitstream's own size: e.g. AMD RDNA3 AV1 codes 1080p as 1082 lines.
            segment->video = {codec_, info->width, info->height, fps_, 1, info->configRecord};
            segment->audio = audio_;
            buffer_.setSegmentInfo(segment_, std::move(segment));
            configured_ = true;
        }
        scratch_.clear();
        bitstream::appendSample(codec_, p.data, scratch_);
        buffer_.append(kVideoTrack, p.pts100ns, p.keyframe, segment_, scratch_);
        encoded_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    ReplayBuffer& buffer_;
    Codec codec_;
    uint32_t segment_;
    uint32_t fps_;
    std::vector<Mp4AudioTrack> audio_;
    std::atomic<uint64_t>& encoded_;
    bool configured_ = false;
    std::vector<uint8_t> scratch_;
};

CaptureSession::CaptureSession(SessionConfig config)
    : config_(std::move(config)), buffer_(std::make_shared<ReplayBuffer>()) {
    stopEvent_.create(wil::EventOptions::ManualReset);
    startedEvent_.create(wil::EventOptions::ManualReset);
}

CaptureSession::~CaptureSession() { stop(); }

bool CaptureSession::start() {
    if (thread_.joinable()) return running();
    const auto& s = config_.settings;
    ReplayBuffer::Options o;
    o.capacityBytes = replayBufferBytes(s.replay.seconds, s.video.gopSeconds,
                                        std::max(s.video.maxBitrateKbps, s.video.bitrateKbps), s.audio.bitrateKbps * 3);
    o.maxDuration100ns = clock::secondsTo100ns(s.replay.seconds + s.video.gopSeconds + 1.0);
    o.storage = s.replay.storage;
    o.diskDirectory = paths::logDir().parent_path() / L"buffer";
    if (!buffer_->init(o)) return false;

    // Audio first: its tracks are part of every segment's description.
    AudioConfig ac;
    ac.captureSystem = s.audio.captureSystem;
    ac.gamePid = config_.processId;
    ac.gameAudioOnly = s.audio.gameAudioOnly;
    ac.captureMic = s.audio.captureMic;
    ac.micDeviceId = widen(s.audio.micDeviceId);
    ac.separateTracks = s.audio.separateTracks;
    ac.kbps = s.audio.bitrateKbps;
    ac.gameLabel = config_.processId ? "Game" : "Desktop";
    audioTracks_.clear();
    if ((ac.captureSystem || ac.captureMic) &&
        audio_.start(ac, [this](uint8_t track, int64_t pts, std::span<const uint8_t> frame) {
            buffer_->append(track, pts, true, currentSegment_.load(std::memory_order_relaxed), frame);
        })) {
        audioTracks_ = audio_.tracks();
    } else if (ac.captureSystem || ac.captureMic) {
        log::warn("session: no audio device available; recording video only");
    }

    stopEvent_.ResetEvent();
    startedEvent_.ResetEvent();
    thread_ = std::thread([this] { run(); });
    WaitForSingleObject(startedEvent_.get(), INFINITE);
    if (!startOk_) stop();
    return startOk_;
}

void CaptureSession::stop() {
    stopEvent_.SetEvent();
    if (thread_.joinable()) thread_.join();
    audio_.stop();
    running_ = false;
}

void CaptureSession::fail(const char* what) {
    log::error("session \"{}\": {}", config_.name, what);
    stopEvent_.SetEvent();
    if (config_.notifyWindow) PostMessageW(config_.notifyWindow, config_.notifyMessage, kSessionFailed, 0);
}

bool CaptureSession::initPipeline() {
    const auto& s = config_.settings;
    HMONITOR monitor = config_.target.monitor;
    if (config_.target.window) monitor = MonitorFromWindow(config_.target.window, MONITOR_DEFAULTTOPRIMARY);
    auto gpu = createGpuDevice({.monitor = monitor, .highGpuPriority = s.capture.highGpuPriority});
    if (!gpu) {
        log::error("session: {}", gpu.error());
        return false;
    }
    gpu_ = std::move(*gpu);
    if (!AmfEncoder::runtimeAvailable()) {
        // NVENC and QSV backends come next; until then AMF is required.
        log::error("session: no supported hardware encoder on {}", gpu_->adapterName);
        return false;
    }

    if (config_.synthetic) {
        source_ = std::make_unique<FakeCapture>(*gpu_, 1920, 1080);
    } else {
        source_ = std::make_unique<WgcCapture>(*gpu_, config_.target,
                                               CaptureOptions{.fps = s.video.fps, .cursor = s.capture.captureCursor,
                                                              .poolBuffers = kZeroCopyPoolBuffers});
    }
    return source_->start();
}

bool CaptureSession::configureEncoder(Size captureSize, Size textureSize) {
    const auto& s = config_.settings;
    if (encoder_) encoder_->flush(*sink_); // finish the old segment
    encoder_.reset();
    lastFrame_.reset();
    lastSurface_.reset();
    converter_.reset();

    outputSize_ = scaledOutputSize(captureSize, s.video.outputHeight);
    EncoderConfig ec{.codec = s.video.codec,
                     .tenBit = s.video.tenBit && s.video.codec != Codec::H264,
                     .width = outputSize_.width,
                     .height = outputSize_.height,
                     .fpsNum = s.video.fps,
                     .fpsDen = 1,
                     .rateControl = s.video.rateControl,
                     .bitrateKbps = s.video.bitrateKbps,
                     .maxBitrateKbps = s.video.maxBitrateKbps,
                     .cqLevel = s.video.cqLevel,
                     .gopFrames = s.video.fps * std::max(1u, s.video.gopSeconds),
                     .preset = s.video.preset};

    // Zero-copy when nothing needs scaling and the encoder converts RGB in fixed-function hardware:
    // the captured frame goes straight to the encoder, with no shader pass and no extra copy.
    const auto path = s.capture.convertPath;
    const bool canZeroCopy = (path == ConvertPath::Auto || path == ConvertPath::EncoderNative) &&
                             outputSize_ == captureSize && captureSize == textureSize && !ec.tenBit;
    zeroCopy_ = false;
    if (canZeroCopy) {
        auto enc = std::make_unique<AmfEncoder>();
        ec.rgbInput = true;
        if (enc->init(*gpu_, ec) && (enc->hardwareRgbConversion() || path == ConvertPath::EncoderNative)) {
            encoder_ = std::move(enc);
            zeroCopy_ = true;
        }
    }
    if (!encoder_) {
        ec.rgbInput = false;
        auto enc = std::make_unique<AmfEncoder>();
        if (!enc->init(*gpu_, ec)) return false;
        encoder_ = std::move(enc);
        if (!converter_.init(gpu_->device.get(), gpu_->context.get(), textureSize, outputSize_, encoder_->inputFormat(),
                             s.video.fps))
            return false;
    }

    ++segment_;
    currentSegment_ = segment_;
    sink_ = std::make_unique<BufferSink>(*buffer_, s.video.codec, segment_, s.video.fps, audioTracks_, framesEncoded_);
    captureSize_ = captureSize;
    forceKeyframe_ = true;
    statsZeroCopy_ = zeroCopy_;
    statsWidth_ = outputSize_.width;
    statsHeight_ = outputSize_.height;
    log::info("session \"{}\": {} {}x{} -> {}x{} via {} ({})", config_.name, codecName(s.video.codec), captureSize.width,
              captureSize.height, outputSize_.width, outputSize_.height, encoder_->name(),
              zeroCopy_ ? "zero-copy RGB" : "video processor");
    return true;
}

void CaptureSession::tick(int64_t pts) {
    auto frame = source_->acquireLatest();
    bool ok = true;
    if (frame) {
        framesCaptured_.fetch_add(1, std::memory_order_relaxed);
        D3D11_TEXTURE2D_DESC td{};
        frame->texture->GetDesc(&td);
        const Size textureSize{td.Width, td.Height};
        const Size captureSize{frame->width, frame->height};
        if (!encoder_ || captureSize != captureSize_ || (zeroCopy_ && textureSize != outputSize_)) {
            if (!configureEncoder(captureSize, textureSize)) {
                fail("could not configure the encoder");
                return;
            }
        }
        const uint32_t flags = forceKeyframe_ ? kFrameForceKeyframe : kFrameNone;
        if (zeroCopy_) {
            ok = encoder_->encodeExternal(frame->texture.get(), frame->owner, pts, flags);
            lastFrame_ = std::move(frame);
        } else if (auto surface = encoder_->acquireSurface()) {
            if (converter_.inputSize() != textureSize &&
                !converter_.init(gpu_->device.get(), gpu_->context.get(), textureSize, outputSize_,
                                 encoder_->inputFormat(), config_.settings.video.fps)) {
                fail("could not configure the colour converter");
                return;
            }
            ok = converter_.convert(frame->texture.get(), captureSize, surface->texture, surface->arraySlice) &&
                 encoder_->encode(*surface, pts, flags);
            lastSurface_ = surface;
        } else {
            droppedBusy_.fetch_add(1, std::memory_order_relaxed); // every surface still in the encoder
            frame.reset();
        }
        if (ok) forceKeyframe_ = false;
    } else if (encoder_ && (lastFrame_ || lastSurface_)) {
        // Nothing changed on screen: repeat the previous frame to keep a constant frame rate.
        duplicates_.fetch_add(1, std::memory_order_relaxed);
        ok = zeroCopy_ ? encoder_->encodeExternal(lastFrame_->texture.get(), lastFrame_->owner, pts, kFrameDuplicate)
                       : encoder_->encode(*lastSurface_, pts, kFrameDuplicate);
    }

    if (!ok) {
        encodeErrors_.fetch_add(1, std::memory_order_relaxed);
        if (++consecutiveErrors_ >= kMaxConsecutiveErrors) {
            fail("the encoder keeps failing (GPU reset or driver update?)");
            return;
        }
    } else {
        consecutiveErrors_ = 0;
    }
    if (encoder_) encoder_->drain(*sink_);
}

void CaptureSession::run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    qos::setThreadName(L"capture pump");
    qos::setHighQos();
    qos::MmcssScope mmcss(L"Capture");

    startOk_ = initPipeline();
    running_ = startOk_.load();
    SetEvent(startedEvent_.get());
    if (!startOk_) {
        source_.reset();
        gpu_.reset();
        return;
    }

    FrameTimer timer(config_.settings.video.fps);
    while (timer.waitNext(stopEvent_.get()) && !source_->targetLost()) tick(timer.tickTime100ns());
    if (source_->targetLost()) {
        log::info("session \"{}\": capture target closed", config_.name);
        if (config_.notifyWindow) PostMessageW(config_.notifyWindow, config_.notifyMessage, kSessionEnded, 0);
    }

    if (encoder_) encoder_->flush(*sink_);
    lastFrame_.reset();
    lastSurface_.reset();
    encoder_.reset();
    converter_.reset();
    source_->stop();
    source_.reset();
    gpu_.reset();
    running_ = false;
}

void CaptureSession::saveReplay(ClipSaver& saver, uint32_t seconds) const {
    SaveJob job;
    job.buffer = buffer_;
    job.anchorQpc100ns = clock::now100ns();
    GetSystemTimePreciseAsFileTime(&job.anchorWall);
    job.to100ns = job.anchorQpc100ns;
    job.from100ns = job.anchorQpc100ns - clock::secondsTo100ns(seconds);
    job.root = config_.settings.outputRoot();
    job.fileTemplate = config_.settings.output.fileTemplate;
    job.game = config_.name;
    job.type = "Replay";
    saver.enqueue(std::move(job));
}

bool CaptureSession::startRecording(Recorder& recorder, uint32_t prerollSeconds) const {
    RecordJob job;
    job.buffer = buffer_;
    job.anchorQpc100ns = clock::now100ns();
    GetSystemTimePreciseAsFileTime(&job.anchorWall);
    job.from100ns = job.anchorQpc100ns - clock::secondsTo100ns(prerollSeconds);
    job.root = config_.settings.outputRoot();
    job.fileTemplate = config_.settings.output.fileTemplate;
    job.game = config_.name;
    return recorder.start(std::move(job));
}

SessionStats CaptureSession::stats() const {
    SessionStats s;
    s.framesCaptured = framesCaptured_.load(std::memory_order_relaxed);
    s.framesEncoded = framesEncoded_.load(std::memory_order_relaxed);
    s.duplicates = duplicates_.load(std::memory_order_relaxed);
    s.droppedBusy = droppedBusy_.load(std::memory_order_relaxed);
    s.encodeErrors = encodeErrors_.load(std::memory_order_relaxed);
    s.zeroCopy = statsZeroCopy_.load(std::memory_order_relaxed);
    s.audioTracks = audioTracks_.size();
    s.outputSize = {statsWidth_.load(std::memory_order_relaxed), statsHeight_.load(std::memory_order_relaxed)};
    s.buffer = buffer_->stats();
    return s;
}

} // namespace rec
