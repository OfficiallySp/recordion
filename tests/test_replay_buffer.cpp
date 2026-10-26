#include "buffer/ReplayBuffer.h"

#include <doctest/doctest.h>

#include <numeric>

using rec::ReplayBuffer;

namespace {

constexpr int64_t kFrame = 166'667; // 60 fps in 100 ns units

std::vector<uint8_t> payload(size_t n, uint8_t fill) { return std::vector<uint8_t>(n, fill); }

ReplayBuffer::Options options(size_t bytes, double seconds) {
    ReplayBuffer::Options o;
    o.capacityBytes = bytes;
    o.maxDuration100ns = static_cast<int64_t>(seconds * 10'000'000);
    return o;
}

// 60 fps video with a keyframe every `gop` frames, `size` bytes each, starting at t=0.
void appendVideo(ReplayBuffer& b, int frames, int gop, size_t size, uint32_t segment = 1, int firstFrame = 0) {
    for (int i = firstFrame; i < firstFrame + frames; ++i)
        b.append(rec::kVideoTrack, i * kFrame, i % gop == 0, segment, payload(size, static_cast<uint8_t>(i)));
}

} // namespace

TEST_CASE("snapshot starts on the keyframe at or before the requested start") {
    ReplayBuffer b;
    REQUIRE(b.init(options(1 << 20, 100)));
    appendVideo(b, 600, 120, 100); // 10 s, keyframes every 2 s

    auto snap = b.snapshot(5 * 10'000'000, 9 * 10'000'000); // last ~4 s, ending at 9 s
    REQUIRE(snap);
    REQUIRE_FALSE(snap->packets.empty());
    CHECK(snap->packets.front().keyframe);
    CHECK(snap->packets.front().pts == 240 * kFrame); // keyframe at 4 s
    CHECK(snap->packets.back().pts <= 9 * 10'000'000);
    // Bytes are readable and belong to the right packet.
    CHECK(snap->bytes(snap->packets.front())[0] == static_cast<uint8_t>(240));
}

TEST_CASE("start before the oldest data falls back to the oldest keyframe") {
    ReplayBuffer b;
    REQUIRE(b.init(options(1 << 20, 100)));
    appendVideo(b, 300, 60, 100, 1, 30); // starts mid-GOP at frame 30
    auto snap = b.snapshot(0, 600 * kFrame);
    REQUIRE(snap);
    CHECK(snap->packets.front().pts == 60 * kFrame); // first keyframe held
}

TEST_CASE("old packets are evicted by duration") {
    ReplayBuffer b;
    REQUIRE(b.init(options(4 << 20, 2.0)));
    appendVideo(b, 600, 60, 100);
    const auto s = b.stats();
    // About 2 s (120 frames) are kept; the oldest keyframe is within the last ~2 s.
    CHECK(s.oldestVideoKeyframePts >= (600 - 121) * kFrame);
    CHECK(s.usedBytes <= 121 * 100 + 100);
}

TEST_CASE("the video clock moves on while nothing changes on screen") {
    ReplayBuffer b;
    REQUIRE(b.init(options(1 << 20, 100)));
    appendVideo(b, 60, 60, 100); // 1 s
    b.advanceVideoClock(3 * 10'000'000);
    CHECK(b.newestPts(rec::kVideoTrack) == 3 * 10'000'000);
    b.advanceVideoClock(2 * 10'000'000); // never backwards
    CHECK(b.newestPts(rec::kVideoTrack) == 3 * 10'000'000);
    // A save up to the clock gets every frame, ending with the last one encoded.
    auto snap = b.snapshot(0, 3 * 10'000'000);
    REQUIRE(snap);
    CHECK(snap->packets.back().pts == 59 * kFrame);
}

TEST_CASE("old packets are evicted when the arena is full, and data never wraps mid-packet") {
    ReplayBuffer b;
    REQUIRE(b.init(options(1 << 16, 1000))); // 64 KB arena
    appendVideo(b, 2000, 30, 1000);          // 2 MB through a 64 KB arena
    const auto s = b.stats();
    CHECK(s.usedBytes <= 65536);
    auto snap = b.snapshot(0, INT64_MAX);
    REQUIRE(snap);
    for (const auto& p : snap->packets) {
        const auto bytes = snap->bytes(p);
        REQUIRE(bytes.size() == 1000);
        const int frame = static_cast<int>(p.pts / kFrame);
        CHECK(bytes.front() == static_cast<uint8_t>(frame));
        CHECK(bytes.back() == static_cast<uint8_t>(frame));
    }
}

TEST_CASE("pinned data is never overwritten; a save that blocks the buffer is aborted") {
    ReplayBuffer b;
    REQUIRE(b.init(options(1 << 16, 1000)));
    appendVideo(b, 50, 10, 1000);
    auto snap = b.snapshot(0, INT64_MAX);
    REQUIRE(snap);
    appendVideo(b, 200, 10, 1000, 1, 50); // would need the pinned space
    CHECK(snap->aborted());
    CHECK(b.stats().abortedSnapshots == 1);
    CHECK(b.stats().droppedPackets >= 1);

    snap.reset(); // the saver gives up; producers can reclaim the space
    appendVideo(b, 100, 10, 1000, 1, 250);
    auto again = b.snapshot(0, INT64_MAX);
    REQUIRE(again);
    CHECK_FALSE(again->aborted());
    CHECK(again->packets.back().pts == 349 * kFrame);
}

TEST_CASE("a save that fits in the headroom is not disturbed") {
    ReplayBuffer b;
    REQUIRE(b.init(options(1 << 20, 1000)));
    appendVideo(b, 100, 10, 1000);
    auto snap = b.snapshot(0, INT64_MAX);
    REQUIRE(snap);
    appendVideo(b, 100, 10, 1000, 1, 100);
    CHECK_FALSE(snap->aborted());
    CHECK(snap->bytes(snap->packets.front())[0] == 0);
}

TEST_CASE("audio from after the keyframe is included even if it was appended before it") {
    ReplayBuffer b;
    REQUIRE(b.init(options(1 << 20, 1000)));
    // Audio is ready before the video encoder outputs the frame it belongs with.
    b.append(1, 9 * kFrame, true, 1, payload(10, 0xA0));  // before keyframe pts: excluded
    b.append(1, 10 * kFrame + 5, true, 1, payload(10, 0xA1));
    b.append(rec::kVideoTrack, 10 * kFrame, true, 1, payload(10, 0xF0));
    b.append(1, 11 * kFrame, true, 1, payload(10, 0xA2));
    b.append(rec::kVideoTrack, 11 * kFrame, false, 1, payload(10, 0xF1));

    auto snap = b.snapshot(10 * kFrame, 20 * kFrame);
    REQUIRE(snap);
    std::vector<uint8_t> firstBytes;
    for (const auto& p : snap->packets) firstBytes.push_back(snap->bytes(p)[0]);
    CHECK(firstBytes == std::vector<uint8_t>{0xA1, 0xF0, 0xA2, 0xF1});
}

TEST_CASE("snapshots only use one encoder segment") {
    ReplayBuffer b;
    REQUIRE(b.init(options(1 << 20, 1000)));
    appendVideo(b, 60, 30, 100, 1);
    appendVideo(b, 60, 30, 100, 2, 60); // e.g. resolution changed
    auto snap = b.snapshot(0, INT64_MAX);
    REQUIRE(snap);
    CHECK(snap->segment == 2);
    CHECK(snap->packets.front().pts == 60 * kFrame);
    for (const auto& p : snap->packets) CHECK(p.segment == 2);
}

TEST_CASE("RAM follows what the replay window holds, not the worst-case size") {
    ReplayBuffer b;
    REQUIRE(b.init(options(64 << 20, 2.0))); // room for 64 MB, but keep only 2 s
    CHECK(b.stats().committedBytes == 0);
    // 20 s of 60 fps at 20 KB a frame (~10 Mbps): about 2.4 MB live at any time.
    appendVideo(b, 1200, 60, 20'000);
    const auto s = b.stats();
    CHECK(s.capacityBytes == 64u << 20);
    CHECK(s.committedBytes <= 6u << 20); // live data plus the chunks at either end
    CHECK(s.committedBytes >= s.usedBytes);
    // Data that is still held reads back intact after the arena wrapped and chunks were recycled.
    auto snap = b.snapshot(0, INT64_MAX);
    REQUIRE(snap);
    for (const auto& p : snap->packets) CHECK(snap->bytes(p)[0] == static_cast<uint8_t>(p.pts / kFrame));
}

TEST_CASE("buffer sizing covers the clip, one GOP and headroom") {
    // 120 s + 2 s GOP + 1 s at 75 Mbps + 192 kbps, plus 25%
    const size_t bytes = rec::replayBufferBytes(120, 2, 75'000, 192);
    CHECK(bytes == (123ull * 75'192 * 1000 / 8) * 5 / 4);
}

TEST_CASE("reader streams new packets from a keyframe and pins what it hasn't released") {
    ReplayBuffer b;
    REQUIRE(b.init(options(1 << 16, 1000))); // 64 KB arena
    appendVideo(b, 25, 10, 1000);            // keyframes at 0, 10, 20
    auto reader = b.openReader(15 * kFrame, 0);
    REQUIRE(reader);
    CHECK(reader->startPts() == 10 * kFrame);

    std::vector<rec::BufferedPacket> got;
    b.read(*reader, got);
    REQUIRE(got.size() == 15);
    CHECK(got.front().pts == 10 * kFrame);
    CHECK(reader->bytes(got.front())[0] == 10);

    appendVideo(b, 5, 10, 1000, 1, 25);
    got.clear();
    b.read(*reader, got);
    REQUIRE(got.size() == 5);
    CHECK(got.front().pts == 25 * kFrame);

    // Unreleased data blocks the producer from wrapping onto it...
    appendVideo(b, 60, 10, 1000, 1, 30);
    CHECK(reader->aborted());

    // ...while a reader that releases as it writes never gets in the way.
    auto r2 = b.openReader(INT64_MAX, 0);
    REQUIRE(r2);
    reader.reset();
    for (int i = 0; i < 200; ++i) {
        appendVideo(b, 1, 10, 1000, 1, 90 + i);
        std::vector<rec::BufferedPacket> batch;
        b.read(*r2, batch);
        if (!batch.empty()) b.release(*r2, batch.back().offset + batch.back().size);
    }
    CHECK_FALSE(r2->aborted());
}

TEST_CASE("reader includes audio pre-roll that was buffered before the keyframe") {
    ReplayBuffer b;
    REQUIRE(b.init(options(1 << 20, 1000)));
    b.append(1, 8 * kFrame, true, 1, payload(10, 0xA0)); // too early
    b.append(1, 9 * kFrame, true, 1, payload(10, 0xA1)); // within pre-roll
    b.append(rec::kVideoTrack, 10 * kFrame, true, 1, payload(10, 0xF0));
    auto reader = b.openReader(10 * kFrame, kFrame + kFrame / 2);
    REQUIRE(reader);
    std::vector<rec::BufferedPacket> got;
    b.read(*reader, got);
    REQUIRE(got.size() == 2);
    CHECK(reader->bytes(got[0])[0] == 0xA1);
    CHECK(reader->bytes(got[1])[0] == 0xF0);
}
