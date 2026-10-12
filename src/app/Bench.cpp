#include "app/Bench.h"

#include "capture/FakeCapture.h"
#include "capture/WgcCapture.h"
#include "core/Clock.h"
#include "core/FrameTimer.h"
#include "core/Log.h"
#include "core/ThreadQos.h"
#include "core/Utf.h"
#include "encode/AmfEncoder.h"
#include "gpu/ColorConverter.h"
#include "gpu/D3DDevice.h"
#include "gpu/GpuTimer.h"
#include "mux/Bitstream.h"
#include "mux/Mp4Writer.h"
#include "output/ClipSaver.h"
#include "output/Recorder.h"
#include "pipeline/CaptureSession.h"

#include <windows.h>
#include <intrin.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <print>

namespace rec {
namespace {

struct BenchOptions {
    double seconds = 10;
    uint32_t fps = 60;
    uint32_t height = 0; // 0 = native
    bool tenBit = false;
    bool cursor = true;
    int monitorIndex = 0;
    std::optional<Codec> codec; // set = encode, otherwise capture + convert only
    uint32_t bitrateKbps = 50'000;
    QualityPreset preset = QualityPreset::Balanced;
    std::wstring out;       // MP4 output (encode mode)
    std::wstring dumpFrame; // raw NV12/P010 file of one converted frame (convert mode)
    Size synthetic{};       // non-zero = use the synthetic source instead of the screen
    bool rgbInput = false;  // hand BGRA to the encoder (no VideoProcessor pass)
    bool session = false;   // run the real CaptureSession + replay save instead of the raw loop
    bool forceVideoProcessor = false;
    uint32_t replaySeconds = 5;
    std::wstring outDir;
    DWORD pid = 0; // session mode: capture only this process's audio
    double recordSeconds = 0; // session mode: also make a manual recording this long
};

void usage() {
    std::println("usage: recordion --bench [--seconds N] [--fps N] [--height N] [--10bit] [--no-cursor] [--monitor N]\n"
                 "                         [--codec h264|hevc|av1 [--bitrate KBPS] [--preset speed|balanced|quality]\n"
                 "                          [--out file.mp4]]\n"
                 "                         [--dump-frame file.yuv]");
}

bool parse(const std::vector<std::wstring>& args, BenchOptions& o) {
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        auto next = [&]() -> const std::wstring* { return i + 1 < args.size() ? &args[++i] : nullptr; };
        if (a == L"--bench") continue;
        if (a == L"--10bit") { o.tenBit = true; continue; }
        if (a == L"--no-cursor") { o.cursor = false; continue; }
        if (a == L"--synthetic") { o.synthetic = {1920, 1080}; continue; }
        if (a == L"--rgb-input") { o.rgbInput = true; continue; }
        if (a == L"--session") { o.session = true; continue; }
        if (a == L"--video-processor") { o.forceVideoProcessor = true; continue; }
        const std::wstring* v = next();
        if (!v) return false;
        if (a == L"--seconds") o.seconds = std::stod(*v);
        else if (a == L"--fps") o.fps = static_cast<uint32_t>(std::stoul(*v));
        else if (a == L"--height") o.height = static_cast<uint32_t>(std::stoul(*v));
        else if (a == L"--monitor") o.monitorIndex = std::stoi(*v);
        else if (a == L"--bitrate") o.bitrateKbps = static_cast<uint32_t>(std::stoul(*v));
        else if (a == L"--out") o.out = *v;
        else if (a == L"--dump-frame") o.dumpFrame = *v;
        else if (a == L"--replay") o.replaySeconds = static_cast<uint32_t>(std::stoul(*v));
        else if (a == L"--out-dir") o.outDir = *v;
        else if (a == L"--pid") o.pid = static_cast<DWORD>(std::stoul(*v));
        else if (a == L"--record") o.recordSeconds = std::stod(*v);
        else if (a == L"--synthetic-size") {
            const auto x = v->find(L'x');
            if (x == std::wstring::npos) return false;
            o.synthetic = {static_cast<uint32_t>(std::stoul(v->substr(0, x))), static_cast<uint32_t>(std::stoul(v->substr(x + 1)))};
        }
        else if (a == L"--codec") {
            if (*v == L"h264") o.codec = Codec::H264;
            else if (*v == L"hevc") o.codec = Codec::HEVC;
            else if (*v == L"av1") o.codec = Codec::AV1;
            else return false;
        } else if (a == L"--preset") {
            if (*v == L"speed") o.preset = QualityPreset::Speed;
            else if (*v == L"balanced") o.preset = QualityPreset::Balanced;
            else if (*v == L"quality") o.preset = QualityPreset::Quality;
            else return false;
        } else return false;
    }
    return o.fps > 0 && o.seconds > 0;
}

HMONITOR monitorByIndex(int index) {
    struct Ctx { int want, seen = 0; HMONITOR found = nullptr; } ctx{index};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(p);
        if (c->seen++ == c->want) { c->found = m; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found ? ctx.found : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

winrt::com_ptr<ID3D11Texture2D> createYuvTexture(ID3D11Device* device, Size size, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = size.width;
    d.Height = size.height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> tex;
    device->CreateTexture2D(&d, nullptr, tex.put());
    return tex;
}

// Copies a converted NV12/P010 frame to the CPU and writes both planes, tightly packed.
bool dumpYuv(const GpuDevice& gpu, ID3D11Texture2D* tex, Size size, bool tenBit, const std::wstring& path) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(gpu.device->CreateTexture2D(&d, nullptr, staging.put()))) return false;
    gpu.context->CopyResource(staging.get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(gpu.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    std::ofstream out(path, std::ios::binary);
    const size_t rowBytes = size.width * (tenBit ? 2 : 1);
    const auto* base = static_cast<const uint8_t*>(m.pData);
    for (uint32_t y = 0; y < size.height; ++y) out.write(reinterpret_cast<const char*>(base + y * m.RowPitch), rowBytes);
    // The interleaved UV plane follows the luma plane (texture height rows down) at the same pitch.
    const uint8_t* uv = base + static_cast<size_t>(m.RowPitch) * d.Height;
    for (uint32_t y = 0; y < size.height / 2; ++y) out.write(reinterpret_cast<const char*>(uv + y * m.RowPitch), rowBytes);
    gpu.context->Unmap(staging.get(), 0);
    return static_cast<bool>(out);
}

class OfstreamSink final : public ByteSink {
public:
    explicit OfstreamSink(const std::wstring& path) : out_(path, std::ios::binary | std::ios::trunc) {}
    bool write(std::span<const uint8_t> b) override {
        out_.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        pos_ += b.size();
        return static_cast<bool>(out_);
    }
    uint64_t position() const override { return pos_; }
    bool ok() const { return static_cast<bool>(out_); }

private:
    std::ofstream out_;
    uint64_t pos_ = 0;
};

uint64_t threadCycles() {
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return c;
}

uint64_t processCycles() {
    ULONG64 c = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &c);
    return c;
}

// Cycle counter rate, measured against QPC (cycle counts are exact; GetThreadTimes is 15.6 ms granular).
double cyclesPerSecond() {
    const uint64_t c0 = __rdtsc();
    const int64_t t0 = clock::now100ns();
    Sleep(50);
    return static_cast<double>(__rdtsc() - c0) / clock::toSeconds(clock::now100ns() - t0);
}

// Wall time and CPU cycles of a code section on the pump thread.
struct Section {
    const char* name;
    int64_t wall = 0;
    uint64_t cycles = 0;
    uint64_t calls = 0;
};
struct SectionScope {
    explicit SectionScope(Section& s) : s_(s), start_(clock::now100ns()), cycles_(threadCycles()) {}
    ~SectionScope() {
        s_.wall += clock::now100ns() - start_;
        s_.cycles += threadCycles() - cycles_;
        ++s_.calls;
    }
    Section& s_;
    int64_t start_;
    uint64_t cycles_;
};

// Collects encoder output in sample format, plus latency statistics.
class CollectingSink final : public PacketSink {
public:
    explicit CollectingSink(Codec codec) : codec_(codec) {}

    void submitted(int64_t pts) { submitTime_[pts] = clock::now100ns(); }

    void onPacket(const EncodedPacket& p) override {
        SectionScope _(section);
        if (auto it = submitTime_.find(p.pts100ns); it != submitTime_.end()) {
            const double ms = clock::toSeconds(clock::now100ns() - it->second) * 1000.0;
            latencyTotalMs += ms;
            latencyMaxMs = std::max(latencyMaxMs, ms);
            ++latencySamples;
            submitTime_.erase(it);
        }
        if (samples.empty() && !p.keyframe) return; // a file must start on a keyframe
        if (p.keyframe && !config) config = bitstream::parseVideoConfig(codec_, p.data);
        auto& s = samples.emplace_back();
        bitstream::appendSample(codec_, p.data, s.bytes);
        s.pts = p.pts100ns;
        s.keyframe = p.keyframe;
        bytes += p.data.size();
        keyframes += p.keyframe;
    }

    struct Sample {
        std::vector<uint8_t> bytes;
        int64_t pts = 0;
        bool keyframe = false;
    };
    std::vector<Sample> samples;
    std::optional<bitstream::VideoStreamInfo> config;
    uint64_t bytes = 0;
    uint64_t keyframes = 0;
    double latencyTotalMs = 0, latencyMaxMs = 0;
    uint64_t latencySamples = 0;
    Section section{"  of which sink"};

private:
    Codec codec_;
    std::map<int64_t, int64_t> submitTime_;
};

// Accumulates wall time of a code section on the pump thread.
const char* codecName(Codec c) { return c == Codec::H264 ? "H.264" : c == Codec::HEVC ? "HEVC" : "AV1"; }

// Runs the production pipeline (CaptureSession -> ReplayBuffer -> ClipSaver) headlessly.
int runSessionBench(const BenchOptions& o, HMONITOR monitor) {
    constexpr UINT kSaved = WM_APP + 10;
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);

    SessionConfig cfg;
    cfg.target.monitor = monitor;
    cfg.name = o.synthetic.width ? "Synthetic Test" : "Desktop";
    cfg.synthetic = o.synthetic.width != 0;
    cfg.processId = o.pid;
    cfg.settings.video.codec = o.codec.value_or(Codec::HEVC);
    cfg.settings.video.tenBit = o.tenBit;
    cfg.settings.video.fps = o.fps;
    cfg.settings.video.outputHeight = o.height;
    cfg.settings.video.bitrateKbps = o.bitrateKbps;
    cfg.settings.video.maxBitrateKbps = o.bitrateKbps * 3 / 2;
    cfg.settings.video.preset = o.preset;
    cfg.settings.capture.captureCursor = o.cursor;
    if (o.rgbInput) cfg.settings.capture.convertPath = ConvertPath::EncoderNative;
    if (o.forceVideoProcessor) cfg.settings.capture.convertPath = ConvertPath::VideoProcessor;
    cfg.settings.replay.seconds = o.replaySeconds;
    if (!o.outDir.empty()) cfg.settings.output.folder = narrow(o.outDir);

    CaptureSession session(cfg);
    const uint64_t cpu0 = processCycles();
    const double hz = cyclesPerSecond();
    const int64_t t0 = clock::now100ns();
    if (!session.start()) {
        std::println("error: session failed to start (see log)");
        return 1;
    }
    std::optional<SaveResult> recording;
    if (o.recordSeconds > 0) {
        // Record in the middle of the session: start after 1 s, stop after recordSeconds.
        Recorder recorder(window, kSaved);
        Sleep(1000);
        session.startRecording(recorder, 0);
        Sleep(static_cast<DWORD>(o.recordSeconds * 1000));
        recorder.stop(clock::now100ns());
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            if (m.message == kSaved) {
                std::unique_ptr<SaveResult> r(reinterpret_cast<SaveResult*>(m.lParam));
                recording = *r;
                break;
            }
            DispatchMessageW(&m);
        }
    }
    const double remaining = o.seconds - clock::toSeconds(clock::now100ns() - t0);
    if (remaining > 0) Sleep(static_cast<DWORD>(remaining * 1000));
    const double wall = clock::toSeconds(clock::now100ns() - t0);
    const double cpu = (processCycles() - cpu0) / hz;

    ClipSaver saver(window, kSaved);
    const int64_t saveStart = clock::now100ns();
    session.saveReplay(saver, o.replaySeconds);
    MSG msg;
    SaveResult result;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kSaved) {
            std::unique_ptr<SaveResult> r(reinterpret_cast<SaveResult*>(msg.lParam));
            result = *r;
            break;
        }
        DispatchMessageW(&msg);
    }
    const double saveMs = clock::toSeconds(clock::now100ns() - saveStart) * 1000;
    const auto st = session.stats();
    session.stop();
    DestroyWindow(window);

    std::println("--- session {:.1f} s ---", wall);
    std::println("output:               {}x{} via {}", st.outputSize.width, st.outputSize.height,
                 st.zeroCopy ? "zero-copy RGB" : "video processor");
    std::println("captured/encoded:     {} / {} ({} duplicates, {} dropped busy, {} errors)", st.framesCaptured,
                 st.framesEncoded, st.duplicates, st.droppedBusy, st.encodeErrors);
    std::println("replay buffer:        {:.1f} / {:.1f} MB", st.buffer.usedBytes / 1048576.0,
                 st.buffer.capacityBytes / 1048576.0);
    std::println("CPU (whole process):  {:.2f}% of one core", 100 * cpu / wall);
    if (recording) {
        if (recording->ok) {
            std::println("recording:            {:.2f} s, {:.1f} MB{}{}", recording->seconds,
                         recording->bytes / 1048576.0, recording->error.empty() ? "" : " - ", recording->error);
            std::println("recording file:       {}", narrow(recording->path.wstring()));
        } else {
            std::println("recording FAILED:     {}", recording->error);
        }
    }
    if (!result.ok) {
        std::println("save FAILED: {}", result.error);
        return 1;
    }
    std::println("saved:                {:.2f} s clip, {:.1f} MB in {:.0f} ms", result.seconds, result.bytes / 1048576.0, saveMs);
    std::println("file:                 {}", narrow(result.path.wstring()));
    return 0;
}

} // namespace

int runBench(const std::vector<std::wstring>& args) {
    BenchOptions o;
    if (!parse(args, o)) {
        usage();
        return 2;
    }
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    qos::setThreadName(L"bench");
    qos::setHighQos();
    qos::MmcssScope mmcss(L"Capture");

    const HMONITOR monitor = monitorByIndex(o.monitorIndex);
    if (o.session) return runSessionBench(o, monitor);
    auto gpu = createGpuDevice({.monitor = monitor, .highGpuPriority = true});
    if (!gpu) {
        std::println("error: {}", gpu.error());
        return 1;
    }
    std::println("GPU: {} ({})", gpu->adapterName, gpuVendorName(gpu->vendor));

    std::unique_ptr<ICaptureSource> source;
    if (o.synthetic.width) source = std::make_unique<FakeCapture>(*gpu, o.synthetic.width, o.synthetic.height);
    else source = std::make_unique<WgcCapture>(*gpu, CaptureTarget{.monitor = monitor}, CaptureOptions{.fps = o.fps, .cursor = o.cursor});
    ICaptureSource& capture = *source;
    if (!capture.start()) {
        std::println("error: capture failed to start (see log)");
        return 1;
    }

    const DXGI_FORMAT yuvFormat = o.tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    VideoProcessorConverter converter;
    std::array<winrt::com_ptr<ID3D11Texture2D>, 4> outputs; // convert-only mode targets
    size_t outIndex = 0;
    Size outSize{};
    GpuTimer gpuTimer;
    gpuTimer.init(gpu->device.get(), gpu->context.get());

    std::unique_ptr<IVideoEncoder> encoder;
    std::unique_ptr<CollectingSink> sink;
    std::optional<InputSurface> lastSurface;
    uint64_t converted = 0, duplicates = 0, encoderBusy = 0, encodeErrors = 0;
    bool dumped = o.dumpFrame.empty() || o.codec.has_value();

    Section secAcquire{"acquire"}, secConvert{"convert"}, secEncode{"encode submit"}, secDrain{"drain + mux prep"};
    FrameTimer timer(o.fps);
    const double hz = cyclesPerSecond();
    uint64_t cpu0 = processCycles();
    uint64_t threadCpu0 = threadCycles();
    int64_t t0 = clock::now100ns();
    const int64_t end = t0 + clock::secondsTo100ns(o.seconds);

    while (timer.waitNext() && clock::now100ns() < end) {
        const int64_t pts = timer.tickTime100ns();
        std::optional<CapturedFrame> frame;
        {
            SectionScope _(secAcquire);
            frame = capture.acquireLatest();
        }
        if (!frame) {
            ++duplicates;
            if (encoder && lastSurface) {
                sink->submitted(pts);
                if (!encoder->encode(*lastSurface, pts, kFrameDuplicate)) ++encodeErrors;
            }
        } else {
            D3D11_TEXTURE2D_DESC td{};
            frame->texture->GetDesc(&td);
            const Size inSize{td.Width, td.Height};
            if (converter.inputSize() != inSize) {
                outSize = scaledOutputSize({frame->width, frame->height}, o.height);
                if (!converter.init(gpu->device.get(), gpu->context.get(), inSize, outSize, yuvFormat, o.fps)) return 1;
                std::println("capture {}x{} -> output {}x{} {}", frame->width, frame->height, outSize.width,
                             outSize.height, o.tenBit ? "P010" : "NV12");
                if (o.codec && !encoder) {
                    if (!AmfEncoder::runtimeAvailable()) {
                        std::println("error: no AMF runtime (only the AMD encoder is implemented so far)");
                        return 1;
                    }
                    EncoderConfig ec{.codec = *o.codec, .tenBit = o.tenBit, .width = outSize.width,
                                     .height = outSize.height, .fpsNum = o.fps, .fpsDen = 1,
                                     .bitrateKbps = o.bitrateKbps, .maxBitrateKbps = o.bitrateKbps * 3 / 2,
                                     .gopFrames = o.fps * 2, .preset = o.preset, .rgbInput = o.rgbInput};
                    if (o.rgbInput && outSize != Size{frame->width, frame->height}) {
                        std::println("error: --rgb-input needs output size == capture size");
                        return 1;
                    }
                    encoder = std::make_unique<AmfEncoder>();
                    if (!encoder->init(*gpu, ec)) {
                        std::println("error: encoder init failed (see log)");
                        return 1;
                    }
                    sink = std::make_unique<CollectingSink>(*o.codec);
                    std::println("encoding {} {}-bit with {} at {} kbps", codecName(*o.codec), o.tenBit ? 10 : 8,
                                 encoder->name(), o.bitrateKbps);
                    // Start the stats after one-time setup.
                    cpu0 = processCycles();
                    threadCpu0 = threadCycles();
                    t0 = clock::now100ns();
                } else if (!o.codec) {
                    for (auto& t : outputs) t = createYuvTexture(gpu->device.get(), outSize, yuvFormat);
                }
            }

            ID3D11Texture2D* target = nullptr;
            std::optional<InputSurface> surface;
            if (encoder) {
                surface = encoder->acquireSurface();
                if (!surface) ++encoderBusy; // all surfaces in flight: drop this frame
                else target = surface->texture;
            } else {
                target = outputs[outIndex++ % outputs.size()].get();
            }
            if (target) {
                bool ok = false;
                {
                    SectionScope _(secConvert);
                    gpuTimer.begin();
                    if (o.rgbInput) {
                        const D3D11_BOX box{0, 0, 0, frame->width, frame->height, 1};
                        gpu->context->CopySubresourceRegion(target, 0, 0, 0, 0, frame->texture.get(), 0, &box);
                        ok = true;
                    } else {
                        ok = converter.convert(frame->texture.get(), {frame->width, frame->height}, target);
                    }
                    gpuTimer.end();
                }
                if (!ok) {
                    std::println("error: conversion failed");
                    return 1;
                }
                ++converted;
                if (encoder) {
                    SectionScope _(secEncode);
                    sink->submitted(pts);
                    if (!encoder->encode(*surface, pts, lastSurface ? kFrameNone : kFrameForceKeyframe)) ++encodeErrors;
                    lastSurface = surface;
                }
            }
            frame.reset(); // GPU work is queued; hand the capture surface back to the pool
            if (!dumped && converted >= static_cast<uint64_t>(o.fps)) {
                dumped = dumpYuv(*gpu, target, outSize, o.tenBit, o.dumpFrame);
                std::println("dumped frame to {} ({}x{} {})", narrow(o.dumpFrame), outSize.width, outSize.height,
                             o.tenBit ? "p010le" : "nv12");
            }
        }
        if (encoder) {
            SectionScope _(secDrain);
            encoder->drain(*sink);
        }
        gpuTimer.collect();
    }

    if (encoder) encoder->flush(*sink);
    gpu->context->Flush();
    const double wall = clock::toSeconds(clock::now100ns() - t0);
    const double cpu = (processCycles() - cpu0) / hz;
    const double pumpCpu = (threadCycles() - threadCpu0) / hz;
    PROCESS_MEMORY_COUNTERS_EX pmc{sizeof(pmc)};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    capture.stop();

    std::println("--- {:.1f} s at {} fps target ---", wall, o.fps);
    std::println("frames from {}:{:<{}}{} ({:.1f}/s)", capture.name(), "", 10 - std::strlen(capture.name()),
                 capture.framesArrived(), capture.framesArrived() / wall);
    std::println("converted:            {} ({:.1f}/s)", converted, converted / wall);
    std::println("ticks w/o new frame:  {}{}", duplicates, encoder ? " (encoded as duplicates)" : "");
    std::println("timer ticks skipped:  {}", timer.skippedTicks());
    std::println("GPU convert time:     avg {:.3f} ms, max {:.3f} ms ({} samples)", gpuTimer.averageMs(),
                 gpuTimer.maxMs(), gpuTimer.samples());
    if (encoder) {
        std::println("encoded packets:      {} ({} keyframes), {} dropped (encoder busy), {} errors",
                     sink->samples.size(), sink->keyframes, encoderBusy, encodeErrors);
        std::println("encode latency:       avg {:.2f} ms, max {:.2f} ms", sink->latencySamples ? sink->latencyTotalMs / sink->latencySamples : 0.0,
                     sink->latencyMaxMs);
        std::println("bitrate:              {:.1f} Mbps ({:.1f} MB)", sink->bytes * 8 / wall / 1e6, sink->bytes / 1048576.0);
    }
    std::println("CPU:                  {:.2f}% of one core, {:.3f}% of all {} threads", 100 * cpu / wall,
                 100 * cpu / wall / si.dwNumberOfProcessors, si.dwNumberOfProcessors);
    std::println("  pump thread:        {:.2f}% of one core ({:.3f} ms per tick)", 100 * pumpCpu / wall,
                 1000 * pumpCpu / std::max<uint64_t>(1, timer.tickIndex()));
    std::println("  other threads:      {:.2f}% of one core (encoder runtime, driver, capture callbacks)",
                 100 * (cpu - pumpCpu) / wall);
    for (const Section* sec : {&secAcquire, &secConvert, &secEncode, &secDrain, sink ? &sink->section : nullptr})
        if (sec)
        if (sec && sec->calls)
            std::println("  {:<18}  wall {:7.1f} us, CPU {:7.1f} us per call ({} calls)", sec->name,
                         sec->wall / 10.0 / sec->calls, sec->cycles / hz * 1e6 / sec->calls, sec->calls);
    std::println("working set:          {:.1f} MB (private {:.1f} MB)", pmc.WorkingSetSize / 1048576.0,
                 pmc.PrivateUsage / 1048576.0);

    if (encoder && !o.out.empty()) {
        if (sink->samples.empty() || !sink->config) {
            std::println("error: no decodable output (missing keyframe or parameter sets)");
            return 1;
        }
        Mp4ClipInput clip;
        // The bitstream's own size: AMD RDNA3 AV1 can only code 1080p as 1082 lines.
        clip.video = {*o.codec, sink->config->width, sink->config->height, o.fps, 1, sink->config->configRecord};
        for (const auto& s : sink->samples) clip.videoSamples.push_back({s.bytes, s.pts, s.keyframe});
        clip.title = "Recordion bench";
        clip.creationUnixTime = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count();
        OfstreamSink file(o.out);
        if (!writeMp4Clip(clip, file) || !file.ok()) {
            std::println("error: writing {} failed", narrow(o.out));
            return 1;
        }
        std::println("wrote {} ({:.1f} MB)", narrow(o.out), file.position() / 1048576.0);
    }
    return 0;
}

} // namespace rec
