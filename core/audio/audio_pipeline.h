
#pragma once

// DATABREAKs Engine 021 - unified host-side audio pipeline (C++17).
//
// One rendering path for all sound sources and the master output:
//
//   sources (added into the bus buffer, each at its own level)
//     drums: DrumEngine -> SampleVoiceManager sums voices -> x drumLevel
//     (future: synth layers, effects returns, sequencer-driven sources)
//         |
//         v
//   master: StereoMixer (existing Engine 003 class)
//           x masterLevel, then hard clip to [-1, 1] per channel
//
// Gain staging for a drum voice:
//   sample x pad volume x velocity curve (DrumPadBank)
//          x release/steal fade (SampleVoiceManager)
//          x drumLevel (applied once, to the summed drum voices)
//          x masterLevel (applied once, to the whole mix) -> clip
// Each gain is applied exactly once. Left and right always get the same
// gains (no pan stage yet).
//
// The pipeline does not own a scratch buffer and never splits a block, so
// any block size works and DrumEngine's frame-accurate queued events keep
// their exact timing.
//
// Threading: the same model as DrumEngine. render()/mixSources()/
// applyMaster() and all configuration run on ONE audio thread;
// drums().post*() may be called from ONE other thread.
//
// Real-time: no heap allocation, locks, I/O or exceptions in render(),
// mixSources() or applyMaster().
//
// This is not FM-1 firmware and has not been run on FM-1 hardware.

#include "audio_engine.h"
#include "drum_engine.h"
#include "mixer.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace databreaks {

template <std::size_t DrumVoices = 16, std::size_t DrumEvents = 256>
class AudioPipeline {
public:
    using Drums = DrumEngine<DrumVoices, DrumEvents>;

    AudioPipeline() {
        // Unity master by default so that levels are easy to reason about.
        // (StereoMixer's own default is 0.8.)
        master_.setMasterVolume(1.0);
    }

    // ---- Sources -------------------------------------------------------------

    // The drum source: configure pads, trigger, post events through this.
    Drums& drums() { return drums_; }
    const Drums& drums() const { return drums_; }

    // ---- Levels (audio thread) -----------------------------------------------

    // Drum bus level, [0, 2]. Non-finite values are ignored.
    void setDrumLevel(double level) { drums_.setOutputLevel(level); }
    double drumLevel() const { return drums_.outputLevel(); }

    // Master level, [0, 2] (StereoMixer clamps). Non-finite values ignored.
    void setMasterLevel(double level) {
        if (std::isfinite(level)) {
            master_.setMasterVolume(level);
        }
    }
    double masterLevel() const { return master_.getMasterVolume(); }

    // ---- Rendering (audio thread) ----------------------------------------------

    // Final output. ADDS every source into `output` (existing contents, e.g.
    // audio mixed earlier into the same bus, are kept and summed), then runs
    // the master stage over the whole buffer once. Call it once per block as
    // the last stage. The caller clears the buffer for a fresh mix.
    void render(StereoFrame* output, std::size_t frameCount) {
        if (output == nullptr || frameCount == 0) {
            return;
        }
        mixSources(output, frameCount);
        applyMaster(output, frameCount);
    }

    // Adds every source into `output` at its own level, without the master
    // stage (for callers that master the bus themselves). Existing contents
    // are never overwritten or rescaled.
    void mixSources(StereoFrame* output, std::size_t frameCount) {
        if (output == nullptr || frameCount == 0) {
            return;
        }
        drums_.processBlock(output, frameCount);
    }

    // Master stage, in place: non-finite samples become 0 (counted), then
    // StereoMixer applies the master level and clips to [-1, 1]. Samples
    // that needed clipping are counted.
    void applyMaster(StereoFrame* output, std::size_t frameCount) {
        if (output == nullptr) {
            return;
        }
        const double level = master_.getMasterVolume();
        for (std::size_t i = 0; i < frameCount; ++i) {
            StereoFrame& frame = output[i];
            if (!std::isfinite(frame.left)) {
                frame.left = 0.0;
                ++invalidSamples_;
            }
            if (!std::isfinite(frame.right)) {
                frame.right = 0.0;
                ++invalidSamples_;
            }
            if (std::abs(frame.left * level) > 1.0) {
                ++clippedSamples_;
            }
            if (std::abs(frame.right * level) > 1.0) {
                ++clippedSamples_;
            }
            frame = master_.process(frame);
        }
    }

    // ---- Diagnostics (audio thread) ----------------------------------------------

    // Channel samples the master clip had to limit (|x * master| > 1).
    std::uint64_t clippedSampleCount() const { return clippedSamples_; }
    // Channel samples that were NaN/inf on entry to the master stage.
    std::uint64_t invalidSampleCount() const { return invalidSamples_; }

    void resetDiagnostics() {
        clippedSamples_ = 0;
        invalidSamples_ = 0;
    }

private:
    Drums drums_;
    StereoMixer master_;
    std::uint64_t clippedSamples_ = 0;
    std::uint64_t invalidSamples_ = 0;
};

} // namespace databreaks
