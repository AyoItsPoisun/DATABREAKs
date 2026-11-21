
#pragma once

// DATABREAKs Engine 029 - audio I/O boundary (target abstraction layer).
//
// The core renders blocks of double-precision StereoFrames
// (AudioPipeline::render). A real codec consumes interleaved integer or
// float samples in blocks whose size and format the TARGET decides (the
// FM-1's codec, rate and block size are not documented). This header is
// the boundary between the two:
//
//   AudioStreamConfig     rate, block size, sample format (validated);
//   AudioRenderCallback   what a device calls once per block (e.g. from a
//                         DMA-complete interrupt) to get interleaved audio;
//   AudioDevice           a device that owns the stream and calls the
//                         callback (a target adapter implements it; host
//                         implementation: platform/host/offline_audio_device.h);
//   PipelineRenderer<P>   the core-side callback: renders an AudioPipeline
//                         in chunks of at most ChunkFrames into a fixed
//                         StereoFrame scratch and converts to the format.
//
// Conversion (identical to the WAV writers of the render demos, so a
// device stream equals the demo files bit for bit):
//   Int16:   lround(clamp(x, -1, 1) * 32767)
//   Int32:   llround(clamp(x, -1, 1) * 2147483647)
//   Float32: (float)clamp(x, -1, 1)
// Interleaving: L, R, L, R, ...
//
// Real-time rules: renderBlock() never allocates, locks or blocks. Chunking
// a device block into smaller render() calls is invisible in the output
// because the engine is block-size independent (tested for 1..4096 frames);
// commands still apply at render() call boundaries, as before.

#include "../core/audio/audio_engine.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace databreaks {

enum class SampleFormat : std::uint8_t { Int16, Int32, Float32 };

inline std::size_t bytesPerSample(SampleFormat f) {
    return f == SampleFormat::Int16 ? 2 : 4;
}

struct AudioStreamConfig {
    int sampleRate = SAMPLE_RATE;
    std::size_t blockFrames = 256;
    SampleFormat format = SampleFormat::Int16;
    int channels = 2;

    // The core runs at SAMPLE_RATE (compile-time 48 kHz) and is stereo.
    bool valid() const {
        return sampleRate == SAMPLE_RATE && channels == 2 && blockFrames >= 1 &&
               blockFrames <= 8192 &&
               (format == SampleFormat::Int16 || format == SampleFormat::Int32 ||
                format == SampleFormat::Float32);
    }
    std::size_t blockBytes() const {
        return blockFrames * static_cast<std::size_t>(channels) * bytesPerSample(format);
    }
};

class AudioRenderCallback {
public:
    virtual ~AudioRenderCallback() = default;
    // Fills `frames` interleaved stereo frames of `format` into `out`.
    // Real-time: must not allocate, lock or block.
    virtual void renderBlock(void* out, std::size_t frames, SampleFormat format) = 0;
};

class AudioDevice {
public:
    virtual ~AudioDevice() = default;
    virtual const AudioStreamConfig& config() const = 0;
    // Starts calling `callback` once per block; false if the stream cannot
    // run (invalid config, device error).
    virtual bool start(AudioRenderCallback& callback) = 0;
    virtual void stop() = 0;
};

namespace audio_convert {

inline std::int16_t toInt16(double x) {
    return static_cast<std::int16_t>(std::lround(clampAudio(x) * 32767.0));
}
inline std::int32_t toInt32(double x) {
    return static_cast<std::int32_t>(std::llround(clampAudio(x) * 2147483647.0));
}
inline float toFloat32(double x) { return static_cast<float>(clampAudio(x)); }

// Converts `frames` StereoFrames to interleaved samples at out + 2 * offset.
inline void convert(const StereoFrame* in, std::size_t frames, void* out, std::size_t offset,
                    SampleFormat format) {
    switch (format) {
    case SampleFormat::Int16: {
        std::int16_t* o = static_cast<std::int16_t*>(out) + 2 * offset;
        for (std::size_t i = 0; i < frames; ++i) {
            o[2 * i] = toInt16(in[i].left);
            o[2 * i + 1] = toInt16(in[i].right);
        }
        break;
    }
    case SampleFormat::Int32: {
        std::int32_t* o = static_cast<std::int32_t*>(out) + 2 * offset;
        for (std::size_t i = 0; i < frames; ++i) {
            o[2 * i] = toInt32(in[i].left);
            o[2 * i + 1] = toInt32(in[i].right);
        }
        break;
    }
    case SampleFormat::Float32: {
        float* o = static_cast<float*>(out) + 2 * offset;
        for (std::size_t i = 0; i < frames; ++i) {
            o[2 * i] = toFloat32(in[i].left);
            o[2 * i + 1] = toFloat32(in[i].right);
        }
        break;
    }
    }
}

}  // namespace audio_convert

// Core-side render callback for an AudioPipeline-like type `P` (anything
// with render(StereoFrame*, size_t) that ADDS into the buffer, as
// AudioPipeline does). Owns a fixed scratch of ChunkFrames StereoFrames
// (16 bytes each).
template <typename P, std::size_t ChunkFrames = 256>
class PipelineRenderer final : public AudioRenderCallback {
public:
    static_assert(ChunkFrames >= 1, "need a chunk");
    explicit PipelineRenderer(P& pipeline) : pipeline_(pipeline) {}

    void renderBlock(void* out, std::size_t frames, SampleFormat format) override {
        std::size_t done = 0;
        while (done < frames) {
            const std::size_t n = frames - done < ChunkFrames ? frames - done : ChunkFrames;
            for (std::size_t i = 0; i < n; ++i) scratch_[i] = StereoFrame{};
            pipeline_.render(scratch_.data(), n);
            audio_convert::convert(scratch_.data(), n, out, done, format);
            done += n;
        }
        frames_ += frames;
    }

    std::uint64_t renderedFrames() const { return frames_; }

private:
    P& pipeline_;
    std::array<StereoFrame, ChunkFrames> scratch_{};
    std::uint64_t frames_ = 0;
};

} // namespace databreaks
