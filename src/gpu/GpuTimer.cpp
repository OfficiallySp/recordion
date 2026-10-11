#include "gpu/GpuTimer.h"

#include <algorithm>

namespace rec {

bool GpuTimer::init(ID3D11Device* device, ID3D11DeviceContext* context) {
    context_ = context;
    for (auto& s : slots_) {
        D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(device->CreateQuery(&d, s.disjoint.put()))) return false;
        d.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(device->CreateQuery(&d, s.start.put())) || FAILED(device->CreateQuery(&d, s.stop.put()))) return false;
    }
    return true;
}

void GpuTimer::begin() {
    Slot& s = slots_[writeIndex_ % kSlots];
    if (s.pending) return; // ring full: skip this measurement rather than stall
    context_->Begin(s.disjoint.get());
    context_->End(s.start.get());
}

void GpuTimer::end() {
    Slot& s = slots_[writeIndex_ % kSlots];
    if (s.pending) return;
    context_->End(s.stop.get());
    context_->End(s.disjoint.get());
    s.pending = true;
    ++writeIndex_;
}

int GpuTimer::collect() {
    int added = 0;
    while (readIndex_ < writeIndex_) {
        Slot& s = slots_[readIndex_ % kSlots];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 t0 = 0, t1 = 0;
        if (context_->GetData(s.disjoint.get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        if (context_->GetData(s.start.get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        if (context_->GetData(s.stop.get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        s.pending = false;
        ++readIndex_;
        if (dj.Disjoint || dj.Frequency == 0 || t1 < t0) continue;
        const double ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
        totalMs_ += ms;
        maxMs_ = std::max(maxMs_, ms);
        ++samples_;
        ++added;
    }
    return added;
}

} // namespace rec
