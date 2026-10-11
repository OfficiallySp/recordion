#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace rec {

// Big-endian ISO-BMFF box builder. begin() reserves a 32-bit size that end() patches.
class BoxWriter {
public:
    size_t begin(std::string_view type) {
        const size_t start = buf_.size();
        u32(0);
        fourcc(type);
        return start;
    }
    size_t beginFull(std::string_view type, uint8_t version, uint32_t flags) {
        const size_t start = begin(type);
        u32((uint32_t{version} << 24) | (flags & 0xFFFFFF));
        return start;
    }
    void end(size_t start) {
        const auto size = static_cast<uint32_t>(buf_.size() - start);
        buf_[start] = static_cast<uint8_t>(size >> 24);
        buf_[start + 1] = static_cast<uint8_t>(size >> 16);
        buf_[start + 2] = static_cast<uint8_t>(size >> 8);
        buf_[start + 3] = static_cast<uint8_t>(size);
    }

    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v >> 8));
        u8(static_cast<uint8_t>(v));
    }
    void u24(uint32_t v) {
        u8(static_cast<uint8_t>(v >> 16));
        u16(static_cast<uint16_t>(v));
    }
    void u32(uint32_t v) {
        u16(static_cast<uint16_t>(v >> 16));
        u16(static_cast<uint16_t>(v));
    }
    void u64(uint64_t v) {
        u32(static_cast<uint32_t>(v >> 32));
        u32(static_cast<uint32_t>(v));
    }
    void zeros(size_t n) { buf_.insert(buf_.end(), n, 0); }
    void bytes(std::span<const uint8_t> b) { buf_.insert(buf_.end(), b.begin(), b.end()); }
    void str(std::string_view s) { bytes({reinterpret_cast<const uint8_t*>(s.data()), s.size()}); }
    void fourcc(std::string_view s) { str(s.substr(0, 4)); }
    // Unity transformation matrix used by mvhd/tkhd.
    void unityMatrix() {
        for (uint32_t v : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u}) u32(v);
    }

    const std::vector<uint8_t>& data() const { return buf_; }
    std::vector<uint8_t>& data() { return buf_; }
    size_t size() const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
};

} // namespace rec
