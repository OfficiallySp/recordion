#pragma once

#include "mux/ByteSink.h"

#include <windows.h>

#include <array>
#include <filesystem>

namespace rec {

// Sequential file writer that bypasses the file cache (FILE_FLAG_NO_BUFFERING) with two 4 MB
// buffers in flight (overlapped I/O). Saving a large clip this way doesn't evict the game's cached
// files or standby memory, and the CPU only fills buffers while the disk drains the other one.
class AsyncFileWriter final : public ByteSink {
public:
    AsyncFileWriter() = default;
    ~AsyncFileWriter() override;
    AsyncFileWriter(const AsyncFileWriter&) = delete;
    AsyncFileWriter& operator=(const AsyncFileWriter&) = delete;

    bool open(const std::filesystem::path& path);
    bool write(std::span<const uint8_t> bytes) override;
    uint64_t position() const override { return position_; }
    // Flushes, trims the sector padding and closes. `creationTime` (FILETIME, optional) is stamped
    // on the file so Explorer sorts clips by when they happened, not when they were saved.
    bool close(const FILETIME* creationTime = nullptr);
    // Closes and deletes the partial file.
    void discard();
    bool failed() const { return failed_; }

private:
    static constexpr size_t kBufferSize = 4 * 1024 * 1024;

    struct Buffer {
        uint8_t* data = nullptr;
        size_t used = 0;
        OVERLAPPED ov{};
        bool pending = false;
    };

    bool submit(Buffer& b, size_t bytes);
    bool wait(Buffer& b);

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::filesystem::path path_;
    std::array<Buffer, 2> buffers_{};
    size_t current_ = 0;
    uint64_t position_ = 0;  // logical bytes written
    uint64_t fileOffset_ = 0; // offset of the next submitted buffer (sector aligned)
    size_t sectorSize_ = 4096;
    bool failed_ = false;
};

} // namespace rec
