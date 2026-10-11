#include "buffer/ReplayBuffer.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>

namespace rec {

size_t replayBufferBytes(uint32_t seconds, uint32_t gopSeconds, uint32_t videoPeakKbps, uint32_t audioKbps) {
    // One extra GOP so a full-length clip can still start on a keyframe.
    const uint64_t span = uint64_t{seconds} + gopSeconds + 1;
    const uint64_t bytes = span * (uint64_t{videoPeakKbps} + audioKbps) * 1000 / 8;
    return static_cast<size_t>(bytes + bytes / 4);
}

ReplayBuffer::~ReplayBuffer() { releaseArena(); }

bool ReplayBuffer::init(const Options& o) {
    std::scoped_lock lock(mutex_);
    releaseArena();
    capacity_ = (o.capacityBytes + 0xFFFF) & ~size_t{0xFFFF};
    maxDuration_ = o.maxDuration100ns;

    if (o.storage == BufferStorage::Disk) {
        // A temporary, delete-on-close mapping: the memory manager keeps it in RAM while memory is
        // plentiful and spills to disk under pressure, instead of growing our commit charge.
        std::error_code ec;
        std::filesystem::create_directories(o.diskDirectory, ec);
        const auto path = o.diskDirectory / L"replay-buffer.tmp";
        file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            file_ = nullptr;
            log::error("replay: cannot create buffer file (error {})", GetLastError());
            return false;
        }
        const ULARGE_INTEGER size{.QuadPart = capacity_};
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, size.HighPart, size.LowPart, nullptr);
        if (mapping_) arena_ = static_cast<uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, capacity_));
    } else {
        arena_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, capacity_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    if (!arena_) {
        log::error("replay: cannot allocate {} MB buffer (error {})", capacity_ >> 20, GetLastError());
        releaseArena();
        return false;
    }
    log::info("replay: {} MB buffer in {}", capacity_ >> 20, o.storage == BufferStorage::Disk ? "a temp file" : "RAM");
    return true;
}

void ReplayBuffer::releaseArena() {
    for (Snapshot* s : pins_) {
        s->aborted_.store(true, std::memory_order_release);
        s->owner_ = nullptr;
    }
    pins_.clear();
    packets_.clear();
    head_ = 0;
    if (mapping_) {
        if (arena_) UnmapViewOfFile(arena_);
        CloseHandle(mapping_);
        mapping_ = nullptr;
    } else if (arena_) {
        VirtualFree(arena_, 0, MEM_RELEASE);
    }
    arena_ = nullptr;
    if (file_) {
        CloseHandle(file_);
        file_ = nullptr;
    }
}

void ReplayBuffer::clear() {
    std::scoped_lock lock(mutex_);
    for (Snapshot* s : pins_) s->aborted_.store(true, std::memory_order_release);
    packets_.clear();
    std::fill(std::begin(newestPts_), std::end(newestPts_), 0);
}

bool ReplayBuffer::evictFor(uint64_t newEnd, int64_t newestPts) {
    uint64_t pinFrom = UINT64_MAX;
    for (const Snapshot* s : pins_)
        if (!s->aborted()) pinFrom = std::min(pinFrom, s->pinFrom_);

    while (!packets_.empty()) {
        const BufferedPacket& front = packets_.front();
        const bool needSpace = newEnd - front.offset > capacity_;
        const bool tooOld = maxDuration_ > 0 && newestPts - front.pts > maxDuration_;
        if (!needSpace && !tooOld) break;
        if (front.offset >= pinFrom) {
            if (needSpace) return false; // a save is reading this data
            break;                       // old but pinned: keep it until the save ends
        }
        packets_.pop_front();
    }
    return packets_.empty() || newEnd - packets_.front().offset <= capacity_;
}

void ReplayBuffer::append(uint8_t track, int64_t pts, bool keyframe, uint32_t segment, std::span<const uint8_t> data) {
    std::scoped_lock lock(mutex_);
    if (!arena_ || data.empty() || data.size() > capacity_ / 4 || track >= 4) return;

    // Packets are stored contiguously: skip to the start of the arena if this one would wrap.
    uint64_t offset = head_;
    const size_t phys = offset % capacity_;
    if (phys + data.size() > capacity_) offset += capacity_ - phys;

    const int64_t newest = std::max(pts, newestPts_[kVideoTrack]);
    if (!evictFor(offset + data.size(), newest)) {
        // Pinned data is in the way: give the space back by aborting the saves that hold it.
        for (Snapshot* s : pins_) {
            if (!s->aborted_.exchange(true)) {
                ++abortedSnapshots_;
                log::warn("replay: buffer full while saving; save aborted");
            }
        }
        ++dropped_;
        return; // the aborted saves unpin shortly; until then, drop rather than overwrite
    }

    std::memcpy(physical(offset), data.data(), data.size());
    packets_.push_back({offset, static_cast<uint32_t>(data.size()), pts, segment, track, keyframe});
    head_ = offset + data.size();
    newestPts_[track] = std::max(newestPts_[track], pts);
    newestSegment_ = std::max(newestSegment_, segment);
}

std::shared_ptr<ReplayBuffer::Snapshot> ReplayBuffer::snapshot(int64_t from, int64_t to, uint32_t segment,
                                                               int64_t audioPreroll) {
    std::scoped_lock lock(mutex_);
    if (packets_.empty()) return nullptr;
    if (segment == 0) segment = newestSegment_;

    // Newest keyframe at or before `from`; otherwise the oldest keyframe still held.
    const BufferedPacket* start = nullptr;
    for (const auto& p : packets_) {
        if (p.track != kVideoTrack || !p.keyframe || p.segment != segment) continue;
        if (p.pts <= from || !start) start = &p;
        if (p.pts > from) break;
    }
    if (!start || start->pts > to) return nullptr;

    auto snap = std::shared_ptr<Snapshot>(new Snapshot());
    snap->segment = segment;
    snap->owner_ = this;
    snap->keepAlive_ = weak_from_this().lock(); // null when the buffer isn't shared (tests)
    for (const auto& [id, info] : segments_)
        if (id == segment) snap->info = info;
    // Audio and video arrive with different latencies, so audio from after the keyframe can sit
    // earlier in the arena: select by timestamp, then pin from the first selected byte.
    const int64_t startPts = start->pts;
    for (const auto& p : packets_) {
        const int64_t first = p.track == kVideoTrack ? startPts : startPts - audioPreroll;
        if (p.segment == segment && p.pts >= first && p.pts <= to) snap->packets.push_back(p);
    }
    snap->pinFrom_ = snap->packets.front().offset; // packets_ is in offset order
    pins_.push_back(snap.get());
    return snap;
}

void ReplayBuffer::setSegmentInfo(uint32_t segment, std::shared_ptr<const SegmentInfo> info) {
    std::scoped_lock lock(mutex_);
    std::erase_if(segments_, [&](const auto& e) { return e.first == segment; });
    segments_.emplace_back(segment, std::move(info));
    if (segments_.size() > 16) segments_.erase(segments_.begin()); // old segments are long evicted
}

std::shared_ptr<const SegmentInfo> ReplayBuffer::segmentInfo(uint32_t segment) const {
    std::scoped_lock lock(mutex_);
    for (const auto& [id, info] : segments_)
        if (id == segment) return info;
    return nullptr;
}

void ReplayBuffer::unpin(Snapshot* s) {
    std::scoped_lock lock(mutex_);
    std::erase(pins_, s);
}

ReplayBuffer::Snapshot::~Snapshot() {
    if (owner_) owner_->unpin(this);
}

std::span<const uint8_t> ReplayBuffer::Snapshot::bytes(const BufferedPacket& p) const {
    if (!owner_) return {};
    return {owner_->physical(p.offset), p.size};
}

ReplayBuffer::Stats ReplayBuffer::stats() const {
    std::scoped_lock lock(mutex_);
    Stats s;
    s.capacityBytes = capacity_;
    if (!packets_.empty()) s.usedBytes = static_cast<size_t>(head_ - packets_.front().offset);
    for (const auto& p : packets_) {
        if (p.track == kVideoTrack && p.keyframe) {
            s.oldestVideoKeyframePts = p.pts;
            break;
        }
    }
    std::copy(std::begin(newestPts_), std::end(newestPts_), s.newestPts);
    s.newestSegment = newestSegment_;
    s.droppedPackets = dropped_;
    s.abortedSnapshots = abortedSnapshots_;
    return s;
}

int64_t ReplayBuffer::newestPts(uint8_t track) const {
    std::scoped_lock lock(mutex_);
    return track < 4 ? newestPts_[track] : 0;
}

} // namespace rec
