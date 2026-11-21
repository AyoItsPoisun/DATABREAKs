
#pragma once

// DATABREAKs Engine 029 - host AudioDevice that runs offline.
//
// Models a codec stream on the host: it calls the AudioRenderCallback once
// per block of config().blockFrames frames into a fixed internal buffer
// (as a DMA half-buffer interrupt would) and hands each finished block to
// a BlockSink (e.g. to write a file or compare in a test). run() drives a
// given number of frames; nothing is real-time here.
//
// No allocation: the block buffer is a fixed array of MaxBlockBytes.

#include "../audio_io.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace databreaks {

class BlockSink {
public:
    virtual ~BlockSink() = default;
    virtual void consume(const void* interleaved, std::size_t frames, SampleFormat format) = 0;
};

template <std::size_t MaxBlockBytes = 8192 * 2 * 4>
class OfflineAudioDevice final : public AudioDevice {
public:
    explicit OfflineAudioDevice(const AudioStreamConfig& config) : config_(config) {}

    const AudioStreamConfig& config() const override { return config_; }

    bool start(AudioRenderCallback& callback) override {
        if (!config_.valid() || config_.blockBytes() > MaxBlockBytes) return false;
        callback_ = &callback;
        return true;
    }
    void stop() override { callback_ = nullptr; }

    // Runs `frames` frames as whole blocks (a last partial block is
    // rendered at its own length). Returns false if not started.
    bool run(std::size_t frames, BlockSink* sink = nullptr) {
        if (callback_ == nullptr) return false;
        std::size_t done = 0;
        while (done < frames) {
            const std::size_t n =
                frames - done < config_.blockFrames ? frames - done : config_.blockFrames;
            callback_->renderBlock(buffer_.data(), n, config_.format);
            if (sink != nullptr) sink->consume(buffer_.data(), n, config_.format);
            done += n;
            ++blocks_;
        }
        return true;
    }

    std::uint64_t blockCount() const { return blocks_; }

private:
    AudioStreamConfig config_;
    AudioRenderCallback* callback_ = nullptr;
    alignas(8) std::array<std::uint8_t, MaxBlockBytes> buffer_{};
    std::uint64_t blocks_ = 0;
};

} // namespace databreaks
