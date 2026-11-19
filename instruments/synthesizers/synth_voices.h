
#pragma once

// DATABREAKs Engine 022 - stateful synthesizer voices (host-side C++17).
//
// The original MonoSynth, FMSynth and PolySynth (Engines 002-004) are pure
// functions of absolute note time. That design cannot render arbitrary
// blocks, retrigger, steal or release a note early without a jump (findings
// B1-B4 in docs/PROGRESS_REPORT.md). Those classes are kept unchanged so the
// Engine 002-004 reference renders stay byte-identical.
//
// The voices below reuse the same parameter structs (synth_parameters.h) and
// the same oscillator, FM and filter formulas, but keep their state between
// samples:
//   * phase accumulators in cycles [0, 1)        -> fixes B4;
//   * a stateful ADSR whose release starts at once from the current level,
//     and whose attack starts from the current level -> fixes B2 and B3;
//   * retriggering from the current level with continuous phase -> removes
//     the B1 click mechanism (a new note no longer cuts the old one to 0).
// For a note that is held past attack + decay and then released, the output
// matches the original classes sample for sample (tested in engine_022).
//
// Real-time: no allocation, locks or I/O. One sin() per oscillator per
// sample (plus one exp() per sample for the mono filter), as in the
// original classes.

#include "../../core/audio/audio_engine.h"
#include "synth_parameters.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace databreaks {

namespace synth_detail {

inline double finiteOr(double value, double fallback) {
    return std::isfinite(value) ? value : fallback;
}

// Advance a phase in cycles and keep it in [0, 1).
inline void advancePhase(double& phase, double increment) {
    phase += increment;
    if (phase >= 1.0 || phase < 0.0) {
        phase -= std::floor(phase);
    }
}

} // namespace synth_detail

// Sanitise parameters: non-finite fields fall back to their defaults and
// values are clamped to safe ranges. Same minimum stage length (1 ms) and
// sustain range as calculateEnvelope().
inline ADSRParameters sanitized(const ADSRParameters& in) {
    using synth_detail::finiteOr;
    const ADSRParameters d;
    ADSRParameters out;
    out.attack = std::clamp(finiteOr(in.attack, d.attack), 0.001, 30.0);
    out.decay = std::clamp(finiteOr(in.decay, d.decay), 0.001, 30.0);
    out.sustain = std::clamp(finiteOr(in.sustain, d.sustain), 0.0, 1.0);
    out.release = std::clamp(finiteOr(in.release, d.release), 0.001, 30.0);
    return out;
}

inline MonoParameters sanitized(const MonoParameters& in) {
    using synth_detail::finiteOr;
    const MonoParameters d;
    MonoParameters out;
    out.oscillatorMix =
        std::clamp(finiteOr(in.oscillatorMix, d.oscillatorMix), 0.0, 2.0);
    out.subMix = std::clamp(finiteOr(in.subMix, d.subMix), 0.0, 2.0);
    out.filterCutoff =
        std::clamp(finiteOr(in.filterCutoff, d.filterCutoff), 20.0, 20000.0);
    out.filterEnvelopeAmount = std::clamp(
        finiteOr(in.filterEnvelopeAmount, d.filterEnvelopeAmount),
        -20000.0, 20000.0);
    out.volume = std::clamp(finiteOr(in.volume, d.volume), 0.0, 2.0);
    out.envelope = sanitized(in.envelope);
    return out;
}

inline FMParameters sanitized(const FMParameters& in) {
    using synth_detail::finiteOr;
    const FMParameters d;
    FMParameters out;
    out.modulationRatio =
        std::clamp(finiteOr(in.modulationRatio, d.modulationRatio), 0.0, 32.0);
    out.modulationDepth =
        std::clamp(finiteOr(in.modulationDepth, d.modulationDepth), 0.0, 50.0);
    out.volume = std::clamp(finiteOr(in.volume, d.volume), 0.0, 2.0);
    out.envelope = sanitized(in.envelope);
    return out;
}

inline PolyParameters sanitized(const PolyParameters& in) {
    using synth_detail::finiteOr;
    const PolyParameters d;
    PolyParameters out;
    out.detune = std::clamp(finiteOr(in.detune, d.detune), 0.0, 0.5);
    out.voiceVolume =
        std::clamp(finiteOr(in.voiceVolume, d.voiceVolume), 0.0, 2.0);
    out.stereoWidth =
        std::clamp(finiteOr(in.stereoWidth, d.stereoWidth), 0.0, 1.0);
    out.volume = std::clamp(finiteOr(in.volume, d.volume), 0.0, 2.0);
    out.envelope = sanitized(in.envelope);
    return out;
}

// ---------------------------------------------------------------------------
// Stateful linear ADSR.
//
// Shapes match calculateEnvelope(): linear attack to 1 (1/attack per second),
// linear decay to sustain, hold, linear release lasting `release` seconds.
// Differences (the fixes):
//   * noteOff() starts the release immediately, from the current level, in
//     any stage (B2, B3);
//   * noteOn() during an active note restarts the attack from the current
//     level instead of from 0 (no jump on retrigger).
class AdsrEnvelope {
public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    void setParameters(const ADSRParameters& parameters) {
        const ADSRParameters p = sanitized(parameters);
        attackFrames_ = p.attack * SAMPLE_RATE;
        decayFrames_ = p.decay * SAMPLE_RATE;
        releaseFrames_ = p.release * SAMPLE_RATE;
        sustain_ = p.sustain;
    }

    void noteOn() {
        start_ = level_;
        stage_ = Stage::Attack;
        position_ = 0.0;
    }

    void noteOff() {
        if (stage_ == Stage::Idle || stage_ == Stage::Release) {
            return;
        }
        start_ = level_;
        stage_ = Stage::Release;
        position_ = 0.0;
    }

    void reset() {
        stage_ = Stage::Idle;
        level_ = 0.0;
        position_ = 0.0;
    }

    // Level for the current sample, then advance by one sample.
    double next() {
        switch (stage_) {
        case Stage::Idle:
            level_ = 0.0;
            return 0.0;

        case Stage::Attack: {
            const double length = (1.0 - start_) * attackFrames_;
            if (position_ >= length) {
                stage_ = Stage::Decay;
                position_ -= length;
                return next();
            }
            level_ = start_ + position_ / attackFrames_;
            break;
        }

        case Stage::Decay:
            if (position_ >= decayFrames_) {
                stage_ = Stage::Sustain;
                return next();
            }
            level_ = 1.0 - (position_ / decayFrames_) * (1.0 - sustain_);
            break;

        case Stage::Sustain:
            level_ = sustain_;
            return level_;

        case Stage::Release:
            if (position_ >= releaseFrames_) {
                reset();
                return 0.0;
            }
            level_ = start_ * (1.0 - position_ / releaseFrames_);
            break;
        }

        position_ += 1.0;
        return level_;
    }

    bool active() const { return stage_ != Stage::Idle; }
    bool releasing() const { return stage_ == Stage::Release; }
    Stage stage() const { return stage_; }
    double level() const { return level_; }

private:
    Stage stage_ = Stage::Idle;
    double level_ = 0.0;
    double start_ = 0.0;
    double position_ = 0.0;
    double attackFrames_ = 0.01 * SAMPLE_RATE;
    double decayFrames_ = 0.15 * SAMPLE_RATE;
    double releaseFrames_ = 0.20 * SAMPLE_RATE;
    double sustain_ = 0.65;
};

// ---------------------------------------------------------------------------
// Common voice state: pitch, velocity gain and envelope. `slot` and
// `slotCount` let a voice place itself (used by PolyVoice for pan/detune).
struct VoiceCore {
    AdsrEnvelope envelope;
    double frequency = 440.0;  // Hz before pitch bend
    double velocityGain = 1.0; // velocity / 127
    std::size_t slot = 0;
    std::size_t slotCount = 1;
};

// Mono voice: MonoSynth's sine + sub-octave sine through a one-pole low-pass
// whose cutoff follows the envelope. Output is centred (L = R).
class MonoVoice {
public:
    using Params = MonoParameters;

    void setParameters(const Params& parameters) {
        params_ = sanitized(parameters);
        core.envelope.setParameters(params_.envelope);
    }

    void reset() {
        core.envelope.reset();
        phase_ = 0.0;
        subPhase_ = 0.0;
        filterState_ = 0.0;
    }

    StereoFrame tick(double pitchRatio) {
        const double env = core.envelope.next();
        const double f = core.frequency * pitchRatio;

        const double input =
            std::sin(2.0 * PI * phase_) * params_.oscillatorMix +
            std::sin(2.0 * PI * subPhase_) * params_.subMix;

        double cutoff = params_.filterCutoff +
                        env * params_.filterEnvelopeAmount;
        cutoff = std::clamp(cutoff, 20.0, SAMPLE_RATE * 0.45);
        const double coefficient =
            1.0 - std::exp(-2.0 * PI * cutoff / SAMPLE_RATE);
        filterState_ += coefficient * (input - filterState_);

        synth_detail::advancePhase(phase_, f / SAMPLE_RATE);
        synth_detail::advancePhase(subPhase_, 0.5 * f / SAMPLE_RATE);

        const double v =
            filterState_ * env * params_.volume * core.velocityGain;
        return {v, v};
    }

    VoiceCore core;

private:
    Params params_;
    double phase_ = 0.0;
    double subPhase_ = 0.0;
    double filterState_ = 0.0;
};

// FM voice: FMSynth's two-operator FM, modulation depth following the
// envelope. Output is centred (L = R).
class FmVoice {
public:
    using Params = FMParameters;

    void setParameters(const Params& parameters) {
        params_ = sanitized(parameters);
        core.envelope.setParameters(params_.envelope);
    }

    void reset() {
        core.envelope.reset();
        carrierPhase_ = 0.0;
        modulatorPhase_ = 0.0;
    }

    StereoFrame tick(double pitchRatio) {
        const double env = core.envelope.next();
        const double f = core.frequency * pitchRatio;

        const double modulator = std::sin(2.0 * PI * modulatorPhase_);
        const double v =
            std::sin(2.0 * PI * carrierPhase_ +
                     params_.modulationDepth * env * modulator) *
            env * params_.volume * core.velocityGain;

        synth_detail::advancePhase(carrierPhase_, f / SAMPLE_RATE);
        synth_detail::advancePhase(modulatorPhase_,
                                   f * params_.modulationRatio / SAMPLE_RATE);
        return {v, v};
    }

    VoiceCore core;

private:
    Params params_;
    double carrierPhase_ = 0.0;
    double modulatorPhase_ = 0.0;
};

// Poly voice: one PolySynth voice. Even slots are detuned down, odd slots
// up, and slot i of N is panned to (i / (N - 1) - 0.5) * width + 0.5, which
// is PolySynth's pan law for N = 4.
class PolyVoice {
public:
    using Params = PolyParameters;

    void setParameters(const Params& parameters) {
        params_ = sanitized(parameters);
        core.envelope.setParameters(params_.envelope);
    }

    void reset() {
        core.envelope.reset();
        phase_ = 0.0;
    }

    StereoFrame tick(double pitchRatio) {
        const double env = core.envelope.next();
        const double detune =
            (core.slot % 2 == 0) ? -params_.detune : params_.detune;
        const double f = core.frequency * (1.0 + detune) * pitchRatio;

        const double sample = std::sin(2.0 * PI * phase_);
        synth_detail::advancePhase(phase_, f / SAMPLE_RATE);

        double pan = core.slotCount > 1
            ? static_cast<double>(core.slot) /
                  static_cast<double>(core.slotCount - 1)
            : 0.5;
        pan = std::clamp((pan - 0.5) * params_.stereoWidth + 0.5, 0.0, 1.0);

        const double v = sample * env * params_.voiceVolume *
                         params_.volume * core.velocityGain;
        return {v * (1.0 - pan), v * pan};
    }

    VoiceCore core;

private:
    Params params_;
    double phase_ = 0.0;
};

} // namespace databreaks
