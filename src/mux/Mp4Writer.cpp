#include "mux/Mp4Writer.h"

#include "mux/BoxWriter.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <format>

namespace rec {
namespace {

constexpr uint32_t kMovieTimescale = 1000;
constexpr uint64_t kMp4EpochOffset = 2'082'844'800; // seconds from 1904-01-01 to 1970-01-01
constexpr int64_t kChunkSpan100ns = 5'000'000;       // interleave granularity: 0.5 s
constexpr uint32_t kAacFrameSamples = 1024;
constexpr uint16_t kLanguageUnd = 0x55C4; // packed ISO-639-2 "und"

int64_t rescale(int64_t value, int64_t from, int64_t to) {
    const int64_t half = from / 2;
    return value >= 0 ? (value * to + half) / from : -((-value * to + half) / from);
}

struct Track {
    bool video = false;
    uint32_t id = 0;
    uint32_t timescale = 0;
    const std::vector<Mp4Sample>* samples = nullptr;
    const Mp4AudioTrack* audio = nullptr;

    std::vector<uint32_t> deltas;
    uint64_t mediaDuration = 0;
    uint64_t emptyEditMovie = 0; // track starts after the movie start
    int64_t mediaTime = 0;       // first presented media time (skips encoder priming)
    uint64_t editDurationMovie = 0;

    std::vector<uint32_t> chunkFirstSample;
    std::vector<uint64_t> chunkOffsets;
};

uint32_t videoTimescale(const Mp4VideoTrack& v) { return v.fpsDen == 1 ? v.fpsNum * 1000 : v.fpsNum; }

uint32_t sampleSize(const Mp4Sample& s) { return s.data.empty() ? s.size : static_cast<uint32_t>(s.data.size()); }

void layoutVideo(Track& t, const Mp4ClipInput& in) {
    const auto& s = in.videoSamples;
    const int64_t t0 = s.front().time100ns;
    t.timescale = videoTimescale(in.video);
    const auto nominal = static_cast<uint32_t>(std::max<int64_t>(1, t.timescale * int64_t{in.video.fpsDen} / in.video.fpsNum));
    t.deltas.resize(s.size());
    int64_t prev = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        // Each decode time is rounded independently, so per-sample rounding never accumulates.
        const int64_t dts = rescale(s[i].time100ns - t0, 10'000'000, t.timescale);
        t.deltas[i - 1] = static_cast<uint32_t>(std::max<int64_t>(1, dts - prev));
        prev = std::max(dts, prev + 1);
    }
    t.deltas.back() = nominal;
    if (in.videoEnd100ns > s.back().time100ns)
        t.deltas.back() = static_cast<uint32_t>(std::max<int64_t>(1, rescale(in.videoEnd100ns - t0, 10'000'000, t.timescale) - prev));
    for (uint32_t d : t.deltas) t.mediaDuration += d;
    t.editDurationMovie = static_cast<uint64_t>(rescale(static_cast<int64_t>(t.mediaDuration), t.timescale, kMovieTimescale));
}

void layoutAudio(Track& t, const Mp4AudioInput& a, int64_t movieStart100ns) {
    t.timescale = a.track.sampleRate;
    t.deltas.assign(a.samples.size(), kAacFrameSamples);
    t.mediaDuration = uint64_t{kAacFrameSamples} * a.samples.size();

    const int64_t offset = a.samples.front().time100ns - movieStart100ns;
    int64_t skip = a.track.primingSamples;
    if (offset < 0) skip += rescale(-offset, 10'000'000, t.timescale); // audio began before the first video frame
    skip = std::min<int64_t>(skip, static_cast<int64_t>(t.mediaDuration));
    t.mediaTime = skip;
    if (offset > 0) t.emptyEditMovie = static_cast<uint64_t>(rescale(offset, 10'000'000, kMovieTimescale));
    t.editDurationMovie = static_cast<uint64_t>(
        rescale(static_cast<int64_t>(t.mediaDuration) - skip, t.timescale, kMovieTimescale));
}

void buildChunks(Track& t) {
    const auto& s = *t.samples;
    int64_t chunkStart = 0;
    for (uint32_t i = 0; i < s.size(); ++i) {
        if (i == 0 || s[i].time100ns - chunkStart >= kChunkSpan100ns) {
            t.chunkFirstSample.push_back(i);
            chunkStart = s[i].time100ns;
        }
    }
    t.chunkOffsets.assign(t.chunkFirstSample.size(), 0);
}

uint32_t chunkSampleCount(const Track& t, size_t chunk) {
    const uint32_t end = chunk + 1 < t.chunkFirstSample.size() ? t.chunkFirstSample[chunk + 1]
                                                              : static_cast<uint32_t>(t.samples->size());
    return end - t.chunkFirstSample[chunk];
}

void writeFtyp(BoxWriter& w, Codec codec, bool fragmented = false) {
    const size_t b = w.begin("ftyp");
    w.fourcc("isom");
    w.u32(0x200);
    for (const char* brand : {"isom", "iso2", "mp41"}) w.fourcc(brand);
    if (fragmented) w.fourcc("iso6");
    if (codec == Codec::H264) w.fourcc("avc1");
    if (codec == Codec::AV1) w.fourcc("av01");
    w.end(b);
}

void writeVideoSampleEntry(BoxWriter& w, const Mp4VideoTrack& v) {
    const char* type = v.codec == Codec::H264 ? "avc1" : v.codec == Codec::HEVC ? "hvc1" : "av01";
    const size_t entry = w.begin(type);
    w.zeros(6);
    w.u16(1); // data_reference_index
    w.zeros(16);
    w.u16(static_cast<uint16_t>(v.width));
    w.u16(static_cast<uint16_t>(v.height));
    w.u32(0x00480000); // 72 dpi
    w.u32(0x00480000);
    w.u32(0);
    w.u16(1); // frame_count
    w.zeros(32); // compressorname
    w.u16(0x0018);
    w.u16(0xFFFF);

    const char* configType = v.codec == Codec::H264 ? "avcC" : v.codec == Codec::HEVC ? "hvcC" : "av1C";
    const size_t cfg = w.begin(configType);
    w.bytes(v.configRecord);
    w.end(cfg);

    // BT.709, limited range: what the colour converter produces and the encoders signal.
    const size_t colr = w.begin("colr");
    w.fourcc("nclx");
    w.u16(1);
    w.u16(1);
    w.u16(1);
    w.u8(0);
    w.end(colr);
    w.end(entry);
}

void writeDescriptor(BoxWriter& w, uint8_t tag, const std::vector<uint8_t>& body) {
    w.u8(tag);
    w.u8(static_cast<uint8_t>(body.size())); // all our descriptors are < 128 bytes
    w.bytes(body);
}

void writeAudioSampleEntry(BoxWriter& w, const Mp4AudioTrack& a) {
    const size_t entry = w.begin("mp4a");
    w.zeros(6);
    w.u16(1);
    w.zeros(8);
    w.u16(static_cast<uint16_t>(a.channels));
    w.u16(16);
    w.u32(0);
    w.u32(a.sampleRate << 16);

    // ES_Descriptor > DecoderConfigDescriptor > DecoderSpecificInfo (AudioSpecificConfig only).
    BoxWriter dsi;
    writeDescriptor(dsi, 0x05, a.audioSpecificConfig);
    BoxWriter dcd;
    dcd.u8(0x40); // MPEG-4 audio
    dcd.u8(0x15); // audio stream
    dcd.u24(0);
    dcd.u32(a.bitrate);
    dcd.u32(a.bitrate);
    dcd.bytes(dsi.data());
    BoxWriter es;
    es.u16(0); // ES_ID
    es.u8(0);
    writeDescriptor(es, 0x04, dcd.data());
    writeDescriptor(es, 0x06, {0x02}); // SLConfigDescriptor

    const size_t esds = w.beginFull("esds", 0, 0);
    writeDescriptor(w, 0x03, es.data());
    w.end(esds);
    w.end(entry);
}

void writeStbl(BoxWriter& w, const Track& t, const Mp4ClipInput& in) {
    const size_t stbl = w.begin("stbl");

    const size_t stsd = w.beginFull("stsd", 0, 0);
    w.u32(1);
    if (t.video) writeVideoSampleEntry(w, in.video);
    else writeAudioSampleEntry(w, *t.audio);
    w.end(stsd);

    // stts: run-length encoded sample durations
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    for (uint32_t d : t.deltas) {
        if (!runs.empty() && runs.back().second == d) ++runs.back().first;
        else runs.emplace_back(1, d);
    }
    const size_t stts = w.beginFull("stts", 0, 0);
    w.u32(static_cast<uint32_t>(runs.size()));
    for (auto [count, delta] : runs) {
        w.u32(count);
        w.u32(delta);
    }
    w.end(stts);

    const auto& samples = *t.samples;
    if (t.video) {
        const size_t stss = w.beginFull("stss", 0, 0);
        const size_t countPos = w.size();
        w.u32(0);
        uint32_t keyframes = 0;
        for (uint32_t i = 0; i < samples.size(); ++i) {
            if (samples[i].keyframe) {
                w.u32(i + 1);
                ++keyframes;
            }
        }
        auto& d = w.data();
        d[countPos] = static_cast<uint8_t>(keyframes >> 24);
        d[countPos + 1] = static_cast<uint8_t>(keyframes >> 16);
        d[countPos + 2] = static_cast<uint8_t>(keyframes >> 8);
        d[countPos + 3] = static_cast<uint8_t>(keyframes);
        w.end(stss);
    }

    // stsc: samples per chunk, run-length encoded
    std::vector<std::pair<uint32_t, uint32_t>> chunkRuns; // first chunk (1-based), samples per chunk
    for (size_t c = 0; c < t.chunkFirstSample.size(); ++c) {
        const uint32_t n = chunkSampleCount(t, c);
        if (chunkRuns.empty() || chunkRuns.back().second != n) chunkRuns.emplace_back(static_cast<uint32_t>(c + 1), n);
    }
    const size_t stsc = w.beginFull("stsc", 0, 0);
    w.u32(static_cast<uint32_t>(chunkRuns.size()));
    for (auto [first, n] : chunkRuns) {
        w.u32(first);
        w.u32(n);
        w.u32(1);
    }
    w.end(stsc);

    const size_t stsz = w.beginFull("stsz", 0, 0);
    w.u32(0);
    w.u32(static_cast<uint32_t>(samples.size()));
    for (const auto& s : samples) w.u32(sampleSize(s));
    w.end(stsz);

    const size_t co64 = w.beginFull("co64", 0, 0);
    w.u32(static_cast<uint32_t>(t.chunkOffsets.size()));
    for (uint64_t off : t.chunkOffsets) w.u64(off);
    w.end(co64);

    w.end(stbl);
}

void writeTrak(BoxWriter& w, const Track& t, const Mp4ClipInput& in, uint64_t macTime) {
    const size_t trak = w.begin("trak");

    const uint64_t trackDurationMovie = t.emptyEditMovie + t.editDurationMovie;
    const bool enabled = t.video || t.audio->enabled;
    const size_t tkhd = w.beginFull("tkhd", 1, enabled ? 0x7 : 0x2);
    w.u64(macTime);
    w.u64(macTime);
    w.u32(t.id);
    w.u32(0);
    w.u64(trackDurationMovie);
    w.zeros(8);
    w.u16(0);                         // layer
    w.u16(t.video ? 0 : 1);           // alternate_group: audio tracks are alternatives
    w.u16(t.video ? 0 : 0x0100);      // volume
    w.u16(0);
    w.unityMatrix();
    w.u32(t.video ? in.video.width << 16 : 0);
    w.u32(t.video ? in.video.height << 16 : 0);
    w.end(tkhd);

    const size_t edts = w.begin("edts");
    const size_t elst = w.beginFull("elst", 1, 0);
    w.u32(t.emptyEditMovie ? 2 : 1);
    if (t.emptyEditMovie) {
        w.u64(t.emptyEditMovie);
        w.u64(static_cast<uint64_t>(-1)); // empty edit
        w.u16(1);
        w.u16(0);
    }
    w.u64(t.editDurationMovie);
    w.u64(static_cast<uint64_t>(t.mediaTime));
    w.u16(1);
    w.u16(0);
    w.end(elst);
    w.end(edts);

    const size_t mdia = w.begin("mdia");
    const size_t mdhd = w.beginFull("mdhd", 1, 0);
    w.u64(macTime);
    w.u64(macTime);
    w.u32(t.timescale);
    w.u64(t.mediaDuration);
    w.u16(kLanguageUnd);
    w.u16(0);
    w.end(mdhd);

    const size_t hdlr = w.beginFull("hdlr", 0, 0);
    w.u32(0);
    w.fourcc(t.video ? "vide" : "soun");
    w.zeros(12);
    w.str(t.video ? "Video" : t.audio->name);
    w.u8(0);
    w.end(hdlr);

    const size_t minf = w.begin("minf");
    if (t.video) {
        const size_t vmhd = w.beginFull("vmhd", 0, 1);
        w.zeros(8);
        w.end(vmhd);
    } else {
        const size_t smhd = w.beginFull("smhd", 0, 0);
        w.zeros(4);
        w.end(smhd);
    }
    const size_t dinf = w.begin("dinf");
    const size_t dref = w.beginFull("dref", 0, 0);
    w.u32(1);
    const size_t url = w.beginFull("url ", 0, 1); // media is in this file
    w.end(url);
    w.end(dref);
    w.end(dinf);
    writeStbl(w, t, in);
    w.end(minf);
    w.end(mdia);
    w.end(trak);
}

void writeMetadataItem(BoxWriter& w, std::string_view type, std::string_view value) {
    const size_t item = w.begin(type);
    const size_t data = w.begin("data");
    w.u32(1); // UTF-8
    w.u32(0);
    w.str(value);
    w.end(data);
    w.end(item);
}

void writeUdta(BoxWriter& w, const Mp4ClipInput& in) {
    const size_t udta = w.begin("udta");
    const size_t meta = w.beginFull("meta", 0, 0);
    const size_t hdlr = w.beginFull("hdlr", 0, 0);
    w.u32(0);
    w.fourcc("mdir");
    w.fourcc("appl");
    w.zeros(8);
    w.u8(0);
    w.end(hdlr);
    const size_t ilst = w.begin("ilst");
    if (!in.title.empty()) writeMetadataItem(w, "\xA9nam", in.title);
    if (in.creationUnixTime > 0) {
        const auto tp = std::chrono::sys_seconds{std::chrono::seconds{in.creationUnixTime}};
        writeMetadataItem(w, "\xA9" "day", std::format("{:%Y-%m-%dT%H:%M:%SZ}", tp));
    }
    writeMetadataItem(w, "\xA9too", "Recordion");
    w.end(ilst);
    w.end(meta);
    w.end(udta);
}

void writeMoov(BoxWriter& w, const std::vector<Track>& tracks, const Mp4ClipInput& in, bool fragmented = false) {
    const uint64_t macTime = static_cast<uint64_t>(std::max<int64_t>(0, in.creationUnixTime)) + kMp4EpochOffset;
    uint64_t movieDuration = 0;
    for (const auto& t : tracks) movieDuration = std::max(movieDuration, t.emptyEditMovie + t.editDurationMovie);

    const size_t moov = w.begin("moov");
    const size_t mvhd = w.beginFull("mvhd", 1, 0);
    w.u64(macTime);
    w.u64(macTime);
    w.u32(kMovieTimescale);
    w.u64(movieDuration);
    w.u32(0x00010000); // rate 1.0
    w.u16(0x0100);     // volume 1.0
    w.zeros(10);
    w.unityMatrix();
    w.zeros(24);
    w.u32(static_cast<uint32_t>(tracks.size() + 1)); // next_track_ID
    w.end(mvhd);
    for (const auto& t : tracks) writeTrak(w, t, in, macTime);
    if (fragmented) {
        const size_t mvex = w.begin("mvex");
        for (const auto& t : tracks) {
            const size_t trex = w.beginFull("trex", 0, 0);
            w.u32(t.id);
            w.u32(1); // default_sample_description_index
            w.u32(0); // default duration, size and flags: every trun carries its own
            w.u32(0);
            w.u32(0);
            w.end(trex);
        }
        w.end(mvex);
    }
    writeUdta(w, in);
    w.end(moov);
}

// Track tables for `in`, laid out as in a clip (durations, edit lists); chunks are left empty.
std::vector<Track> layoutTracks(const Mp4ClipInput& in) {
    std::vector<Track> tracks;
    tracks.reserve(1 + in.audio.size());
    Track& video = tracks.emplace_back();
    video.video = true;
    video.id = 1;
    video.samples = &in.videoSamples;
    layoutVideo(video, in);
    for (const auto& a : in.audio) {
        if (a.samples.empty()) continue;
        Track& t = tracks.emplace_back();
        t.id = static_cast<uint32_t>(tracks.size());
        t.samples = &a.samples;
        t.audio = &a.track;
        layoutAudio(t, a, in.videoSamples.front().time100ns);
    }
    return tracks;
}

void putU32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
    d[at] = static_cast<uint8_t>(v >> 24);
    d[at + 1] = static_cast<uint8_t>(v >> 16);
    d[at + 2] = static_cast<uint8_t>(v >> 8);
    d[at + 3] = static_cast<uint8_t>(v);
}

} // namespace

bool writeMp4Clip(const Mp4ClipInput& in, ByteSink& sink) {
    if (in.videoSamples.empty() || !in.videoSamples.front().keyframe || in.video.fpsNum == 0) return false;

    std::vector<Track> tracks = layoutTracks(in);
    for (auto& t : tracks) buildChunks(t);

    // Interleave chunks of all tracks by start time.
    struct ChunkRef {
        int64_t time;
        uint32_t track;
        uint32_t chunk;
    };
    std::vector<ChunkRef> order;
    for (uint32_t ti = 0; ti < tracks.size(); ++ti)
        for (uint32_t c = 0; c < tracks[ti].chunkFirstSample.size(); ++c)
            order.push_back({(*tracks[ti].samples)[tracks[ti].chunkFirstSample[c]].time100ns, ti, c});
    std::stable_sort(order.begin(), order.end(), [](const ChunkRef& a, const ChunkRef& b) {
        return a.time != b.time ? a.time < b.time : a.track < b.track;
    });

    BoxWriter ftyp;
    writeFtyp(ftyp, in.video.codec);
    // The moov size doesn't depend on the offset values (co64 is fixed width), so measure it once
    // with zero offsets, then fill the real offsets in.
    BoxWriter probe;
    writeMoov(probe, tracks, in);
    const uint64_t mdatHeader = 16;
    uint64_t pos = ftyp.size() + probe.size() + mdatHeader;
    const uint64_t payloadStart = pos;
    for (const auto& ref : order) {
        Track& t = tracks[ref.track];
        t.chunkOffsets[ref.chunk] = pos;
        const uint32_t first = t.chunkFirstSample[ref.chunk];
        const uint32_t n = chunkSampleCount(t, ref.chunk);
        for (uint32_t i = first; i < first + n; ++i) pos += (*t.samples)[i].data.size();
    }
    BoxWriter moov;
    writeMoov(moov, tracks, in);
    if (moov.size() != probe.size()) return false;

    BoxWriter mdat;
    mdat.u32(1); // 64-bit largesize follows
    mdat.fourcc("mdat");
    mdat.u64(mdatHeader + (pos - payloadStart));

    if (!sink.write(ftyp.data()) || !sink.write(moov.data()) || !sink.write(mdat.data())) return false;
    for (const auto& ref : order) {
        const Track& t = tracks[ref.track];
        const uint32_t first = t.chunkFirstSample[ref.chunk];
        const uint32_t n = chunkSampleCount(t, ref.chunk);
        for (uint32_t i = first; i < first + n; ++i)
            if (!sink.write((*t.samples)[i].data)) return false;
    }
    return true;
}

bool Mp4FragmentedWriter::begin(ByteSink& sink, const Mp4VideoTrack& video, const std::vector<Mp4AudioTrack>& audio,
                                int64_t videoStart100ns, const std::vector<int64_t>& audioStart100ns, std::string title,
                                int64_t creationUnixTime) {
    if (video.fpsNum == 0 || audio.size() != audioStart100ns.size()) return false;
    tables_ = {};
    tables_.video = video;
    tables_.title = std::move(title);
    tables_.creationUnixTime = creationUnixTime;
    tables_.audio.resize(audio.size());
    for (size_t i = 0; i < audio.size(); ++i) tables_.audio[i].track = audio[i];
    chunks_.assign(1 + audio.size(), {});
    audioDecodeTime_.assign(audio.size(), 0);
    retire_.clear();
    videoStart_ = videoStart100ns;
    sequence_ = 0;

    // Empty tracks for the fragmented moov; only the audio edit list (pre-roll) is known up front.
    std::vector<Track> tracks(1 + audio.size());
    static const std::vector<Mp4Sample> kNone;
    tracks[0].video = true;
    tracks[0].id = 1;
    tracks[0].timescale = videoTimescale(video);
    tracks[0].samples = &kNone;
    for (size_t i = 0; i < audio.size(); ++i) {
        Track& t = tracks[i + 1];
        t.id = static_cast<uint32_t>(i + 2);
        t.timescale = audio[i].sampleRate;
        t.samples = &kNone;
        t.audio = &tables_.audio[i].track;
        const int64_t preroll = videoStart100ns - audioStart100ns[i];
        t.mediaTime = std::max<int64_t>(0, rescale(preroll, 10'000'000, t.timescale)) + audio[i].primingSamples;
    }

    BoxWriter head;
    writeFtyp(head, video.codec, true);
    const uint64_t moovAt = sink.position() + head.size();
    writeMoov(head, tracks, tables_, true);
    retire_.push_back(moovAt);
    return sink.write(head.data());
}

bool Mp4FragmentedWriter::writeFragment(ByteSink& sink, std::span<const Mp4Sample> video,
                                        const std::vector<std::span<const Mp4Sample>>& audio, int64_t videoEnd100ns) {
    if (video.empty() && std::ranges::all_of(audio, [](const auto& a) { return a.empty(); })) return true;
    const uint32_t vts = videoTimescale(tables_.video);
    const auto nominal = static_cast<uint32_t>(std::max<int64_t>(1, int64_t{vts} * tables_.video.fpsDen / tables_.video.fpsNum));

    struct Run {
        uint32_t trackId;
        bool video;
        uint64_t decodeTime;
        std::vector<uint32_t> durations;
        std::span<const Mp4Sample> samples;
        size_t trunOffsetField = 0;
        uint64_t bytes = 0;
    };
    std::vector<Run> runs;
    if (!video.empty()) {
        Run r{1, true, static_cast<uint64_t>(std::max<int64_t>(0, rescale(video.front().time100ns - videoStart_, 10'000'000, vts)))};
        int64_t prev = static_cast<int64_t>(r.decodeTime);
        for (size_t i = 1; i < video.size(); ++i) {
            const int64_t dts = rescale(video[i].time100ns - videoStart_, 10'000'000, vts);
            r.durations.push_back(static_cast<uint32_t>(std::max<int64_t>(1, dts - prev)));
            prev = std::max(dts, prev + 1);
        }
        const bool endKnown = videoEnd100ns > video.back().time100ns;
        r.durations.push_back(endKnown ? static_cast<uint32_t>(std::max<int64_t>(
                                             1, rescale(videoEnd100ns - videoStart_, 10'000'000, vts) - prev))
                                       : nominal);
        r.samples = video;
        runs.push_back(std::move(r));
        // The final moov derives every other duration from the next sample; the last needs this.
        tables_.videoEnd100ns = endKnown ? videoEnd100ns : 0;
    }
    for (size_t i = 0; i < audio.size() && i < tables_.audio.size(); ++i) {
        if (audio[i].empty()) continue;
        Run r{static_cast<uint32_t>(i + 2), false, audioDecodeTime_[i]};
        r.durations.assign(audio[i].size(), kAacFrameSamples);
        r.samples = audio[i];
        audioDecodeTime_[i] += uint64_t{kAacFrameSamples} * audio[i].size();
        runs.push_back(std::move(r));
    }
    for (auto& r : runs)
        for (const auto& s : r.samples) r.bytes += s.data.size();

    BoxWriter moof;
    const size_t moofBox = moof.begin("moof");
    const size_t mfhd = moof.beginFull("mfhd", 0, 0);
    moof.u32(++sequence_);
    moof.end(mfhd);
    for (auto& r : runs) {
        const size_t traf = moof.begin("traf");
        const size_t tfhd = moof.beginFull("tfhd", 0, 0x020000); // default-base-is-moof
        moof.u32(r.trackId);
        moof.end(tfhd);
        const size_t tfdt = moof.beginFull("tfdt", 1, 0);
        moof.u64(r.decodeTime);
        moof.end(tfdt);
        // data offset | sample duration | sample size [| sample flags]
        const size_t trun = moof.beginFull("trun", 0, r.video ? 0x000701 : 0x000301);
        moof.u32(static_cast<uint32_t>(r.samples.size()));
        r.trunOffsetField = moof.size();
        moof.u32(0); // patched below
        for (size_t i = 0; i < r.samples.size(); ++i) {
            moof.u32(r.durations[i]);
            moof.u32(static_cast<uint32_t>(r.samples[i].data.size()));
            // sync: depends on nothing; otherwise depends on others and is not a sync sample
            if (r.video) moof.u32(r.samples[i].keyframe ? 0x02000000 : 0x01010000);
        }
        moof.end(trun);
        moof.end(traf);
    }
    moof.end(moofBox);

    // Runs follow each other in one mdat; offsets are relative to the start of the moof.
    const uint64_t moofAt = sink.position();
    uint64_t dataOffset = moof.size() + 8;
    for (auto& r : runs) {
        putU32(moof.data(), r.trunOffsetField, static_cast<uint32_t>(dataOffset));
        auto& chunks = chunks_[r.video ? 0 : r.trackId - 1];
        auto& table = r.video ? tables_.videoSamples : tables_.audio[r.trackId - 2].samples;
        chunks.firstSample.push_back(static_cast<uint32_t>(table.size()));
        chunks.offsets.push_back(moofAt + dataOffset);
        for (const auto& s : r.samples) table.push_back({{}, s.time100ns, s.keyframe, static_cast<uint32_t>(s.data.size())});
        dataOffset += r.bytes;
    }

    uint64_t total = 0;
    for (const auto& r : runs) total += r.bytes;
    BoxWriter mdat;
    mdat.u32(static_cast<uint32_t>(8 + total));
    mdat.fourcc("mdat");
    if (!sink.write(moof.data()) || !sink.write(mdat.data())) return false;
    for (const auto& r : runs)
        for (const auto& s : r.samples)
            if (!sink.write(s.data)) return false;
    retire_.push_back(moofAt);
    return true;
}

double Mp4FragmentedWriter::durationSeconds() const {
    const auto& v = tables_.videoSamples;
    if (v.empty()) return 0;
    if (tables_.videoEnd100ns > v.back().time100ns) return (tables_.videoEnd100ns - v.front().time100ns) / 1e7;
    return (v.back().time100ns - v.front().time100ns) / 1e7 + static_cast<double>(tables_.video.fpsDen) / tables_.video.fpsNum;
}

bool Mp4FragmentedWriter::finish(const std::filesystem::path& file) {
    if (tables_.videoSamples.empty() || !tables_.videoSamples.front().keyframe) return false;

    // The complete moov: every sample, with each fragment's run as one chunk.
    std::vector<Track> tracks = layoutTracks(tables_);
    for (auto& t : tracks) {
        size_t index = 0; // chunks_ is indexed like begin(): video, then every audio input
        for (size_t i = 0; i < tables_.audio.size(); ++i)
            if (t.audio == &tables_.audio[i].track) index = i + 1;
        t.chunkFirstSample = chunks_[index].firstSample;
        t.chunkOffsets = chunks_[index].offsets;
    }
    BoxWriter moov;
    writeMoov(moov, tracks, tables_);

    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    auto writeAt = [&](uint64_t offset, std::span<const uint8_t> bytes) {
        LARGE_INTEGER pos{.QuadPart = static_cast<LONGLONG>(offset)};
        DWORD done = 0;
        return SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) &&
               WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &done, nullptr) && done == bytes.size();
    };
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && writeAt(static_cast<uint64_t>(size.QuadPart), moov.data());
    // Only now retire the fragmented boxes: until the new moov is safely written, the file stays a
    // valid fragmented MP4.
    static constexpr uint8_t kFree[4] = {'f', 'r', 'e', 'e'};
    if (ok) ok = FlushFileBuffers(h);
    for (uint64_t at : retire_)
        if (ok) ok = writeAt(at + 4, kFree);
    if (ok) ok = FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

} // namespace rec
