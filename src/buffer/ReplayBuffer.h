#pragma once

#include "core/Settings.h"
#include "mux/Mp4Writer.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace rec {

inline constexpr uint8_t kVideoTrack = 0; // audio tracks are 1..N

struct BufferedPacket {
    uint64_t seq = 0;    // append order, across all tracks
    uint64_t offset = 0; // logical byte offset in the arena
    uint32_t size = 0;
    int64_t pts = 0;     // QPC timeline, 100 ns
    uint32_t segment = 0;
    uint8_t track = kVideoTrack;
    bool keyframe = false;
};

// Stream description for one encoder configuration; a resolution or codec change starts a new one.
struct SegmentInfo {
    Mp4VideoTrack video;
    std::vector<Mp4AudioTrack> audio; // audio track N is audio[N - 1]
};

// Ring of encoded packets in one pre-allocated arena (RAM, or a temporary memory-mapped file).
// Producers append from the pump/audio threads; savers take pinned snapshots that can be written
// out without copying while recording continues.
class ReplayBuffer : public std::enable_shared_from_this<ReplayBuffer> {
public:
    struct Options {
        size_t capacityBytes = 0;
        int64_t maxDuration100ns = 0;
        BufferStorage storage = BufferStorage::RAM;
        std::filesystem::path diskDirectory; // for BufferStorage::Disk
    };

    // Protects arena bytes from `from` onwards against eviction.
    struct Pin {
        uint64_t from = 0;
        std::atomic<bool> aborted{false};
    };

    class Snapshot {
    public:
        ~Snapshot();
        std::span<const uint8_t> bytes(const BufferedPacket& p) const;
        // True if the buffer had to reclaim this snapshot's space; the data can't be used.
        bool aborted() const { return pin_.aborted.load(std::memory_order_acquire); }

        std::vector<BufferedPacket> packets; // video and audio, in append order
        uint32_t segment = 0;
        std::shared_ptr<const SegmentInfo> info;

    private:
        friend class ReplayBuffer;
        ReplayBuffer* owner_ = nullptr;
        std::shared_ptr<ReplayBuffer> keepAlive_; // a save outlives the session that started it
        Pin pin_;
    };

    // Streams packets as they arrive (recordings). Everything not yet released stays pinned.
    class Reader {
    public:
        ~Reader();
        std::span<const uint8_t> bytes(const BufferedPacket& p) const;
        bool aborted() const { return pin_.aborted.load(std::memory_order_acquire); }
        int64_t startPts() const { return startPts_; } // the video keyframe the stream starts at
        uint32_t segment() const { return segment_; }

    private:
        friend class ReplayBuffer;
        ReplayBuffer* owner_ = nullptr;
        std::shared_ptr<ReplayBuffer> keepAlive_;
        Pin pin_;
        uint64_t nextSeq_ = 0;
        int64_t startPts_ = 0;
        uint32_t segment_ = 0;
    };

    ReplayBuffer() = default;
    ~ReplayBuffer();
    ReplayBuffer(const ReplayBuffer&) = delete;
    ReplayBuffer& operator=(const ReplayBuffer&) = delete;

    bool init(const Options& options);
    void clear();

    // Never blocks on a save: if the only free space is pinned, the overlapping saves are aborted
    // and this packet is dropped.
    void append(uint8_t track, int64_t pts, bool keyframe, uint32_t segment, std::span<const uint8_t> data);

    // Packets of `segment` (the newest one if 0) from the last video keyframe at or before `from`
    // (or the oldest keyframe held) through `to`. Audio starts up to `audioPreroll100ns` before the
    // keyframe, so the first audible frame decodes completely. nullptr if no keyframe is in range.
    std::shared_ptr<Snapshot> snapshot(int64_t from100ns, int64_t to100ns, uint32_t segment = 0,
                                       int64_t audioPreroll100ns = 0);

    // A reader starting at the newest video keyframe at or before `from100ns`, plus audio from
    // `audioPreroll100ns` before it. nullptr until the first keyframe has been buffered.
    std::shared_ptr<Reader> openReader(int64_t from100ns, int64_t audioPreroll100ns);
    // Appends every packet buffered since the previous call.
    void read(Reader& reader, std::vector<BufferedPacket>& out);
    // Data before `offset` has been written out and may be evicted.
    void release(Reader& reader, uint64_t offset);

    void setSegmentInfo(uint32_t segment, std::shared_ptr<const SegmentInfo> info);
    std::shared_ptr<const SegmentInfo> segmentInfo(uint32_t segment) const;

    struct Stats {
        size_t usedBytes = 0;
        size_t capacityBytes = 0;
        int64_t oldestVideoKeyframePts = 0;
        int64_t newestPts[4] = {};
        uint32_t newestSegment = 0;
        uint64_t droppedPackets = 0;
        uint64_t abortedSnapshots = 0;
        size_t committedBytes = 0; // RAM actually backing the buffer right now
    };
    Stats stats() const;
    int64_t newestPts(uint8_t track) const;
    // The video track is complete up to `pts` although no packet was appended: nothing changed on
    // screen, so the last frame still holds. Lets saves stop waiting for video that won't come.
    void advanceVideoClock(int64_t pts);

private:
    void unpin(Pin* pin);
    bool evictFor(uint64_t newEnd, int64_t newestPts); // false if pinned data is in the way
    uint8_t* physical(uint64_t logical) const { return arena_ + (logical % capacity_); }
    void releaseArena();
    bool commit(uint64_t offset, size_t size);
    void decommitDead();

    mutable std::mutex mutex_;
    uint8_t* arena_ = nullptr;
    size_t capacity_ = 0;
    int64_t maxDuration_ = 0;
    HANDLE file_ = nullptr;
    HANDLE mapping_ = nullptr;

    // RAM mode reserves the whole arena but commits it in chunks as the write head reaches them,
    // and decommits chunks the replay window has left behind: memory follows the real bitrate.
    size_t chunk_ = 0;
    std::vector<uint8_t> committed_; // per chunk
    uint64_t deadChunks_ = 0;        // logical chunks below this have been released

    std::deque<BufferedPacket> packets_;
    uint64_t head_ = 0; // logical offset of the next write
    int64_t newestPts_[4] = {};
    uint32_t newestSegment_ = 0;
    std::vector<Pin*> pins_;
    uint64_t nextSeq_ = 0;
    std::vector<std::pair<uint32_t, std::shared_ptr<const SegmentInfo>>> segments_;
    uint64_t dropped_ = 0;
    uint64_t abortedSnapshots_ = 0;
};

// Arena size for a buffer of `seconds` at the given peak bitrates, with 25% headroom so saves
// can be written while new packets keep arriving.
size_t replayBufferBytes(uint32_t seconds, uint32_t gopSeconds, uint32_t videoPeakKbps, uint32_t audioKbps);

} // namespace rec
