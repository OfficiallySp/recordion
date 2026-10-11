#include "output/AsyncFileWriter.h"

#include "core/Log.h"
#include "core/Utf.h"

#include <algorithm>
#include <cstring>

namespace rec {

AsyncFileWriter::~AsyncFileWriter() {
    if (file_ != INVALID_HANDLE_VALUE) discard();
    for (auto& b : buffers_) {
        if (b.data) VirtualFree(b.data, 0, MEM_RELEASE);
        if (b.ov.hEvent) CloseHandle(b.ov.hEvent);
    }
}

bool AsyncFileWriter::open(const std::filesystem::path& path) {
    path_ = path;
    file_ = CreateFileW(path.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES | DELETE, FILE_SHARE_READ, nullptr,
                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        log::error("writer: cannot create {} (error {})", narrow(path.wstring()), GetLastError());
        return false;
    }
    FILE_STORAGE_INFO storage{};
    if (GetFileInformationByHandleEx(file_, FileStorageInfo, &storage, sizeof(storage)))
        sectorSize_ = std::max<size_t>(storage.PhysicalBytesPerSectorForPerformance, 512);

    for (auto& b : buffers_) {
        // VirtualAlloc is page aligned, which satisfies the sector alignment NO_BUFFERING requires.
        if (!b.data) b.data = static_cast<uint8_t*>(VirtualAlloc(nullptr, kBufferSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!b.ov.hEvent) b.ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!b.data || !b.ov.hEvent) return false;
        b.used = 0;
        b.pending = false;
    }
    current_ = 0;
    position_ = fileOffset_ = 0;
    failed_ = false;
    return true;
}

bool AsyncFileWriter::submit(Buffer& b, size_t bytes) {
    b.ov.Offset = static_cast<DWORD>(fileOffset_);
    b.ov.OffsetHigh = static_cast<DWORD>(fileOffset_ >> 32);
    ResetEvent(b.ov.hEvent);
    if (!WriteFile(file_, b.data, static_cast<DWORD>(bytes), nullptr, &b.ov) && GetLastError() != ERROR_IO_PENDING) {
        log::error("writer: write failed (error {})", GetLastError());
        failed_ = true;
        return false;
    }
    b.pending = true;
    fileOffset_ += bytes;
    return true;
}

bool AsyncFileWriter::wait(Buffer& b) {
    if (!b.pending) return true;
    DWORD done = 0;
    b.pending = false;
    if (!GetOverlappedResult(file_, &b.ov, &done, TRUE)) {
        log::error("writer: write failed (error {})", GetLastError());
        failed_ = true;
        return false;
    }
    b.used = 0;
    return true;
}

bool AsyncFileWriter::write(std::span<const uint8_t> bytes) {
    if (failed_ || file_ == INVALID_HANDLE_VALUE) return false;
    while (!bytes.empty()) {
        Buffer& b = buffers_[current_];
        const size_t n = std::min(bytes.size(), kBufferSize - b.used);
        std::memcpy(b.data + b.used, bytes.data(), n);
        b.used += n;
        bytes = bytes.subspan(n);
        position_ += n;
        if (b.used == kBufferSize) {
            if (!submit(b, kBufferSize)) return false;
            current_ ^= 1;
            if (!wait(buffers_[current_])) return false; // the other buffer must be free to refill
        }
    }
    return true;
}

bool AsyncFileWriter::close(const FILETIME* creationTime) {
    if (file_ == INVALID_HANDLE_VALUE) return false;
    Buffer& b = buffers_[current_];
    bool ok = !failed_;
    if (ok && b.used > 0) {
        // Unbuffered writes must be whole sectors: pad, then trim the file to its real size.
        const size_t padded = (b.used + sectorSize_ - 1) / sectorSize_ * sectorSize_;
        std::memset(b.data + b.used, 0, padded - b.used);
        ok = submit(b, padded);
    }
    for (auto& buf : buffers_) ok = wait(buf) && ok;
    if (ok) {
        FILE_END_OF_FILE_INFO eof{};
        eof.EndOfFile.QuadPart = static_cast<LONGLONG>(position_);
        ok = SetFileInformationByHandle(file_, FileEndOfFileInfo, &eof, sizeof(eof)) != FALSE;
    }
    if (ok && creationTime) {
        FILE_BASIC_INFO basic{};
        basic.CreationTime.LowPart = creationTime->dwLowDateTime;
        basic.CreationTime.HighPart = static_cast<LONG>(creationTime->dwHighDateTime);
        SetFileInformationByHandle(file_, FileBasicInfo, &basic, sizeof(basic)); // cosmetic; ignore failure
    }
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
    if (!ok) {
        log::error("writer: finishing {} failed", narrow(path_.wstring()));
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    return ok;
}

void AsyncFileWriter::discard() {
    if (file_ == INVALID_HANDLE_VALUE) return;
    CancelIoEx(file_, nullptr);
    for (auto& b : buffers_) {
        if (b.pending) {
            DWORD done = 0;
            GetOverlappedResult(file_, &b.ov, &done, TRUE);
            b.pending = false;
        }
        b.used = 0;
    }
    FILE_DISPOSITION_INFO del{TRUE};
    SetFileInformationByHandle(file_, FileDispositionInfo, &del, sizeof(del));
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
}

} // namespace rec
