
#pragma once

// DATABREAKs Engine 023 - common effect interface (host-side C++17).
//
// Every effect processes interleaved StereoFrame buffers IN PLACE, for any
// block size, without allocation, locks or I/O. All of them share:
//   * bypass with a short (20 ms) linear crossfade, so switching never
//     clicks. Once fully bypassed, process() returns without touching the
//     buffer (bit-exact pass-through) and without running the effect;
//   * a wet/dry mix in [0, 1], smoothed the same way. At mix 1 the output
//     is exactly the processed signal; at mix 0 exactly the input;
//   * reset(), which clears all internal state (filters, envelopes, delay
//     lines) and snaps smoothed parameters to their targets.
// An effect that comes out of full bypass is reset first (state cleared,
// parameters and mix snapped to their set values), so stale state (an old
// reverb tail, a held envelope) never bursts back in and the result does
// not depend on what the effect did before it was bypassed.
//
// Parameter setters ignore non-finite values and clamp to documented
// ranges. Parameters that could click are smoothed over 20 ms
// (SmoothedValue), per sample, so results do not depend on block size.
//
// Threading: setters, reset() and process() must all run on ONE thread
// (the audio thread). There is no control queue yet.

#include "../audio_engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace databreaks {

inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

inline double gainToDb(double gain) {
    return 20.0 * std::log10(std::max(gain, 1e-12));
}

// Clamp a finite value; keep `current` if the new value is not finite.
inline double validated(double value, double lo, double hi, double current) {
    return std::isfinite(value) ? std::clamp(value, lo, hi) : current;
}

// Linear parameter ramp with a fixed length in frames. Deterministic and
// independent of block size: one next() per sample.
class SmoothedValue {
public:
    static constexpr std::size_t DefaultRampFrames =
        static_cast<std::size_t>(SAMPLE_RATE) / 50;  // 20 ms

    SmoothedValue() : SmoothedValue(0.0) {}

    explicit SmoothedValue(double initial,
                           std::size_t rampFrames = DefaultRampFrames)
        : current_(initial), target_(initial), ramp_(rampFrames) {}

    void setTarget(double target) {
        if (target == target_ && remaining_ == 0) {
            return;
        }
        target_ = target;
        remaining_ = ramp_;
        step_ = (target_ - current_) / static_cast<double>(ramp_);
    }

    void snap(double value) {
        current_ = target_ = value;
        remaining_ = 0;
        step_ = 0.0;
    }

    void snapToTarget() { snap(target_); }

    double next() {
        if (remaining_ > 0) {
            current_ += step_;
            if (--remaining_ == 0) {
                current_ = target_;
            }
        }
        return current_;
    }

    double current() const { return current_; }
    double target() const { return target_; }
    bool smoothing() const { return remaining_ > 0; }

private:
    double current_;
    double target_;
    double step_ = 0.0;
    std::size_t ramp_;
    std::size_t remaining_ = 0;
};

// Polymorphic interface used by EffectChain (one virtual call per block).
class AudioEffect {
public:
    virtual ~AudioEffect() = default;

    virtual void process(StereoFrame* buffer, std::size_t frames) = 0;
    virtual const char* name() const = 0;

    // Clears internal state and snaps smoothed parameters.
    void reset() {
        resetState();
        mix_.snapToTarget();
        active_.snapToTarget();
        fullyBypassed_ = active_.current() == 0.0;
    }

    // Bypass with a 20 ms crossfade. `immediate` skips the fade.
    void setBypassed(bool bypassed, bool immediate = false) {
        bypassTarget_ = bypassed;
        if (immediate) {
            active_.snap(bypassed ? 0.0 : 1.0);
            fullyBypassed_ = bypassed;
            if (!bypassed) {
                resetState();       // start clean ...
                mix_.snapToTarget();  // ... with the mix already in place
            }
            return;
        }
        if (!bypassed && fullyBypassed_) {
            resetState();         // start clean, parameters at their targets,
            mix_.snapToTarget();  // mix included; only `active` fades in
            fullyBypassed_ = false;
        }
        active_.setTarget(bypassed ? 0.0 : 1.0);
    }

    void setEnabled(bool enabled, bool immediate = false) {
        setBypassed(!enabled, immediate);
    }

    bool bypassed() const { return bypassTarget_; }
    bool fullyBypassed() const { return fullyBypassed_; }

    // Wet/dry: 0 = input only, 1 = effect only. Non-finite ignored.
    void setMix(double mix) {
        mix_.setTarget(validated(mix, 0.0, 1.0, mix_.target()));
    }

    double mix() const { return mix_.target(); }

protected:
    virtual void resetState() = 0;

    SmoothedValue mix_{1.0};
    SmoothedValue active_{1.0};
    bool bypassTarget_ = false;
    bool fullyBypassed_ = false;
};

// CRTP helper: the per-sample effect is Derived::tick(StereoFrame), inlined
// into one block loop that also applies bypass and wet/dry.
template <typename Derived>
class EffectBase : public AudioEffect {
public:
    void process(StereoFrame* buffer, std::size_t frames) final {
        if (buffer == nullptr || frames == 0 || fullyBypassed_) {
            return;
        }
        Derived& self = static_cast<Derived&>(*this);
        for (std::size_t i = 0; i < frames; ++i) {
            const StereoFrame dry = buffer[i];
            const StereoFrame wet = self.tick(dry);
            const double m = mix_.next() * active_.next();
            if (m == 1.0) {
                buffer[i] = wet;
            } else if (m == 0.0) {
                buffer[i] = dry;
            } else {
                buffer[i].left = dry.left * (1.0 - m) + wet.left * m;
                buffer[i].right = dry.right * (1.0 - m) + wet.right * m;
            }
        }
        if (bypassTarget_ && !active_.smoothing() &&
            active_.current() == 0.0) {
            fullyBypassed_ = true;
        }
    }
};

} // namespace databreaks
