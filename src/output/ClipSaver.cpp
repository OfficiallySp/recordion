#include "output/ClipSaver.h"

#include "core/Clock.h"
#include "core/Log.h"
#include "core/ThreadQos.h"
#include "core/Utf.h"
#include "mux/Mp4Writer.h"
#include "output/AsyncFileWriter.h"
#include "output/FileNaming.h"

namespace rec {
namespace {

constexpr int64_t kWaitForEncoder100ns = 1'500'000; // 150 ms for in-flight frames to come out

FILETIME addTicks(FILETIME t, int64_t ticks) {
    ULARGE_INTEGER u{.LowPart = t.dwLowDateTime, .HighPart = t.dwHighDateTime};
    u.QuadPart += ticks;
    return {u.LowPart, u.HighPart};
}

SYSTEMTIME toLocal(FILETIME utc) {
    SYSTEMTIME st{}, local{};
    FileTimeToSystemTime(&utc, &st);
    SystemTimeToTzSpecificLocalTime(nullptr, &st, &local);
    return local;
}

int64_t toUnixSeconds(FILETIME t) {
    const ULARGE_INTEGER u{.LowPart = t.dwLowDateTime, .HighPart = t.dwHighDateTime};
    return static_cast<int64_t>(u.QuadPart / 10'000'000) - 11'644'473'600LL;
}

} // namespace

ClipSaver::ClipSaver(HWND window, UINT message)
    : window_(window), message_(message), thread_([this](std::stop_token st) { run(st); }) {}

ClipSaver::~ClipSaver() {
    thread_.request_stop();
    cv_.notify_all();
}

void ClipSaver::enqueue(SaveJob job) {
    {
        std::scoped_lock lock(mutex_);
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
}

bool ClipSaver::busy() const {
    std::scoped_lock lock(mutex_);
    return working_ || !queue_.empty();
}

void ClipSaver::run(std::stop_token stop) {
    qos::setThreadName(L"clip saver");
    qos::BackgroundModeScope background; // low I/O and memory priority: the game comes first
    for (;;) {
        SaveJob job;
        {
            std::unique_lock lock(mutex_);
            if (!cv_.wait(lock, stop, [&] { return !queue_.empty(); })) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            working_ = true;
        }
        auto* result = new SaveResult(save(job));
        {
            std::scoped_lock lock(mutex_);
            working_ = false;
        }
        if (!PostMessageW(window_, message_, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }
}

SaveResult ClipSaver::save(const SaveJob& job) {
    SaveResult r;
    r.game = job.game;
    // Frames captured just before the hotkey are still in the encoder: give them a moment.
    const int64_t deadline = clock::now100ns() + kWaitForEncoder100ns;
    while (job.buffer->newestPts(kVideoTrack) < job.to100ns && clock::now100ns() < deadline) Sleep(5);

    const auto snap = job.buffer->snapshot(job.from100ns, job.to100ns);
    if (!snap || !snap->info) {
        r.error = "Nothing has been recorded yet";
        return r;
    }

    Mp4ClipInput clip;
    clip.video = snap->info->video;
    for (const auto& p : snap->packets)
        if (p.track == kVideoTrack) clip.videoSamples.push_back({snap->bytes(p), p.pts, p.keyframe});
    if (clip.videoSamples.empty()) {
        r.error = "Nothing has been recorded yet";
        return r;
    }
    const int64_t first = clip.videoSamples.front().time100ns;
    const int64_t last = clip.videoSamples.back().time100ns;
    r.seconds = clock::toSeconds(last - first) + static_cast<double>(clip.video.fpsDen) / clip.video.fpsNum;

    const FILETIME startWall = addTicks(job.anchorWall, first - job.anchorQpc100ns);
    clip.title = job.game;
    clip.creationUnixTime = toUnixSeconds(startWall);

    NamingContext naming;
    naming.game = job.game;
    naming.type = job.type;
    naming.localTime = toLocal(startWall);
    naming.durationSeconds = static_cast<int64_t>(r.seconds + 0.5);
    r.path = makeOutputPath(job.root, job.fileTemplate, naming);

    AsyncFileWriter file;
    if (!file.open(r.path)) {
        r.error = "Could not create the file";
        return r;
    }
    if (!writeMp4Clip(clip, file) || file.failed()) {
        file.discard();
        r.error = "Writing the file failed (is the disk full?)";
        return r;
    }
    if (snap->aborted()) {
        // The buffer had to reclaim this data mid-save, so what we wrote may be inconsistent.
        file.discard();
        r.error = "The replay buffer overflowed while saving";
        return r;
    }
    r.bytes = file.position();
    if (!file.close(&startWall)) {
        r.error = "Finishing the file failed";
        return r;
    }
    r.ok = true;
    log::info("saved {:.1f} s replay to {} ({:.1f} MB)", r.seconds, narrow(r.path.wstring()), r.bytes / 1048576.0);
    return r;
}

} // namespace rec
