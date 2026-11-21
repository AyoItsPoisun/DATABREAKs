
#pragma once

// DATABREAKs Engine 029 - storage abstraction (target abstraction layer).
//
// The core never touches files, flash or any other medium. It reads and
// writes BYTES through these interfaces, which a target adapter implements
// for its medium (host file, internal flash, external storage, ...). Host
// implementations live in platform/host/.
//
//   ByteSink    sequential writer (what a serializer streams into).
//   ByteSource  random-access reader of a finished object (what a loader
//               validates and decodes; flash and files both allow reads at
//               an offset, which lets the loader validate before it decodes
//               without buffering the whole object).
//   StorageBackend  named objects with an ATOMIC replace: beginWrite() opens
//               a new version, commitWrite() makes it the current one in a
//               single step, abortWrite() discards it. A failed or
//               interrupted save therefore never damages the previous
//               version (the backend's job; e.g. a file backend writes a
//               temporary file and renames it, a flash backend writes the
//               other of two slots and flips a header).
//
// Contract: not real-time. Call from a non-audio thread or while audio is
// stopped. Implementations must not be shared between threads without the
// caller's own synchronisation. Names: 1-31 characters from [a-z0-9_.-].
//
// Standard C++ only; no allocation required by the interfaces themselves.

#include <cstddef>
#include <cstdint>

namespace databreaks {

class ByteSink {
public:
    virtual ~ByteSink() = default;
    // Appends n bytes; false on any failure (the write is then unusable).
    virtual bool write(const std::uint8_t* data, std::size_t n) = 0;
};

class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual std::size_t size() const = 0;
    // Reads n bytes at `offset`; false if out of range or the medium fails.
    virtual bool read(std::size_t offset, std::uint8_t* dst, std::size_t n) const = 0;
};

class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    // Starts writing a new version of object `name`; nullptr on failure
    // (invalid name, no space, another write open). The returned sink is
    // valid until commitWrite() or abortWrite().
    virtual ByteSink* beginWrite(const char* name) = 0;
    // Atomically replaces the object with what was written; false (and the
    // previous version kept) on failure.
    virtual bool commitWrite() = 0;
    // Discards the open write; the previous version is kept.
    virtual void abortWrite() = 0;

    // Opens the current version of `name` for reading; nullptr if absent.
    // Valid until closeRead() or the next openRead()/commitWrite().
    virtual const ByteSource* openRead(const char* name) = 0;
    virtual void closeRead() = 0;

    static bool validName(const char* name) {
        if (name == nullptr) return false;
        std::size_t n = 0;
        for (; name[n] != '\0'; ++n) {
            const char c = name[n];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                            c == '.' || c == '-';
            if (!ok || n >= 31) return false;
        }
        return n >= 1;
    }
};

// A ByteSource over a caller-owned memory range (no copy).
class MemoryByteSource final : public ByteSource {
public:
    MemoryByteSource(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    std::size_t size() const override { return size_; }
    bool read(std::size_t offset, std::uint8_t* dst, std::size_t n) const override {
        if (offset > size_ || n > size_ - offset || (n > 0 && data_ == nullptr)) return false;
        for (std::size_t i = 0; i < n; ++i) dst[i] = data_[offset + i];
        return true;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
};

} // namespace databreaks
