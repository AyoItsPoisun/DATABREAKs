
#pragma once

// DATABREAKs Engine 029 - host/test StorageBackend in fixed memory.
//
// Up to MaxObjects named objects of at most ObjectCapacity bytes each, plus
// one staging buffer for the write in progress. commitWrite() copies the
// staging buffer over the object in one step, so a failed or aborted write
// never touches the committed version (the same contract a flash backend
// must provide, e.g. with two alternating slots).
//
// Fault injection for tests: fail a write after N bytes, fail the commit,
// fail reads at or after an offset. No allocation (all storage is inside
// the object - make large instances static or heap-allocated).

#include "../storage.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace databreaks {

template <std::size_t ObjectCapacity, std::size_t MaxObjects = 4>
class MemoryStorage final : public StorageBackend {
public:
    // ---- Fault injection ----------------------------------------------------
    void failWriteAfter(std::size_t bytes) { failWriteAfter_ = bytes; }
    void failNextCommit() { failCommit_ = true; }
    void failReadsFrom(std::size_t offset) { failReadFrom_ = offset; }
    void clearFaults() {
        failWriteAfter_ = NoFault;
        failCommit_ = false;
        failReadFrom_ = NoFault;
    }

    // ---- StorageBackend -------------------------------------------------------
    ByteSink* beginWrite(const char* name) override {
        if (writing_ || !validName(name)) return nullptr;
        writing_ = true;
        copyName(pendingName_, name);
        sink_.reset();
        return &sink_;
    }

    bool commitWrite() override {
        if (!writing_) return false;
        writing_ = false;
        if (failCommit_ || sink_.failed) {
            failCommit_ = false;
            return false;
        }
        Object* o = find(pendingName_.data());
        if (o == nullptr) o = freeObject();
        if (o == nullptr) return false;
        o->used = true;
        o->name = pendingName_;
        std::memcpy(o->data.data(), sink_.data.data(), sink_.size);
        o->size = sink_.size;
        ++commits_;
        return true;
    }

    void abortWrite() override { writing_ = false; }

    const ByteSource* openRead(const char* name) override {
        const Object* o = validName(name) ? find(name) : nullptr;
        if (o == nullptr) return nullptr;
        source_.object = o;
        source_.failFrom = failReadFrom_;
        return &source_;
    }
    void closeRead() override { source_.object = nullptr; }

    // ---- Inspection (tests) ----------------------------------------------------
    std::size_t objectSize(const char* name) {
        const Object* o = find(name);
        return o == nullptr ? 0 : o->size;
    }
    const std::uint8_t* objectData(const char* name) {
        const Object* o = find(name);
        return o == nullptr ? nullptr : o->data.data();
    }
    std::uint64_t commitCount() const { return commits_; }

private:
    static constexpr std::size_t NoFault = ~std::size_t{0};

    struct Object {
        bool used = false;
        std::array<char, 32> name{};
        std::array<std::uint8_t, ObjectCapacity> data{};
        std::size_t size = 0;
    };

    struct Sink final : ByteSink {
        std::array<std::uint8_t, ObjectCapacity> data{};
        std::size_t size = 0;
        bool failed = false;
        const std::size_t* failAfter = nullptr;
        void reset() {
            size = 0;
            failed = false;
        }
        bool write(const std::uint8_t* bytes, std::size_t n) override {
            if (failed || n > ObjectCapacity - size ||
                (failAfter != nullptr && *failAfter != NoFault && size + n > *failAfter)) {
                failed = true;
                return false;
            }
            std::memcpy(data.data() + size, bytes, n);
            size += n;
            return true;
        }
    };

    struct Source final : ByteSource {
        const Object* object = nullptr;
        std::size_t failFrom = NoFault;
        std::size_t size() const override { return object == nullptr ? 0 : object->size; }
        bool read(std::size_t offset, std::uint8_t* dst, std::size_t n) const override {
            if (object == nullptr || offset > object->size || n > object->size - offset) return false;
            if (failFrom != NoFault && offset + n > failFrom) return false;
            std::memcpy(dst, object->data.data() + offset, n);
            return true;
        }
    };

    static void copyName(std::array<char, 32>& dst, const char* name) {
        dst.fill('\0');
        for (std::size_t i = 0; i < 31 && name[i] != '\0'; ++i) dst[i] = name[i];
    }

    Object* find(const char* name) {
        for (Object& o : objects_) {
            if (o.used && std::strncmp(o.name.data(), name, 32) == 0) return &o;
        }
        return nullptr;
    }
    Object* freeObject() {
        for (Object& o : objects_) {
            if (!o.used) return &o;
        }
        return nullptr;
    }

    std::array<Object, MaxObjects> objects_{};
    Sink sink_{};
    Source source_{};
    std::array<char, 32> pendingName_{};
    bool writing_ = false;
    std::size_t failWriteAfter_ = NoFault;
    bool failCommit_ = false;
    std::size_t failReadFrom_ = NoFault;
    std::uint64_t commits_ = 0;

public:
    MemoryStorage() { sink_.failAfter = &failWriteAfter_; }
    MemoryStorage(const MemoryStorage&) = delete;
    MemoryStorage& operator=(const MemoryStorage&) = delete;
};

} // namespace databreaks
