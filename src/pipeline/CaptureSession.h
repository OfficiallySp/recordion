#pragma once

#include "buffer/ReplayBuffer.h"
#include "capture/ICaptureSource.h"
#include "core/Settings.h"
#include "encode/IVideoEncoder.h"
#include "gpu/ColorConverter.h"
#include "gpu/D3DDevice.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <wil/resource.h>

namespace rec {

class ClipSaver;

struct SessionConfig {
    CaptureTarget target;
    std::string name = "Desktop"; // game name; used for the output folder and file names
    Settings settings;            // snapshot taken when the session starts
    bool synthetic = false;       // render test frames instead of capturing (benchmarks/tests)
    HWND notifyWindow = nullptr;  // receives notifyMessage with wParam = kSessionFailed / kSessionEnded
    UINT notifyMessage = 0;
};

inline constexpr WPARAM kSessionFailed = 1; // fatal error; the session has stopped
inline constexpr WPARAM kSessionEnded = 2;  // the captured window or display went away

struct SessionStats {
    uint64_t framesCaptured = 0;
    uint64_t framesEncoded = 0;
    uint64_t duplicates = 0;
    uint64_t droppedBusy = 0;
    uint64_t encodeErrors = 0;
    bool zeroCopy = false;
    Size outputSize{};
    ReplayBuffer::Stats buffer;
};

// One capture target (a display or a game window) encoded continuously into a replay buffer.
// All GPU and encoder work happens on the session's pump thread.
class CaptureSession {
public:
    explicit CaptureSession(SessionConfig config);
    ~CaptureSession();
    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    // Starts the pump thread and waits until capture and the encoder are running (or failed).
    bool start();
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    // Queues a save of the last `seconds` up to now.
    void saveReplay(ClipSaver& saver, uint32_t seconds) const;

    SessionStats stats() const;
    const SessionConfig& config() const { return config_; }

private:
    class BufferSink;

    void run();
    bool initPipeline();
    bool configureEncoder(Size captureSize, Size textureSize);
    void tick(int64_t pts);
    void fail(const char* what);

    SessionConfig config_;
    std::shared_ptr<ReplayBuffer> buffer_;
    std::thread thread_;
    wil::unique_event_nothrow stopEvent_;
    wil::unique_event_nothrow startedEvent_;
    std::atomic<bool> running_{false};
    std::atomic<bool> startOk_{false};

    // Pump-thread state.
    std::optional<GpuDevice> gpu_;
    std::unique_ptr<ICaptureSource> source_;
    std::unique_ptr<IVideoEncoder> encoder_;
    std::unique_ptr<BufferSink> sink_;
    VideoProcessorConverter converter_;
    Size captureSize_{};
    Size outputSize_{};
    bool zeroCopy_ = false;
    uint32_t segment_ = 0;
    bool forceKeyframe_ = true;
    std::optional<CapturedFrame> lastFrame_;     // zero-copy: re-sent for duplicates
    std::optional<InputSurface> lastSurface_;    // converted path: re-sent for duplicates
    uint32_t consecutiveErrors_ = 0;

    std::atomic<uint64_t> framesCaptured_{0}, framesEncoded_{0}, duplicates_{0}, droppedBusy_{0}, encodeErrors_{0};
    std::atomic<bool> statsZeroCopy_{false};
    std::atomic<uint32_t> statsWidth_{0}, statsHeight_{0};
};

} // namespace rec
