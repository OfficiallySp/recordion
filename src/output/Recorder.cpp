#include "output/Recorder.h"

#include "core/Clock.h"
#include "core/Log.h"
#include "core/ThreadQos.h"
#include "core/Utf.h"
#include "mux/Mp4Writer.h"
#include "output/AsyncFileWriter.h"
#include "output/FileNaming.h"
#include "output/WallClock.h"

#include <algorithm>
#include <deque>

namespace rec {
namespace {

constexpr int64_t kAudioPreroll100ns = 2 * 1024 * 10'000'000LL / 48000; // decoder warm-up frames
constexpr DWORD kPollMs = 200;
constexpr int64_t kOpenTimeout100ns = 50'000'000;  // wait up to 5 s for the first keyframe
constexpr int64_t kCatchUp100ns = 4'000'000;       // frames still in the encoders when stopping

// One output file of a recording (a resolution change mid-recording starts the next one).
struct OutputFile {
    std::filesystem::path path;
    AsyncFileWriter sink;
    Mp4FragmentedWriter mp4;
    FILETIME startWall{};
};

} // namespace

Recorder::Recorder(HWND window, UINT message) : window_(window), message_(message) {
    stopEvent_.create(wil::EventOptions::ManualReset);
}

Recorder::~Recorder() {
    if (thread_.joinable()) {
        stop(clock::now100ns());
        thread_.join();
    }
}

bool Recorder::start(RecordJob job) {
    if (active()) return false;
    if (thread_.joinable()) thread_.join(); // the previous recording has finished
    job_ = std::move(job);
    stopAt_ = INT64_MAX;
    seconds_ = 0;
    stopEvent_.ResetEvent();
    active_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void Recorder::stop(int64_t at100ns) {
    int64_t expected = INT64_MAX;
    stopAt_.compare_exchange_strong(expected, at100ns);
    stopEvent_.SetEvent();
}

void Recorder::post(SaveResult result) {
    result.type = "Recording";
    result.game = job_.game;
    auto* r = new SaveResult(std::move(result));
    if (!PostMessageW(window_, message_, 0, reinterpret_cast<LPARAM>(r))) delete r;
}

void Recorder::run() {
    qos::setThreadName(L"recorder");
    qos::BackgroundModeScope background;
    ReplayBuffer& buffer = *job_.buffer;

    // The stream starts at a keyframe; at the very start of a session there may not be one yet.
    std::shared_ptr<ReplayBuffer::Reader> reader;
    for (const int64_t deadline = clock::now100ns() + kOpenTimeout100ns; !reader;) {
        reader = buffer.openReader(job_.from100ns, kAudioPreroll100ns);
        if (reader || clock::now100ns() > deadline || stopAt_ != INT64_MAX) break;
        Sleep(50);
    }
    if (!reader) {
        post({.error = "Nothing has been captured yet"});
        active_ = false;
        return;
    }

    uint32_t segment = reader->segment();
    int64_t start = reader->startPts();
    std::shared_ptr<const SegmentInfo> info;
    std::deque<BufferedPacket> video;
    std::vector<std::deque<BufferedPacket>> audio;
    std::vector<BufferedPacket> incoming;
    std::unique_ptr<OutputFile> file;
    double finishedSeconds = 0; // earlier files of this recording
    int64_t stopDeadline = 0;

    auto releaseWritten = [&] {
        uint64_t keep = UINT64_MAX; // everything still queued stays pinned
        if (!video.empty()) keep = video.front().offset;
        for (const auto& q : audio)
            if (!q.empty()) keep = std::min(keep, q.front().offset);
        if (keep != UINT64_MAX) buffer.release(*reader, keep);
    };

    // Writes the first `videoCount` queued frames, the last of them showing until `videoEnd`, and
    // the audio before `audioUntil`.
    auto writeFragment = [&](size_t videoCount, int64_t videoEnd, int64_t audioUntil) {
        std::vector<Mp4Sample> v;
        for (size_t i = 0; i < videoCount; ++i) v.push_back({reader->bytes(video[i]), video[i].pts, video[i].keyframe});
        std::vector<std::vector<Mp4Sample>> a(audio.size());
        std::vector<std::span<const Mp4Sample>> spans;
        for (size_t t = 0; t < audio.size(); ++t) {
            while (!audio[t].empty() && audio[t].front().pts < audioUntil) {
                a[t].push_back({reader->bytes(audio[t].front()), audio[t].front().pts, true});
                audio[t].pop_front(); // safe: the bytes stay pinned until release below
            }
            spans.emplace_back(a[t]);
        }
        const bool ok = file->mp4.writeFragment(file->sink, v, spans, videoEnd);
        video.erase(video.begin(), video.begin() + static_cast<ptrdiff_t>(videoCount));
        releaseWritten();
        seconds_ = finishedSeconds + file->mp4.durationSeconds();
        return ok;
    };

    auto finishFile = [&](std::string error) {
        SaveResult r;
        r.path = file->path;
        r.bytes = file->sink.position();
        r.seconds = file->mp4.durationSeconds();
        const bool closed = file->sink.close(&file->startWall);
        if (closed && file->mp4.videoSamples() > 0 && file->mp4.finish(file->path)) {
            r.ok = true;              // a cut-short recording is still a recording...
            r.error = std::move(error); // ...and this says why it ended early
            log::info("recording saved: {} ({:.1f} s, {:.1f} MB)", narrow(r.path.wstring()), r.seconds, r.bytes / 1048576.0);
        } else if (!closed) {
            r.error = "Writing the file failed (is the disk full?)";
        } else {
            r.error = "The recording is empty";
            std::error_code ec;
            std::filesystem::remove(file->path, ec);
        }
        finishedSeconds += r.seconds;
        post(std::move(r));
        file.reset();
    };

    enum class Open { Ready, NotYet, Failed };
    auto openFile = [&]() -> Open {
        info = buffer.segmentInfo(segment);
        if (!info) return Open::NotYet; // the encoder hasn't described this segment yet
        if (audio.size() != info->audio.size()) audio.resize(info->audio.size());
        std::vector<int64_t> audioStart;
        for (const auto& q : audio) {
            if (q.empty()) return Open::NotYet; // audio runs ~100 ms behind video; wait for it
            audioStart.push_back(q.front().pts);
        }
        file = std::make_unique<OutputFile>();
        file->startWall = wallclock::fromQpc(start, job_.anchorQpc100ns, job_.anchorWall);
        NamingContext naming;
        naming.game = job_.game;
        naming.type = "Recording";
        naming.localTime = wallclock::toLocal(file->startWall);
        file->path = makeOutputPath(job_.root, job_.fileTemplate, naming);
        if (!file->sink.open(file->path) ||
            !file->mp4.begin(file->sink, info->video, info->audio, start, audioStart, job_.game,
                             wallclock::toUnixSeconds(file->startWall))) {
            post({.path = file->path, .error = "Could not create the file"});
            file.reset();
            return Open::Failed;
        }
        log::info("recording to {}", narrow(file->path.wstring()));
        return Open::Ready;
    };

    for (bool failed = false; !failed;) {
        const bool signalled = WaitForSingleObject(stopEvent_.get(), kPollMs) == WAIT_OBJECT_0;
        const int64_t stopAt = stopAt_.load();
        if (signalled && !stopDeadline) stopDeadline = clock::now100ns() + kCatchUp100ns;

        incoming.clear();
        buffer.read(*reader, incoming);
        if (reader->aborted()) {
            log::error("recording: fell behind the capture and lost data");
            if (file) finishFile("The disk couldn't keep up; the recording was cut short");
            else post({.error = "The disk couldn't keep up"});
            break;
        }
        for (const auto& p : incoming) {
            if (p.segment > segment && p.track == kVideoTrack && p.keyframe) {
                // Resolution or codec changed: finish this file, continue in a new one.
                if (file) {
                    if (!video.empty()) writeFragment(video.size(), p.pts, p.pts);
                    finishFile({});
                }
                video.clear();
                for (auto& q : audio) std::erase_if(q, [&](const BufferedPacket& a) { return a.pts < p.pts - kAudioPreroll100ns; });
                segment = p.segment;
                start = p.pts;
            }
            if (p.segment != segment && p.track == kVideoTrack) continue;
            if (p.track == kVideoTrack) {
                if (p.pts >= start && p.pts <= stopAt) video.push_back(p);
            } else if (p.pts >= start - kAudioPreroll100ns && p.pts <= stopAt) {
                if (audio.size() < p.track) audio.resize(p.track);
                audio[p.track - 1].push_back(p);
            }
        }

        if (!file && !video.empty() && openFile() == Open::Failed) break;
        if (file) {
            // One fragment per GOP: write everything before the newest complete GOP's keyframe.
            for (size_t k = 1; k < video.size() && !failed; ++k) {
                if (!video[k].keyframe) continue;
                if (!writeFragment(k, video[k].pts, video[k].pts)) {
                    finishFile("Writing the file failed (is the disk full?)");
                    failed = true;
                }
                k = 0; // the keyframe is now at the front; keep scanning after it
            }
            if (failed) break;
        }

        if (stopDeadline) {
            // Audio is mixed ~100 ms behind real time: wait for it too, or the file ends early.
            bool caughtUp = buffer.newestPts(kVideoTrack) >= stopAt;
            for (size_t t = 1; t <= audio.size() && caughtUp; ++t)
                caughtUp = buffer.newestPts(static_cast<uint8_t>(t)) >= stopAt;
            if (caughtUp || clock::now100ns() >= stopDeadline) {
                if (file) {
                    // Unchanged frames aren't encoded: the last one may have held until the stop.
                    int64_t end = 0;
                    if (!video.empty() && stopAt != INT64_MAX && info)
                        end = std::max(stopAt, video.back().pts + clock::kTicksPerSecond * info->video.fpsDen / info->video.fpsNum);
                    if (!video.empty()) writeFragment(video.size(), end, INT64_MAX);
                    finishFile({});
                } else {
                    post({.error = "Nothing was recorded"});
                }
                break;
            }
        }
    }
    active_ = false;
}

} // namespace rec
