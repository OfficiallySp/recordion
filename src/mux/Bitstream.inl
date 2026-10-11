#pragma once

namespace rec::bitstream {

template <class F>
bool forEachObu(std::span<const uint8_t> data, F&& fn) {
    size_t pos = 0;
    while (pos < data.size()) {
        const uint8_t header = data[pos];
        if (header & 0x80) return false; // forbidden bit
        const uint8_t type = (header >> 3) & 0x0F;
        const bool hasExtension = (header & 0x04) != 0;
        const bool hasSize = (header & 0x02) != 0;
        size_t headerLen = 1 + (hasExtension ? 1 : 0);
        if (pos + headerLen > data.size()) return false;

        uint64_t payloadSize = 0;
        if (hasSize) {
            size_t lebLen = 0;
            if (!readLeb128(data.subspan(pos + headerLen), payloadSize, lebLen)) return false;
            headerLen += lebLen;
        } else {
            payloadSize = data.size() - pos - headerLen; // last OBU without size field
        }
        if (pos + headerLen + payloadSize > data.size()) return false;
        fn(type, data.subspan(pos, headerLen + static_cast<size_t>(payloadSize)),
           data.subspan(pos + headerLen, static_cast<size_t>(payloadSize)));
        pos += headerLen + static_cast<size_t>(payloadSize);
    }
    return true;
}

} // namespace rec::bitstream
