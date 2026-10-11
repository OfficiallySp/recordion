#pragma once

#include <d3d11.h>

#include <winrt/base.h>

#include <array>
#include <cstdint>

namespace rec {

// Measures GPU time of a span of commands with timestamp queries, read back a few frames later
// without stalling (D3D11_ASYNC_GETDATA_DONOTFLUSH).
class GpuTimer {
public:
    bool init(ID3D11Device* device, ID3D11DeviceContext* context);
    void begin();
    void end();
    // Collects finished measurements; returns how many were added to the totals.
    int collect();

    double averageMs() const { return samples_ ? totalMs_ / samples_ : 0.0; }
    double maxMs() const { return maxMs_; }
    uint64_t samples() const { return samples_; }

private:
    struct Slot {
        winrt::com_ptr<ID3D11Query> disjoint, start, stop;
        bool pending = false;
    };
    static constexpr size_t kSlots = 8;

    ID3D11DeviceContext* context_ = nullptr;
    std::array<Slot, kSlots> slots_;
    size_t writeIndex_ = 0;
    size_t readIndex_ = 0;
    double totalMs_ = 0;
    double maxMs_ = 0;
    uint64_t samples_ = 0;
};

} // namespace rec
