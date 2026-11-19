
#pragma once

// DATABREAKs Engine 023 - feed-forward dynamics compressor (host-side).
//
// Signal path per sample:
//   detector = max(|L|, |R|)                       (stereo-linked peak)
//   envelope: one-pole follower, attack coefficient while rising, release
//             coefficient while falling (time constants: 1 - 1/e reached
//             after `attack` / `release`)
//   level dB = 20 log10(envelope)
//   gain reduction (dB) from a soft-knee curve:
//       below threshold - knee/2  : 0
//       above threshold + knee/2  : (level - threshold) x (1 - 1/ratio)
//       inside the knee           : quadratic blend of the two
//   output = input x 10^((-reduction + makeup) / 20), both channels equally
//
// Threshold, ratio, knee and makeup glide over 20 ms. Wet/dry (parallel
// compression) comes from the common AudioEffect mix.
//
// Memory: a few doubles.

#include "audio_effect.h"

#include <algorithm>
#include <cmath>

namespace databreaks {

class Compressor : public EffectBase<Compressor> {
public:
    Compressor() {
        threshold_.snap(-18.0);
        ratio_.snap(4.0);
        knee_.snap(6.0);
        makeup_.snap(0.0);
        setAttackMs(10.0);
        setReleaseMs(120.0);
    }

    const char* name() const override { return "Compressor"; }

    // Threshold -60..0 dBFS (default -18).
    void setThresholdDb(double db) {
        threshold_.setTarget(validated(db, -60.0, 0.0, threshold_.target()));
    }
    // Ratio 1..20 (default 4). 1 = no compression.
    void setRatio(double ratio) {
        ratio_.setTarget(validated(ratio, 1.0, 20.0, ratio_.target()));
    }
    // Soft knee width 0..24 dB (default 6). 0 = hard knee.
    void setKneeDb(double db) {
        knee_.setTarget(validated(db, 0.0, 24.0, knee_.target()));
    }
    // Attack 0.1..200 ms (default 10).
    void setAttackMs(double ms) {
        attackMs_ = validated(ms, 0.1, 200.0, attackMs_);
        attackCoeff_ = coefficientFor(attackMs_);
    }
    // Release 10..2000 ms (default 120).
    void setReleaseMs(double ms) {
        releaseMs_ = validated(ms, 10.0, 2000.0, releaseMs_);
        releaseCoeff_ = coefficientFor(releaseMs_);
    }
    // Makeup gain -12..+24 dB (default 0).
    void setMakeupDb(double db) {
        makeup_.setTarget(validated(db, -12.0, 24.0, makeup_.target()));
    }

    double thresholdDb() const { return threshold_.target(); }
    double ratio() const { return ratio_.target(); }
    double attackMs() const { return attackMs_; }
    double releaseMs() const { return releaseMs_; }
    double makeupDb() const { return makeup_.target(); }

    // Current gain reduction in dB (>= 0), for metering and tests.
    double gainReductionDb() const { return reductionDb_; }
    double envelope() const { return envelope_; }

    // Static curve: gain reduction (dB) for an input level (dB).
    static double reductionFor(double levelDb, double thresholdDb,
                               double ratio, double kneeDb) {
        const double slope = 1.0 - 1.0 / ratio;
        const double over = levelDb - thresholdDb;
        if (kneeDb > 0.0 && std::abs(over) <= kneeDb / 2.0) {
            const double x = over + kneeDb / 2.0;
            return slope * x * x / (2.0 * kneeDb);
        }
        return over > 0.0 ? slope * over : 0.0;
    }

    StereoFrame tick(const StereoFrame& in) {
        const double threshold = threshold_.next();
        const double ratio = ratio_.next();
        const double knee = knee_.next();
        const double makeup = makeup_.next();

        const double detector = std::max(std::abs(in.left), std::abs(in.right));
        const double coeff = detector > envelope_ ? attackCoeff_ : releaseCoeff_;
        envelope_ += coeff * (detector - envelope_);
        if (envelope_ < 1e-20) {
            envelope_ = 0.0;  // avoid denormals in long silences
        }

        reductionDb_ = envelope_ > 0.0
            ? reductionFor(gainToDb(envelope_), threshold, ratio, knee)
            : 0.0;
        const double gain = dbToGain(makeup - reductionDb_);
        return {in.left * gain, in.right * gain};
    }

protected:
    void resetState() override {
        envelope_ = 0.0;
        reductionDb_ = 0.0;
        threshold_.snapToTarget();
        ratio_.snapToTarget();
        knee_.snapToTarget();
        makeup_.snapToTarget();
    }

private:
    SmoothedValue threshold_;
    SmoothedValue ratio_;
    SmoothedValue knee_;
    SmoothedValue makeup_;
    double attackMs_ = 10.0;
    double releaseMs_ = 120.0;
    double attackCoeff_ = 0.0;
    double releaseCoeff_ = 0.0;
    double envelope_ = 0.0;
    double reductionDb_ = 0.0;

    static double coefficientFor(double ms) {
        return 1.0 - std::exp(-1.0 / (ms * 0.001 * SAMPLE_RATE));
    }
};

} // namespace databreaks
