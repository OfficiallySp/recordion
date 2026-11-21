#include "app/Bench.h"

#include "capture/FakeCapture.h"
#include "capture/WgcCapture.h"
#include "core/Clock.h"
#include "core/FrameTimer.h"
#include "core/Log.h"
#include "core/ThreadQos.h"
#include "core/Utf.h"
#include "app/SettingsWindow.h"
#include "detect/GameLibrary.h"
#include "encode/AmfEncoder.h"

#include <AMF/components/DisplayCapture.h>
#include <AMF/core/Factory.h>
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
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <thread>

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
    bool constantFrameRate = false; // session mode: encode repeats when nothing changed
    bool forceVideoProcessor = false;
    bool captureOnly = false; // capture and discard (compare capture APIs)
    bool amdCapture = false;
    uint32_t replaySeconds = 5;
    std::wstring outDir;
    DWORD pid = 0; // session mode: capture only this process's audio
    double recordSeconds = 0; // session mode: also make a manual recording this long
    bool noAudio = false;     // session mode: video only
    bool micOnly = false;     // session mode: the default microphone, no system audio
    std::wstring profileThread; // session mode: sample this thread's instruction pointer
    // Encoder tuning (encode mode).
    RateControl rateControl = RateControl::VBR;
    uint32_t peakKbps = 0; // 0 = 1.5x the bitrate
    uint32_t cqLevel = 23;
    uint32_t gopSeconds = 2;
    // Quality testing: grab real frames once, then encode exactly those frames with every setting.
    std::wstring grabRaw;   // write this many captured frames, uncompressed BGRA, to this file
    uint32_t frames = 0;
    std::wstring sourceRaw; // read frames from such a file instead of capturing
    Size sourceSize{};
    uint32_t every = 1;     // a new raw frame only every Nth tick, like a game running below the frame rate
    bool skipUnchanged = false; // ticks without a new frame aren't encoded (as CaptureSession does by default)
};

void usage() {
    std::println("usage: recordion --bench [--seconds N] [--fps N] [--height N] [--10bit] [--no-cursor] [--monitor N]\n"
                 "                         [--codec h264|hevc|av1 [--bitrate KBPS] [--preset speed|balanced|quality]\n"
                 "                          [--rc vbr|cbr|cqp] [--peak KBPS] [--cq N] [--gop SECONDS] [--out file.mp4]]\n"
                 "                         [--dump-frame file.yuv]\n"
                 "                         [--source-raw file.bgra --source-size WxH]  (encode grabbed frames)\n"
                 "       recordion --bench --grab-raw file.bgra --frames N [--fps N] [--monitor N]\n"
                 "       recordion --bench --session [--synthetic] [--mic | --no-audio] [--profile-thread NAME]\n"
                 "                         [--replay N] [--record N] [--out-dir DIR]  (the real pipeline; CPU by thread)");
}

// Replays frames saved by --grab-raw (tightly packed BGRA, no header) once, at the bench's pace, so
// every encoder setting sees exactly the same real game frames.
class RawFileSource final : public ICaptureSource {
public:
    RawFileSource(const GpuDevice& gpu, std::wstring path, Size size, uint32_t every)
        : device_(gpu.device), context_(gpu.context), path_(std::move(path)), size_(size), every_(every) {}
    ~RawFileSource() override { stop(); }

    const char* name() const override { return "raw file"; }
    bool start() override {
        file_ = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                            nullptr);
        LARGE_INTEGER bytes{};
        frameBytes_ = size_t{size_.width} * size_.height * 4;
        if (file_ == INVALID_HANDLE_VALUE || frameBytes_ == 0 || !GetFileSizeEx(file_, &bytes)) return false;
        count_ = static_cast<uint64_t>(bytes.QuadPart) / frameBytes_;
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        data_ = mapping_ ? static_cast<const uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0)) : nullptr;
        if (!data_ || count_ == 0) return false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = size_.width;
        td.Height = size_.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (auto& t : textures_)
            if (FAILED(device_->CreateTexture2D(&td, nullptr, t.put()))) return false;
        log::info("raw source: {} frames of {}x{}", count_, size_.width, size_.height);
        return true;
    }
    void stop() override {
        if (data_) UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        data_ = nullptr;
        mapping_ = nullptr;
        file_ = INVALID_HANDLE_VALUE;
    }
    std::optional<CapturedFrame> acquireLatest() override {
        if (next_ >= count_ || calls_++ % every_ != 0) return std::nullopt;
        auto& texture = textures_[next_ % textures_.size()];
        context_->UpdateSubresource(texture.get(), 0, nullptr, data_ + next_ * frameBytes_, size_.width * 4, 0);
        CapturedFrame f;
        f.texture = texture;
        f.width = size_.width;
        f.height = size_.height;
        f.time100ns = clock::now100ns();
        f.sequence = ++next_;
        return f;
    }
    bool targetLost() const override { return next_ >= count_; }
    uint64_t framesArrived() const override { return next_; }

private:
    winrt::com_ptr<ID3D11Device5> device_;
    winrt::com_ptr<ID3D11DeviceContext4> context_;
    std::wstring path_;
    Size size_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    const uint8_t* data_ = nullptr;
    size_t frameBytes_ = 0;
    uint64_t count_ = 0;
    uint64_t next_ = 0;
    uint32_t every_ = 1;
    uint64_t calls_ = 0;
    std::array<winrt::com_ptr<ID3D11Texture2D>, 8> textures_; // deep enough for frames still in the encoder
};

// Saves `frames` consecutive new frames from the screen, uncompressed, for --source-raw. They are
// kept in RAM while grabbing (a disk can't take 500 MB/s steadily) and written at the end.
int runGrab(const BenchOptions& o, HMONITOR monitor) {
    auto gpu = createGpuDevice({.monitor = monitor, .highGpuPriority = true});
    if (!gpu || o.frames == 0) return 1;
    WgcCapture capture(*gpu, CaptureTarget{.monitor = monitor}, CaptureOptions{.fps = o.fps, .cursor = o.cursor});
    if (!capture.start()) return 1;
    winrt::com_ptr<ID3D11Texture2D> staging;
    std::vector<uint8_t> data;
    Size size{};
    uint64_t grabbed = 0, idleTicks = 0;
    FrameTimer timer(o.fps);
    while (grabbed < o.frames && timer.waitNext()) {
        auto frame = capture.acquireLatest();
        if (!frame) {
            ++idleTicks; // nothing new on screen: not part of the sequence
            continue;
        }
        if (!staging) {
            size = {frame->width, frame->height};
            D3D11_TEXTURE2D_DESC d{};
            frame->texture->GetDesc(&d);
            d.Width = size.width;
            d.Height = size.height;
            d.Usage = D3D11_USAGE_STAGING;
            d.BindFlags = 0;
            d.MiscFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(gpu->device->CreateTexture2D(&d, nullptr, staging.put()))) return 1;
            data.resize(size_t{o.frames} * size.width * size.height * 4);
        }
        const D3D11_BOX box{0, 0, 0, size.width, size.height, 1};
        gpu->context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, frame->texture.get(), 0, &box);
        frame.reset();
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(gpu->context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) return 1;
        uint8_t* out = data.data() + grabbed * size.width * size.height * 4;
        for (uint32_t y = 0; y < size.height; ++y)
            std::memcpy(out + size_t{y} * size.width * 4, static_cast<const uint8_t*>(m.pData) + size_t{y} * m.RowPitch,
                        size_t{size.width} * 4);
        gpu->context->Unmap(staging.get(), 0);
        ++grabbed;
    }
    capture.stop();
    std::ofstream file(o.grabRaw, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(grabbed * size.width * size.height * 4));
    std::println("grabbed {} frames of {}x{} ({} ticks with nothing new, {} timer ticks skipped) to {}", grabbed,
                 size.width, size.height, idleTicks, timer.skippedTicks(), narrow(o.grabRaw));
    return file ? 0 : 1;
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
        if (a == L"--cfr") { o.constantFrameRate = true; continue; }
        if (a == L"--skip-unchanged") { o.skipUnchanged = true; continue; }
        if (a == L"--video-processor") { o.forceVideoProcessor = true; continue; }
        if (a == L"--capture-only") { o.captureOnly = true; continue; }
        if (a == L"--no-audio") { o.noAudio = true; continue; }
        if (a == L"--mic") { o.micOnly = true; continue; }
        if (a == L"--amd-capture") { o.captureOnly = o.amdCapture = true; continue; }
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
        else if (a == L"--profile-thread") o.profileThread = *v;
        else if (a == L"--pid") o.pid = static_cast<DWORD>(std::stoul(*v));
        else if (a == L"--record") o.recordSeconds = std::stod(*v);
        else if (a == L"--peak") o.peakKbps = static_cast<uint32_t>(std::stoul(*v));
        else if (a == L"--cq") o.cqLevel = static_cast<uint32_t>(std::stoul(*v));
        else if (a == L"--gop") o.gopSeconds = static_cast<uint32_t>(std::stoul(*v));
        else if (a == L"--grab-raw") o.grabRaw = *v;
        else if (a == L"--frames") o.frames = static_cast<uint32_t>(std::stoul(*v));
        else if (a == L"--source-raw") o.sourceRaw = *v;
        else if (a == L"--every") o.every = std::max(1u, static_cast<uint32_t>(std::stoul(*v)));
        else if (a == L"--source-size") {
            const auto x = v->find(L'x');
            if (x == std::wstring::npos) return false;
            o.sourceSize = {static_cast<uint32_t>(std::stoul(v->substr(0, x))), static_cast<uint32_t>(std::stoul(v->substr(x + 1)))};
        }
        else if (a == L"--rc") {
            if (*v == L"vbr") o.rateControl = RateControl::VBR;
            else if (*v == L"cbr") o.rateControl = RateControl::CBR;
            else if (*v == L"cqp") o.rateControl = RateControl::CQP;
            else return false;
        }
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

// CPU cycles of every thread in this process, by thread id, with its description.
struct ThreadCpu {
    std::wstring name;
    uint64_t cycles = 0;
};
std::map<DWORD, ThreadCpu> threadCpuSnapshot() {
    std::map<DWORD, ThreadCpu> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te{sizeof(te)};
    const DWORD self = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != self) continue;
        HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        ThreadCpu& tc = out[te.th32ThreadID];
        ULONG64 c = 0;
        QueryThreadCycleTime(t, &c);
        tc.cycles = c;
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(t, &desc)) && desc) {
            tc.name = desc;
            LocalFree(desc);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return out;
}

// Prints each thread's share of one core between two snapshots, busiest first, grouped by name.
void printThreadCpu(const std::map<DWORD, ThreadCpu>& before, const std::map<DWORD, ThreadCpu>& after, double hz,
                    double wall) {
    std::map<std::wstring, double> byName;
    for (const auto& [tid, tc] : after) {
        const auto it = before.find(tid);
        const uint64_t c = tc.cycles - (it != before.end() ? it->second.cycles : 0);
        byName[tc.name.empty() ? L"(unnamed: runtime/driver)" : tc.name] += 100.0 * c / hz / wall;
    }
    std::vector<std::pair<double, std::wstring>> rows;
    for (const auto& [name, pct] : byName) rows.emplace_back(pct, name);
    std::ranges::sort(rows, std::greater{});
    for (const auto& [pct, name] : rows)
        if (pct >= 0.005) std::println("  {:<26} {:.2f}% of one core", narrow(name), pct);
}

// Statistical profiler for one named thread of this process: suspends it at random 0.2-0.8 ms
// intervals, records its instruction pointer, and maps the addresses to module!export afterwards
// (samples sitting in a wait syscall count as blocked). Spins one core while it runs; development
// only (`--profile-thread NAME`).
class ThreadSampler {
public:
    explicit ThreadSampler(std::wstring name) : name_(std::move(name)) {}
    ~ThreadSampler() { stop(); }
    void start() {
        stop_ = false;
        thread_ = std::thread([this] { run(); });
    }
    void stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    void print() const {
        std::map<std::string, uint64_t> byModule, bySymbol;
        size_t blocked = 0;
        for (size_t i = 0; i < count_; ++i) {
            auto [module, symbol] = describe(rips_[i]);
            if (module == "ntdll.dll" && (symbol.starts_with("NtWaitFor") || symbol.starts_with("NtDelayExecution") ||
                                          symbol.starts_with("NtRemoveIoCompletion") || symbol.starts_with("NtWaitForAlert"))) {
                ++blocked; // asleep in a wait, not using the CPU
                continue;
            }
            ++byModule[module];
            ++bySymbol[module + "!" + symbol];
        }
        const size_t running = count_ - blocked;
        std::println("profile of \"{}\": {} samples, {} blocked in a wait, {} running", narrow(name_), count_, blocked,
                     running);
        auto top = [&](const std::map<std::string, uint64_t>& m, size_t n) {
            std::vector<std::pair<uint64_t, std::string>> v;
            for (const auto& [k, c] : m) v.emplace_back(c, k);
            std::ranges::sort(v, std::greater{});
            for (size_t i = 0; i < std::min(n, v.size()); ++i)
                std::println("  {:5.1f}%  {}", 100.0 * v[i].first / std::max<size_t>(1, running), v[i].second);
        };
        std::println(" by module:");
        top(byModule, 12);
        std::println(" by function (nearest export):");
        top(bySymbol, 30);
    }

private:
    static std::pair<std::string, std::string> describe(uint64_t rip) {
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(rip), &m))
            return {"?", std::format("{:x}", rip)};
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(m, path, MAX_PATH);
        std::string module = narrow(std::filesystem::path(path).filename().wstring());
        const auto base = reinterpret_cast<const uint8_t*>(m);
        const uint64_t rva = rip - reinterpret_cast<uint64_t>(m);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        std::string best = std::format("+{:x}", rva);
        if (dir.VirtualAddress && dir.Size) {
            auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
            auto* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
            auto* ordinals = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);
            auto* functions = reinterpret_cast<const DWORD*>(base + exp->AddressOfFunctions);
            uint64_t bestRva = 0;
            for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
                const DWORD f = functions[ordinals[i]];
                if (f <= rva && f > bestRva) {
                    bestRva = f;
                    best = std::format("{}+{:x}", reinterpret_cast<const char*>(base + names[i]), rva - f);
                }
            }
            // Far from any export: an internal function; group by 4 KB page instead.
            if (rva - bestRva > 0x2000) best = std::format("+{:x}xxx", rva >> 12);
        }
        return {module, best};
    }
    void run() {
        HANDLE target = nullptr;
        for (int tries = 0; !target && tries < 200 && !stop_; ++tries) {
            for (const auto& [tid, tc] : threadCpuSnapshot())
                if (tc.name == name_)
                    target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
            if (!target) Sleep(10);
        }
        if (!target) return;
        qos::setThreadName(L"profiler");
        // Spin for a random 0.2-0.8 ms between samples: a timer would wake together with the
        // target's own timer and only ever catch it waking up.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        uint64_t rng = 0x9E3779B97F4A7C15ull ^ __rdtsc();
        while (!stop_ && count_ < rips_.size()) {
            rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
            const int64_t until = clock::now100ns() + 2000 + static_cast<int64_t>(rng % 6000);
            while (clock::now100ns() < until) YieldProcessor();
            if (SuspendThread(target) == static_cast<DWORD>(-1)) break;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            const bool got = GetThreadContext(target, &ctx);
            ResumeThread(target);
            ++taken_;
            if (got) rips_[count_++] = ctx.Rip;
        }
        CloseHandle(target);
    }

    std::wstring name_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::array<uint64_t, 200000> rips_{};
    size_t count_ = 0;
    size_t taken_ = 0;
};

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

// Spike: capture with AMF's DisplayCapture component (AMD Direct Capture in the driver runtime)
// or with WGC, doing nothing else with the frames, to compare what capture alone costs.
int runCaptureOnly(const BenchOptions& o, HMONITOR monitor, bool amd) {
    auto gpu = createGpuDevice({.monitor = monitor, .highGpuPriority = true});
    if (!gpu) return 1;
    const double hz = cyclesPerSecond();
    uint64_t frames = 0;
    if (!amd) {
        WgcCapture capture(*gpu, CaptureTarget{.monitor = monitor}, CaptureOptions{.fps = o.fps, .cursor = o.cursor});
        if (!capture.start()) return 1;
        FrameTimer timer(o.fps);
        const uint64_t c0 = processCycles();
        const int64_t t0 = clock::now100ns(), end = t0 + clock::secondsTo100ns(o.seconds);
        while (timer.waitNext() && clock::now100ns() < end)
            if (capture.acquireLatest()) ++frames;
        const double wall = clock::toSeconds(clock::now100ns() - t0);
        std::println("WGC: {} frames ({:.1f}/s), CPU {:.2f}% of one core", frames, frames / wall,
                     100.0 * (processCycles() - c0) / hz / wall);
        return 0;
    }
    HMODULE dll = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto init = dll ? reinterpret_cast<AMFInit_Fn>(GetProcAddress(dll, AMF_INIT_FUNCTION_NAME)) : nullptr;
    amf::AMFFactory* factory = nullptr;
    if (!init || init(AMF_FULL_VERSION, &factory) != AMF_OK) {
        std::println("error: no AMF runtime");
        return 1;
    }
    amf::AMFContextPtr context;
    amf::AMFComponentPtr capture;
    factory->CreateContext(&context);
    context->InitDX11(gpu->device.get());
    AMF_RESULT r = factory->CreateComponent(context, AMFDisplayCapture, &capture);
    if (r != AMF_OK) {
        std::println("error: AMF DisplayCapture unavailable ({})", static_cast<int>(r));
        return 1;
    }
    capture->SetProperty(AMF_DISPLAYCAPTURE_MONITOR_INDEX, amf_int64{o.monitorIndex});
    // Non-blocking: hand back the current surface; our frame timer does the pacing.
    capture->SetProperty(AMF_DISPLAYCAPTURE_MODE, amf_int64{AMF_DISPLAYCAPTURE_MODE_GET_CURRENT_SURFACE});
    capture->SetProperty(AMF_DISPLAYCAPTURE_FRAMERATE, AMFConstructRate(o.fps, 1));
    r = capture->Init(amf::AMF_SURFACE_UNKNOWN, 0, 0);
    if (r != AMF_OK) {
        std::println("error: DisplayCapture Init failed ({})", static_cast<int>(r));
        return 1;
    }
    amf_int64 format = 0;
    capture->GetProperty(AMF_DISPLAYCAPTURE_FORMAT, &format);
    // Paced like the WGC run: one pass per frame interval, taking whatever is ready.
    FrameTimer timer(o.fps);
    const uint64_t c0 = processCycles();
    const int64_t t0 = clock::now100ns(), end = t0 + clock::secondsTo100ns(o.seconds);
    while (timer.waitNext() && clock::now100ns() < end) {
        amf::AMFDataPtr data;
        capture->QueryOutput(&data);
        if (data) ++frames;
    }
    const double wall = clock::toSeconds(clock::now100ns() - t0);
    std::println("AMD Direct Capture: {} frames ({:.1f}/s), CPU {:.2f}% of one core, surface format {}", frames,
                 frames / wall, 100.0 * (processCycles() - c0) / hz / wall, format);
    capture->Terminate();
    context->Terminate();
    return 0;
}

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
    cfg.settings.video.constantFrameRate = o.constantFrameRate;
    cfg.settings.capture.captureCursor = o.cursor;
    if (o.rgbInput) cfg.settings.capture.convertPath = ConvertPath::EncoderNative;
    if (o.forceVideoProcessor) cfg.settings.capture.convertPath = ConvertPath::VideoProcessor;
    if (o.amdCapture) cfg.settings.capture.method = CaptureMethod::AmdDirect;
    cfg.settings.replay.seconds = o.replaySeconds;
    if (!o.outDir.empty()) cfg.settings.output.folder = narrow(o.outDir);
    if (o.noAudio || o.micOnly) cfg.settings.audio.captureSystem = false;
    if (o.micOnly) cfg.settings.audio.captureMic = true;

    CaptureSession session(cfg);
    const double hz = cyclesPerSecond();
    const int64_t t0 = clock::now100ns();
    if (!session.start()) {
        std::println("error: session failed to start (see log)");
        return 1;
    }
    // Steady state only: thread start-up and encoder init are left out.
    Sleep(1000);
    const uint64_t cpu0 = processCycles();
    const int64_t steady0 = clock::now100ns();
    const auto threads0 = threadCpuSnapshot();
    std::unique_ptr<ThreadSampler> sampler;
    if (!o.profileThread.empty()) {
        sampler = std::make_unique<ThreadSampler>(o.profileThread);
        sampler->start();
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
    const double steadyWall = clock::toSeconds(clock::now100ns() - steady0);
    const double cpu = (processCycles() - cpu0) / hz;
    const auto threads1 = threadCpuSnapshot();
    if (sampler) sampler->stop();

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
    std::println("captured/encoded:     {} / {} ({} unchanged skipped, {} duplicates, {} dropped busy, {} errors)",
                 st.framesCaptured, st.framesEncoded, st.unchanged, st.duplicates, st.droppedBusy, st.encodeErrors);
    std::println("replay buffer:        {:.1f} MB held, {:.1f} MB of RAM committed, {:.1f} MB reserved",
                 st.buffer.usedBytes / 1048576.0, st.buffer.committedBytes / 1048576.0,
                 st.buffer.capacityBytes / 1048576.0);
    PROCESS_MEMORY_COUNTERS_EX pmc{sizeof(pmc)};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    std::println("process memory:       {:.1f} MB working set, {:.1f} MB private", pmc.WorkingSetSize / 1048576.0,
                 pmc.PrivateUsage / 1048576.0);
    std::println("CPU (whole process):  {:.2f}% of one core (steady state, last {:.1f} s)", 100 * cpu / steadyWall,
                 steadyWall);
    printThreadCpu(threads0, threads1, hz, steadyWall);
    if (sampler) sampler->print();
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
    if (args.size() >= 3 && args[1] == L"--settings-screenshots") {
        // Renders every settings page to PNGs, to check the layout without a display.
        struct Host final : SettingsHost {
            Settings s;
            const Settings& currentSettings() const override { return s; }
            std::string applySettings(const Settings&) override { return {}; }
            void suspendHotkeys(bool) override {}
            std::vector<std::string> statusLines() const override {
                return {"Game: Haste", "Capturing \"Haste\" at 1920x1080, 60 fps, HEVC", "Path: zero-copy"};
            }
            std::vector<std::string> availableEncoders() const override { return {"AMD (AMF)"}; }
        } host;
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        SettingsWindow window(host);
        const bool ok = window.screenshotPages(GetModuleHandleW(nullptr), args[2]);
        std::println("{}", ok ? "wrote settings-*.png" : "failed");
        return ok ? 0 : 1;
    }
    if (args.size() >= 2 && args[1] == L"--games") {
        GameLibrary lib;
        lib.build();
        std::println("installed titles: {}, library folders: {}, known to Windows: {}", lib.installCount(),
                     lib.rootCount(), lib.knownExeCount());
        for (const auto& name : lib.installNames()) std::println("  {}", name);
        return 0;
    }
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
    if (!o.grabRaw.empty()) return runGrab(o, monitor);
    if (o.session) return runSessionBench(o, monitor);
    if (o.captureOnly) return runCaptureOnly(o, monitor, o.amdCapture);
    auto gpu = createGpuDevice({.monitor = monitor, .highGpuPriority = true});
    if (!gpu) {
        std::println("error: {}", gpu.error());
        return 1;
    }
    std::println("GPU: {} ({})", gpu->adapterName, gpuVendorName(gpu->vendor));

    std::unique_ptr<ICaptureSource> source;
    if (!o.sourceRaw.empty()) source = std::make_unique<RawFileSource>(*gpu, o.sourceRaw, o.sourceSize, o.every);
    else if (o.synthetic.width) source = std::make_unique<FakeCapture>(*gpu, o.synthetic.width, o.synthetic.height);
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
        if (!frame && capture.targetLost()) break; // a raw file has run out
        if (!frame) {
            ++duplicates;
            if (encoder && lastSurface && !o.skipUnchanged) {
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
                                     .rateControl = o.rateControl, .bitrateKbps = o.bitrateKbps,
                                     .maxBitrateKbps = o.peakKbps ? o.peakKbps : o.bitrateKbps * 3 / 2,
                                     .cqLevel = o.cqLevel, .gopFrames = o.fps * o.gopSeconds, .preset = o.preset,
                                     .rgbInput = o.rgbInput};
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
