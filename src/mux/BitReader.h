#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace rec {

// MSB-first bit reader with Exp-Golomb support. Reading past the end yields zeros and sets
// overrun(), so parsers can read straight through and check once at the end.
class BitReader {
public:
    explicit BitReader(std::span<const uint8_t> data) : data_(data) {}

    uint32_t bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    }
    uint64_t bits64(int n) {
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    }
    bool flag() { return bit() != 0; }
    void skip(size_t n) { pos_ += n; }

    // Unsigned Exp-Golomb (H.264/HEVC ue(v)).
    uint32_t ue() {
        int zeros = 0;
        while (bit() == 0) {
            if (++zeros > 31 || overrun()) return 0;
        }
        return (zeros == 0) ? 0 : ((1u << zeros) - 1 + bits(zeros));
    }
    // Signed Exp-Golomb (se(v)).
    int32_t se() {
        const uint32_t k = ue();
        return (k & 1) ? static_cast<int32_t>((k + 1) / 2) : -static_cast<int32_t>(k / 2);
    }
    // AV1 uvlc().
    uint32_t uvlc() {
        int zeros = 0;
        while (bit() == 0) {
            if (++zeros >= 32 || overrun()) return UINT32_MAX;
        }
        return zeros == 0 ? 0 : bits(zeros) + (1u << zeros) - 1;
    }

    size_t bitPosition() const { return pos_; }
    bool overrun() const { return pos_ > data_.size() * 8; }

private:
    uint32_t bit() {
        const size_t byte = pos_ >> 3;
        const uint32_t b = byte < data_.size() ? (data_[byte] >> (7 - (pos_ & 7))) & 1u : 0u;
        ++pos_;
        return b;
    }

    std::span<const uint8_t> data_;
    size_t pos_ = 0;
};

} // namespace rec
