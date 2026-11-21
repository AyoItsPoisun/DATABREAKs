
#pragma once

// DATABREAKs Engine 021-025 - unified host-side audio pipeline (C++17).
//
// One rendering path for all sound sources and the master output:
//
//   sources (added into the bus buffer, each at its own level)
//     drums: DrumEngine -> SampleVoiceManager sums voices -> x drumLevel
//     mono:  MonoSynthEngine (1 voice)  -> sum -> x monoLevel   (Engine 022)
//     fm:    FmSynthEngine (4 voices)   -> sum -> x fmLevel     (Engine 022)
//     poly:  PolySynthEngine (8 voices) -> sum -> x polyLevel   (Engine 022)
//     (future: sequencer-driven sources)
//         |
//         v
//   effects (Engine 023, in place on the summed bus, in chain order):
//     EQ -> Compressor -> SoftClipper -> Tape -> Distortion -> Reverb
//     (all bypassed by default; Tape added in Engine 024 via insert())
//         |
//         v
//   master: StereoMixer (existing Engine 003 class)
//           x masterLevel, then hard clip to [-1, 1] per channel
//
// Gain staging for a synth voice (Engine 022):
//   oscillator x params.volume (x voiceVolume for poly) x velocity / 127
//              x envelope x source level (once, summed voices) x master
//
// Gain staging for a drum voice:
//   sample x pad volume x velocity curve (DrumPadBank)
//          x release/steal fade (SampleVoiceManager)
//          x drumLevel (applied once, to the summed drum voices)
//          x masterLevel (applied once, to the whole mix) -> clip
// Each gain is applied exactly once. Left and right always get the same
// gains (no pan stage yet).
//
// The pipeline does not own a scratch buffer. Drums render the whole block
// (DrumEngine applies its own queued events at their frames). Synths render
// in segments between their queued note events, so posted synth events are
// frame-accurate too. Every source renders sample by sample with carried
// state, so any block size gives identical audio.
//
// Threading: render()/mixSources()/applyMaster(), all configuration and the
// direct synth calls (mono().noteOn() ...) run on ONE audio thread.
// drums().post*() may be called from ONE other thread, and the synth
// post*() functions from ONE other thread (each queue is single-producer;
// the same producer thread may feed both).
//
// Real-time: no heap allocation, locks, I/O or exceptions in render(),
// mixSources() or applyMaster().
//
// This is not FM-1 firmware and has not been run on FM-1 hardware.

#include "audio_engine.h"
#include "drum_engine.h"
#include "mixer.h"
#include "spsc_ring.h"
#include "synth_event.h"
#include "../sequencer/sequencer.h"
#include "effects/compressor.h"
#include "effects/distortion.h"
#include "effects/effect_chain.h"
#include "effects/equalizer.h"
#include "effects/reverb.h"
#include "effects/soft_clipper.h"
#include "effects/tape_effect.h"
#include "../../instruments/synthesizers/synth_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace databreaks {

// SynthTarget, SynthEventType and SynthEvent: see synth_event.h.

// FmVoices / PolyVoices (Engine 031) let a target resource profile choose
// the synth voice counts; the defaults are the existing host engine
// (AudioPipeline<16> is unchanged).
template <std::size_t DrumVoices = 16,
          std::size_t DrumEvents = 256,
          std::size_t SynthEvents = 256,
          std::size_t FmVoices = 4,
          std::size_t PolyVoices = 8>
class AudioPipeline {
public:
    using Drums = DrumEngine<DrumVoices, DrumEvents>;
    using Mono = MonoSynthEngine;
    using Fm = FmSynthEngine<FmVoices>;
    using Poly = PolySynthEngine<PolyVoices>;
    using SynthQueue = SpscRing<SynthEvent, SynthEvents>;

    AudioPipeline() {
        // Unity master by default so that levels are easy to reason about.
        // (StereoMixer's own default is 0.8.)
        master_.setMasterVolume(1.0);

        // Engine 023 effects chain. Every effect starts fully bypassed, so
        // a pipeline that never enables one renders exactly as in
        // Engine 022 (bit-exact).
        effects_.append(&eq_);
        effects_.append(&compressor_);
        effects_.append(&softClipper_);
        effects_.append(&distortion_);
        effects_.append(&reverb_);
        // Engine 024: tape goes after the soft clipper, through the chain's
        // insertion point (the Engine 023 order of the others is unchanged).
        effects_.insert(effects_.indexOf(&softClipper_) + 1, &tape_);
        for (std::size_t i = 0; i < effects_.size(); ++i) {
            effects_.slot(i)->setBypassed(true, true);
        }
    }

    // ---- Effects (Engine 023, audio thread) --------------------------------

    Equalizer& eq() { return eq_; }
    Compressor& compressor() { return compressor_; }
    SoftClipper& softClipper() { return softClipper_; }
    Distortion& distortion() { return distortion_; }
    Reverb& reverb() { return reverb_; }
    TapeEffect& tape() { return tape_; }  // Engine 024

    // The ordered chain (insert() is the hook for later stages, e.g. tape).
    EffectChain<8>& effects() { return effects_; }
    const EffectChain<8>& effects() const { return effects_; }

    // ---- Sources -------------------------------------------------------------

    // The drum source: configure pads, trigger, post events through this.
    Drums& drums() { return drums_; }
    const Drums& drums() const { return drums_; }

    // The synth sources (Engine 022): parameters, direct note calls (audio
    // thread only) and per-source level.
    Mono& mono() { return mono_; }
    const Mono& mono() const { return mono_; }
    Fm& fm() { return fm_; }
    const Fm& fm() const { return fm_; }
    Poly& poly() { return poly_; }
    const Poly& poly() const { return poly_; }

    // ---- Levels (audio thread) -----------------------------------------------

    // Drum bus level, [0, 2]. Non-finite values are ignored.
    void setDrumLevel(double level) { drums_.setOutputLevel(level); }
    double drumLevel() const { return drums_.outputLevel(); }

    // Synth source levels, [0, 2]; non-finite values are ignored.
    void setMonoLevel(double level) { mono_.setLevel(level); }
    double monoLevel() const { return mono_.level(); }
    void setFmLevel(double level) { fm_.setLevel(level); }
    double fmLevel() const { return fm_.level(); }
    void setPolyLevel(double level) { poly_.setLevel(level); }
    double polyLevel() const { return poly_.level(); }

    // ---- Synth events (producer thread) -------------------------------------
    //
    // Queue a note event for the next render()/mixSources() call, applied at
    // frameOffset (clamped to that block, FIFO order, like drum events).
    // Returns false if the event is invalid (note outside 0..127) or the
    // queue is full (dropped and counted in droppedSynthEventCount()).

    bool postNoteOn(SynthTarget target, int note, int velocity,
                    std::size_t frameOffset = 0) {
        if (note < 0 || note > 127) {
            return false;
        }
        return synthQueue_.push({SynthEventType::NoteOn, target, note,
                                 velocity, 0.0, frameOffset});
    }

    bool postNoteOff(SynthTarget target, int note,
                     std::size_t frameOffset = 0) {
        if (note < 0 || note > 127) {
            return false;
        }
        return synthQueue_.push({SynthEventType::NoteOff, target, note, 0,
                                 0.0, frameOffset});
    }

    bool postPitchBend(SynthTarget target, double semitones,
                       std::size_t frameOffset = 0) {
        if (!std::isfinite(semitones)) {
            return false;
        }
        return synthQueue_.push({SynthEventType::PitchBend, target, 0, 0,
                                 semitones, frameOffset});
    }

    bool postAllNotesOff(SynthTarget target, std::size_t frameOffset = 0) {
        return synthQueue_.push({SynthEventType::AllNotesOff, target, 0, 0,
                                 0.0, frameOffset});
    }

    // Safe to read from either thread.
    std::uint32_t droppedSynthEventCount() const {
        return synthQueue_.droppedCount();
    }

    static constexpr std::size_t synthEventCapacity() {
        return SynthQueue::capacity();
    }

    // ---- Scheduled synth events (Engine 025, AUDIO THREAD ONLY) --------------
    //
    // For schedulers running on the audio thread (the sequencer), which
    // must not push into the single-producer queue above. Applied during the
    // next render()/mixSources() call at their exact frameOffset (larger
    // offsets clamp to the last frame, counted), merged with queued synth
    // events in frame order; at the same frame, scheduled events come first,
    // in scheduling order. At most ScheduledSynthCapacity per block; beyond
    // that the call returns false and droppedScheduledSynthCount() grows.

    static constexpr std::size_t ScheduledSynthCapacity = 1024;

    bool scheduleNoteOn(SynthTarget target, int note, int velocity,
                        std::size_t frameOffset) {
        if (note < 0 || note > 127) {
            return false;
        }
        return scheduleSynth({SynthEventType::NoteOn, target, note, velocity,
                              0.0, frameOffset});
    }

    bool scheduleNoteOff(SynthTarget target, int note, std::size_t frameOffset) {
        if (note < 0 || note > 127) {
            return false;
        }
        return scheduleSynth({SynthEventType::NoteOff, target, note, 0, 0.0,
                              frameOffset});
    }

    bool scheduleAllNotesOff(SynthTarget target, std::size_t frameOffset) {
        return scheduleSynth({SynthEventType::AllNotesOff, target, 0, 0, 0.0,
                              frameOffset});
    }

    std::uint64_t processedScheduledSynthCount() const { return processedScheduledSynth_; }
    std::uint64_t droppedScheduledSynthCount() const { return droppedScheduledSynth_; }
    std::uint64_t clampedScheduledSynthCount() const { return clampedScheduledSynth_; }

    // ---- Sequencer (Engine 025) ----------------------------------------------
    //
    // Driven by mixSources(): each block it delivers its events through the
    // scheduled paths above at exact frames. Stopped by default, so the
    // pipeline behaves exactly as before until it is started.
    Sequencer& sequencer() { return sequencer_; }
    const Sequencer& sequencer() const { return sequencer_; }

    // Sequencer events the instruments could not take (scheduled lists full).
    std::uint64_t undeliveredSequencerEventCount() const { return undelivered_; }

    // Audio thread.
    std::uint64_t processedSynthEventCount() const {
        return processedSynthEvents_;
    }

    // Master level, [0, 2] (StereoMixer clamps). Non-finite values ignored.
    void setMasterLevel(double level) {
        if (std::isfinite(level)) {
            master_.setMasterVolume(level);
        }
    }
    double masterLevel() const { return master_.getMasterVolume(); }

    // ---- Rendering (audio thread) ----------------------------------------------

    // Final output. ADDS every source into `output` (existing contents, e.g.
    // audio mixed earlier into the same bus, are kept and summed), runs the
    // effects chain over the whole bus, then the master stage once. Call it
    // once per block as the last stage. The caller clears the buffer for a
    // fresh mix.
    void render(StereoFrame* output, std::size_t frameCount) {
        if (output == nullptr || frameCount == 0) {
            return;
        }
        mixSources(output, frameCount);
        applyEffects(output, frameCount);
        applyMaster(output, frameCount);
    }

    // Runs the effects chain in place (Engine 023). Bypassed effects are
    // skipped entirely.
    void applyEffects(StereoFrame* output, std::size_t frameCount) {
        effects_.process(output, frameCount);
    }

    // Adds every source into `output` at its own level, without the master
    // stage (for callers that master the bus themselves). Existing contents
    // are never overwritten or rescaled.
    void mixSources(StereoFrame* output, std::size_t frameCount) {
        if (output == nullptr || frameCount == 0) {
            return;
        }
        sequencer_.process(frameCount, [this](const SequencerEvent& e) {
            deliver(e);
        });
        drums_.processBlock(output, frameCount);
        mixSynths(output, frameCount);
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
    Mono mono_;
    Fm fm_;
    Poly poly_;
    SynthQueue synthQueue_;
    std::uint64_t processedSynthEvents_ = 0;
    std::array<SynthEvent, ScheduledSynthCapacity> scheduledSynth_{};
    std::size_t scheduledSynthCount_ = 0;
    std::uint64_t processedScheduledSynth_ = 0;
    std::uint64_t droppedScheduledSynth_ = 0;
    std::uint64_t clampedScheduledSynth_ = 0;
    Sequencer sequencer_{SAMPLE_RATE};
    std::uint64_t undelivered_ = 0;

    bool scheduleSynth(const SynthEvent& event) {
        if (scheduledSynthCount_ == ScheduledSynthCapacity) {
            ++droppedScheduledSynth_;
            return false;
        }
        std::size_t i = scheduledSynthCount_;
        while (i > 0 && scheduledSynth_[i - 1].frameOffset > event.frameOffset) {
            scheduledSynth_[i] = scheduledSynth_[i - 1];
            --i;
        }
        scheduledSynth_[i] = event;
        ++scheduledSynthCount_;
        return true;
    }

    void deliver(const SequencerEvent& e) {
        bool ok = false;
        switch (e.kind) {
        case SequencerEvent::Kind::Drum:
            ok = drums_.scheduleTrigger(static_cast<std::size_t>(e.value),
                                        e.velocity, e.offset);
            break;
        case SequencerEvent::Kind::NoteOn:
            ok = scheduleNoteOn(e.target, e.value, e.velocity, e.offset);
            break;
        case SequencerEvent::Kind::NoteOff:
            ok = scheduleNoteOff(e.target, e.value, e.offset);
            break;
        }
        if (!ok) {
            ++undelivered_;
        }
    }
    StereoMixer master_;
    Equalizer eq_;
    Compressor compressor_;
    SoftClipper softClipper_;
    Distortion distortion_;
    Reverb reverb_;
    TapeEffect tape_;
    EffectChain<8> effects_;
    std::uint64_t clippedSamples_ = 0;
    std::uint64_t invalidSamples_ = 0;

    void renderSynths(StereoFrame* output, std::size_t frames) {
        mono_.render(output, frames);
        fm_.render(output, frames);
        poly_.render(output, frames);
    }

    // Applies queued synth events at their frames (FIFO, at most
    // synthEventCapacity() per call), rendering the synths in between.
    // Queued events keep their Engine 022 rule (FIFO, at
    // min(max(offset, previous queued event's frame), n - 1)); scheduled
    // events (Engine 025) are merged in frame order and win ties.
    void mixSynths(StereoFrame* output, std::size_t frameCount) {
        std::size_t position = 0;
        std::size_t queuePosition = 0;
        std::size_t next = 0;
        std::size_t popped = 0;
        SynthEvent held;
        bool hasHeld = false;
        std::size_t heldAt = 0;

        for (;;) {
            if (!hasHeld && popped < SynthQueue::capacity() &&
                synthQueue_.pop(held)) {
                ++popped;
                hasHeld = true;
                heldAt = std::min(std::max(held.frameOffset, queuePosition),
                                  frameCount - 1);
                queuePosition = heldAt;
            }
            const bool hasScheduled = next < scheduledSynthCount_;
            if (!hasScheduled && !hasHeld) {
                break;
            }
            std::size_t scheduledAt = 0;
            if (hasScheduled) {
                scheduledAt = scheduledSynth_[next].frameOffset;
                if (scheduledAt > frameCount - 1) {
                    scheduledAt = frameCount - 1;
                    ++clampedScheduledSynth_;
                }
            }
            const bool takeScheduled =
                hasScheduled && (!hasHeld || scheduledAt <= heldAt);
            const std::size_t at = takeScheduled ? scheduledAt : heldAt;
            if (at > position) {
                renderSynths(output + position, at - position);
                position = at;
            }
            if (takeScheduled) {
                applySynthEvent(scheduledSynth_[next]);
                ++next;
                ++processedScheduledSynth_;
            } else {
                applySynthEvent(held);
                hasHeld = false;
                ++processedSynthEvents_;
            }
        }
        scheduledSynthCount_ = 0;
        renderSynths(output + position, frameCount - position);
    }

    template <typename Engine>
    static void applyTo(Engine& engine, const SynthEvent& event) {
        switch (event.type) {
        case SynthEventType::NoteOn:
            engine.noteOn(event.note, event.velocity);
            break;
        case SynthEventType::NoteOff:
            engine.noteOff(event.note);
            break;
        case SynthEventType::PitchBend:
            engine.setPitchBend(event.bendSemitones);
            break;
        case SynthEventType::AllNotesOff:
            engine.allNotesOff();
            break;
        }
    }

    void applySynthEvent(const SynthEvent& event) {
        switch (event.target) {
        case SynthTarget::Mono:
            applyTo(mono_, event);
            break;
        case SynthTarget::Fm:
            applyTo(fm_, event);
            break;
        case SynthTarget::Poly:
            applyTo(poly_, event);
            break;
        }
    }
};

} // namespace databreaks
