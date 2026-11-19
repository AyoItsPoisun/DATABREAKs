
#pragma once

// DATABREAKs Engine 023 - distortion with three characters (host-side).
//
// Original waveshapers (x already multiplied by the drive gain d):
//   Soft:       y = tanh(x)
//               symmetric, odd harmonics, gentle.
//   Asymmetric: x >= 0 : y = 1 - exp(-x)
//               x <  0 : y = -(1 - exp(1.6 x)) / 1.6
//               the negative half saturates sooner and lower, adding even
//               harmonics (and a DC offset, removed below).
//   Fuzz:       v = 4 x + 0.2 (bias), clipped to [-1, 1], then the cubic
//               soft corner c(v) = 1.5 v - 0.5 v^3: near-square, gated
//               feel. Shifted and scaled, y = (c(v) - c(0.2)) / (1 + c(0.2)),
//               so that y(0) = 0 (silence stays silent, no thump when the
//               effect or mode is switched) and y stays in [-1, 0.55].
// Every shape is bounded to [-1, 1] and maps 0 to 0. A DC blocker
// (one-pole high-pass at about 10 Hz) follows, so the signal-dependent DC
// of the asymmetric shapes cannot build up. Then the output level is
// applied.
//
// Changing the character crossfades from the old shape to the new one over
// 20 ms. Drive and output level glide over 20 ms.
// No oversampling: high drive aliases (documented limitation).
//
// Memory: a few doubles per channel.

#include "audio_effect.h"

#include <algorithm>
#include <cmath>

namespace databreaks {

enum class DistortionMode { Soft = 0, Asymmetric = 1, Fuzz = 2 };

class Distortion : public EffectBase<Distortion> {
public:
    Distortion() {
        driveDb_.snap(12.0);
        outputDb_.snap(-6.0);
        modeFade_.snap(1.0);
    }

    const char* name() const override { return "Distortion"; }

    void setMode(DistortionMode mode) {
        if (mode == mode_) {
            return;
        }
        previousMode_ = mode_;
        mode_ = mode;
        modeFade_.snap(0.0);
        modeFade_.setTarget(1.0);
    }
    // Drive 0..+36 dB (default 12).
    void setDriveDb(double db) {
        driveDb_.setTarget(validated(db, 0.0, 36.0, driveDb_.target()));
    }
    // Output level -36..+6 dB (default -6).
    void setOutputDb(double db) {
        outputDb_.setTarget(validated(db, -36.0, 6.0, outputDb_.target()));
    }

    DistortionMode mode() const { return mode_; }
    double driveDb() const { return driveDb_.target(); }
    double outputDb() const { return outputDb_.target(); }

    static double shape(DistortionMode mode, double x) {
        switch (mode) {
        case DistortionMode::Soft:
            return std::tanh(x);
        case DistortionMode::Asymmetric:
            return x >= 0.0 ? 1.0 - std::exp(-x)
                            : -(1.0 - std::exp(1.6 * x)) / 1.6;
        case DistortionMode::Fuzz: {
            const double v = std::clamp(4.0 * x + 0.2, -1.0, 1.0);
            constexpr double c0 = 1.5 * 0.2 - 0.5 * 0.2 * 0.2 * 0.2;  // 0.296
            return (1.5 * v - 0.5 * v * v * v - c0) / (1.0 + c0);
        }
        }
        return x;
    }

    StereoFrame tick(const StereoFrame& in) {
        const double drive = dbToGain(driveDb_.next());
        const double out = dbToGain(outputDb_.next());
        const double fade = modeFade_.next();
        return {out * dcBlock(shaped(in.left * drive, fade), dcL_),
                out * dcBlock(shaped(in.right * drive, fade), dcR_)};
    }

protected:
    void resetState() override {
        dcL_ = {};
        dcR_ = {};
        driveDb_.snapToTarget();
        outputDb_.snapToTarget();
        modeFade_.snapToTarget();
    }

private:
    struct DcState {
        double x1 = 0.0;
        double y1 = 0.0;
    };

    // One-pole DC blocker: y = x - x1 + R y1, R for ~10 Hz at 48 kHz.
    static constexpr double DcPole = 1.0 - 2.0 * PI * 10.0 / SAMPLE_RATE;

    DistortionMode mode_ = DistortionMode::Soft;
    DistortionMode previousMode_ = DistortionMode::Soft;
    SmoothedValue driveDb_;
    SmoothedValue outputDb_;
    SmoothedValue modeFade_;
    DcState dcL_;
    DcState dcR_;

    double shaped(double x, double fade) const {
        const double now = shape(mode_, x);
        if (fade >= 1.0) {
            return now;
        }
        return shape(previousMode_, x) * (1.0 - fade) + now * fade;
    }

    static double dcBlock(double x, DcState& s) {
        double y = x - s.x1 + DcPole * s.y1;
        if (std::abs(y) < 1e-20) {
            y = 0.0;  // no denormals in silence
        }
        s.x1 = x;
        s.y1 = y;
        return y;
    }
};

} // namespace databreaks
