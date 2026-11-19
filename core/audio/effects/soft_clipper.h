
#pragma once

// DATABREAKs Engine 023 - soft clipper (host-side C++17).
//
// A musical saturation stage, separate from the master stage's hard safety
// clip (StereoMixer). Per channel, after the input drive:
//
//   knee k = threshold x ceiling, ceiling c
//   |x| <= k : y = x                                  (exactly linear)
//   |x| >  k : y = sign(x) (k + (c - k) tanh((|x| - k) / (c - k)))
//
// Value and slope are continuous at the knee (tanh'(0) = 1), the curve
// approaches the ceiling smoothly, and |y| <= c always: the output can never
// exceed the ceiling, whatever the drive. (Mathematically |y| < c; in double
// precision tanh rounds to exactly 1 for large arguments, so |y| can equal
// c.)
//
// Drive and threshold glide over 20 ms. Memory: a few doubles.

#include "audio_effect.h"

#include <cmath>

namespace databreaks {

class SoftClipper : public EffectBase<SoftClipper> {
public:
    SoftClipper() {
        driveDb_.snap(0.0);
        threshold_.snap(0.7);
    }

    const char* name() const override { return "SoftClipper"; }

    // Input drive 0..+24 dB (default 0).
    void setDriveDb(double db) {
        driveDb_.setTarget(validated(db, 0.0, 24.0, driveDb_.target()));
    }
    // Knee as a fraction of the ceiling, 0.1..0.99 (default 0.7).
    void setThreshold(double fraction) {
        threshold_.setTarget(
            validated(fraction, 0.1, 0.99, threshold_.target()));
    }
    // Output ceiling 0.1..1.0 (default 1.0, i.e. 0 dBFS).
    void setCeiling(double ceiling) {
        ceiling_ = validated(ceiling, 0.1, 1.0, ceiling_);
    }

    double driveDb() const { return driveDb_.target(); }
    double threshold() const { return threshold_.target(); }
    double ceiling() const { return ceiling_; }

    static double shape(double x, double knee, double ceiling) {
        const double ax = std::abs(x);
        if (ax <= knee) {
            return x;
        }
        const double span = ceiling - knee;
        const double y = knee + span * std::tanh((ax - knee) / span);
        return x < 0.0 ? -y : y;
    }

    StereoFrame tick(const StereoFrame& in) {
        const double drive = dbToGain(driveDb_.next());
        const double knee = threshold_.next() * ceiling_;
        return {shape(in.left * drive, knee, ceiling_),
                shape(in.right * drive, knee, ceiling_)};
    }

protected:
    void resetState() override {
        driveDb_.snapToTarget();
        threshold_.snapToTarget();
    }

private:
    SmoothedValue driveDb_;
    SmoothedValue threshold_;
    double ceiling_ = 1.0;
};

} // namespace databreaks
