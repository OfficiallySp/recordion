#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace rec {

// Destination for muxed bytes, written strictly sequentially.
class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual bool write(std::span<const uint8_t> bytes) = 0;
    virtual uint64_t position() const = 0;
};

class VectorSink final : public ByteSink {
public:
    bool write(std::span<const uint8_t> bytes) override {
        data.insert(data.end(), bytes.begin(), bytes.end());
        return true;
    }
    uint64_t position() const override { return data.size(); }

    std::vector<uint8_t> data;
};

} // namespace rec
