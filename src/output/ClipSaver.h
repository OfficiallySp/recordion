#pragma once

#include "buffer/ReplayBuffer.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rec {

struct SaveJob {
    std::shared_ptr<ReplayBuffer> buffer;
    int64_t from100ns = 0;  // QPC timeline
    int64_t to100ns = 0;
    std::filesystem::path root;
    std::string fileTemplate;
    std::string game;
    std::string type = "Replay";
    // The same instant on both clocks, to turn capture timestamps into a wall-clock file name.
    int64_t anchorQpc100ns = 0;
    FILETIME anchorWall{};
};

struct SaveResult {
    bool ok = false;
    std::filesystem::path path;
    std::string game;
    std::string error;
    double seconds = 0;
    uint64_t bytes = 0;
};

// Writes replay clips on a background-priority thread. Each finished save is posted to `window`
// as `message` with a heap-allocated SaveResult* in lParam (the receiver deletes it).
class ClipSaver {
public:
    ClipSaver(HWND window, UINT message);
    ~ClipSaver();
    ClipSaver(const ClipSaver&) = delete;
    ClipSaver& operator=(const ClipSaver&) = delete;

    void enqueue(SaveJob job);
    bool busy() const;

private:
    void run(std::stop_token stop);
    SaveResult save(const SaveJob& job);

    HWND window_;
    UINT message_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<SaveJob> queue_;
    bool working_ = false;
    std::jthread thread_;
};

} // namespace rec
