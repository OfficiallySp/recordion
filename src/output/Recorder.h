#pragma once

#include "buffer/ReplayBuffer.h"
#include "output/ClipSaver.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <wil/resource.h>

namespace rec {

struct RecordJob {
    std::shared_ptr<ReplayBuffer> buffer;
    int64_t from100ns = 0; // start at the keyframe at or before this time (pre-roll)
    std::filesystem::path root;
    std::string fileTemplate;
    std::string game;
    int64_t anchorQpc100ns = 0; // the same instant on both clocks, for wall-clock file names
    FILETIME anchorWall{};
};

// Streams a recording from the replay buffer into a hybrid MP4 (see Mp4FragmentedWriter) on a
// background-priority thread. Each finished file is posted to `window` as `message` with a
// SaveResult* (type "Recording") in lParam.
class Recorder {
public:
    Recorder(HWND window, UINT message);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    bool start(RecordJob job);
    // Finishes with everything captured up to `at100ns` (QPC timeline); returns immediately.
    void stop(int64_t at100ns);
    bool active() const { return active_.load(std::memory_order_acquire); }
    double seconds() const { return seconds_.load(std::memory_order_relaxed); }

private:
    void run();
    void post(SaveResult result);

    HWND window_;
    UINT message_;
    RecordJob job_;
    std::thread thread_;
    wil::unique_event_nothrow stopEvent_;
    std::atomic<int64_t> stopAt_{INT64_MAX};
    std::atomic<bool> active_{false};
    std::atomic<double> seconds_{0};
};

} // namespace rec
