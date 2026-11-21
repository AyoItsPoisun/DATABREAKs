
#pragma once

// DATABREAKs Engine 029 - host StorageBackend on files (HOST ONLY).
//
// Object `name` is the file <directory>/<name>. A write goes to
// <name>.tmp; commitWrite() closes it and renames it over <name>, which
// replaces the previous version atomically on POSIX file systems (a
// failed or aborted write leaves the previous file untouched). Reads use
// fseek/fread at the requested offset (the file is never loaded whole).
//
// This file uses <cstdio> and is the only place the project touches the
// file system; core code must never include it (enforced by the
// core_purity build check).

#include "../storage.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace databreaks {

class FileStorage final : public StorageBackend {
public:
    // `directory` must exist; it is not created. Paths are limited to 511
    // characters.
    explicit FileStorage(const char* directory) {
        std::size_t i = 0;
        for (; directory != nullptr && directory[i] != '\0' && i + 1 < dir_.size(); ++i) dir_[i] = directory[i];
        dir_[i] = '\0';
    }
    ~FileStorage() override {
        abortWrite();
        closeRead();
    }
    FileStorage(const FileStorage&) = delete;
    FileStorage& operator=(const FileStorage&) = delete;

    ByteSink* beginWrite(const char* name) override {
        if (sink_.file != nullptr || !validName(name)) return nullptr;
        if (!path(finalPath_, name, "") || !path(tempPath_, name, ".tmp")) return nullptr;
        sink_.file = std::fopen(tempPath_.data(), "wb");
        sink_.failed = false;
        return sink_.file == nullptr ? nullptr : &sink_;
    }

    bool commitWrite() override {
        if (sink_.file == nullptr) return false;
        const bool closed = std::fclose(sink_.file) == 0;
        sink_.file = nullptr;
        if (!closed || sink_.failed) {
            std::remove(tempPath_.data());
            return false;
        }
        closeRead();  // a reader of the old version must not keep it open
        return std::rename(tempPath_.data(), finalPath_.data()) == 0;
    }

    void abortWrite() override {
        if (sink_.file == nullptr) return;
        std::fclose(sink_.file);
        sink_.file = nullptr;
        std::remove(tempPath_.data());
    }

    const ByteSource* openRead(const char* name) override {
        closeRead();
        std::array<char, 512> p{};
        if (!validName(name) || !path(p, name, "")) return nullptr;
        source_.file = std::fopen(p.data(), "rb");
        if (source_.file == nullptr) return nullptr;
        if (std::fseek(source_.file, 0, SEEK_END) != 0) {
            closeRead();
            return nullptr;
        }
        const long end = std::ftell(source_.file);
        if (end < 0) {
            closeRead();
            return nullptr;
        }
        source_.bytes = static_cast<std::size_t>(end);
        return &source_;
    }

    void closeRead() override {
        if (source_.file != nullptr) std::fclose(source_.file);
        source_.file = nullptr;
        source_.bytes = 0;
    }

private:
    struct Sink final : ByteSink {
        std::FILE* file = nullptr;
        bool failed = false;
        bool write(const std::uint8_t* data, std::size_t n) override {
            if (file == nullptr || failed) return false;
            if (std::fwrite(data, 1, n, file) != n) failed = true;
            return !failed;
        }
    };
    struct Source final : ByteSource {
        std::FILE* file = nullptr;
        std::size_t bytes = 0;
        std::size_t size() const override { return bytes; }
        bool read(std::size_t offset, std::uint8_t* dst, std::size_t n) const override {
            if (file == nullptr || offset > bytes || n > bytes - offset) return false;
            if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) return false;
            return std::fread(dst, 1, n, file) == n;
        }
    };

    bool path(std::array<char, 512>& out, const char* name, const char* suffix) const {
        const int n = std::snprintf(out.data(), out.size(), "%s/%s%s", dir_.data(), name, suffix);
        return n > 0 && static_cast<std::size_t>(n) < out.size();
    }

    std::array<char, 400> dir_{};
    std::array<char, 512> finalPath_{};
    std::array<char, 512> tempPath_{};
    Sink sink_{};
    Source source_{};
};

} // namespace databreaks
