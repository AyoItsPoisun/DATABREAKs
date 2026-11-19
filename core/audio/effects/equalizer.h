
#pragma once

// DATABREAKs Engine 023 - three-band equalizer (host-side C++17).
//
// Low shelf -> mid peak -> high shelf -> output gain, per channel.
// Each band is a second-order IIR (biquad) in transposed direct form II,
// with coefficients from the widely published "Audio EQ Cookbook" design
// equations (R. Bristow-Johnson), implemented here from those formulas.
//
// Stability: for the clamped ranges below (frequencies 20 Hz .. 0.45 x the
// sample rate, Q > 0, shelf slope 1) these designs have both poles inside
// the unit circle. tests/audio/engine_023 checks the poles for the range
// corners.
//
// Smoothing: gains (dB), frequencies (in log2 Hz) and Q glide linearly over
// 20 ms; while any of them is gliding, coefficients are recomputed every
// sample (cost only during changes). A band at 0 dB gain is skipped, so the
// EQ at its default settings passes audio through bit-exactly.
//
// Memory: 3 bands x 2 channels x 2 state values + coefficients
// (well under 1 KB).

#include "audio_effect.h"

#include <array>
#include <cmath>

namespace databreaks {

struct BiquadCoefficients {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

class Biquad {
public:
    double process(double x, const BiquadCoefficients& c) {
        const double y = c.b0 * x + s1_;
        s1_ = c.b1 * x - c.a1 * y + s2_;
        s2_ = c.b2 * x - c.a2 * y;
        return y;
    }

    void reset() { s1_ = s2_ = 0.0; }

private:
    double s1_ = 0.0;
    double s2_ = 0.0;
};

enum class BiquadShape { LowShelf, Peak, HighShelf };

// Cookbook designs; shelves use slope S = 1.
inline BiquadCoefficients designBiquad(BiquadShape shape, double frequency,
                                       double gainDb, double q) {
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * PI * frequency / SAMPLE_RATE;
    const double cosw = std::cos(w0);
    const double sinw = std::sin(w0);

    double b0, b1, b2, a0, a1, a2;
    if (shape == BiquadShape::Peak) {
        const double alpha = sinw / (2.0 * q);
        b0 = 1.0 + alpha * a;
        b1 = -2.0 * cosw;
        b2 = 1.0 - alpha * a;
        a0 = 1.0 + alpha / a;
        a1 = -2.0 * cosw;
        a2 = 1.0 - alpha / a;
    } else {
        const double alpha = sinw / 2.0 * std::sqrt(2.0);  // S = 1
        const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;
        if (shape == BiquadShape::LowShelf) {
            b0 = a * ((a + 1.0) - (a - 1.0) * cosw + twoSqrtAAlpha);
            b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosw);
            b2 = a * ((a + 1.0) - (a - 1.0) * cosw - twoSqrtAAlpha);
            a0 = (a + 1.0) + (a - 1.0) * cosw + twoSqrtAAlpha;
            a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosw);
            a2 = (a + 1.0) + (a - 1.0) * cosw - twoSqrtAAlpha;
        } else {
            b0 = a * ((a + 1.0) + (a - 1.0) * cosw + twoSqrtAAlpha);
            b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosw);
            b2 = a * ((a + 1.0) + (a - 1.0) * cosw - twoSqrtAAlpha);
            a0 = (a + 1.0) - (a - 1.0) * cosw + twoSqrtAAlpha;
            a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosw);
            a2 = (a + 1.0) - (a - 1.0) * cosw - twoSqrtAAlpha;
        }
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

class Equalizer : public EffectBase<Equalizer> {
public:
    static constexpr double MinGainDb = -18.0;
    static constexpr double MaxGainDb = 18.0;
    static constexpr double MaxFrequency = SAMPLE_RATE * 0.45;

    Equalizer() {
        for (std::size_t b = 0; b < 3; ++b) {
            gainDb_[b].snap(0.0);
        }
        logFreq_[0].snap(std::log2(120.0));
        logFreq_[1].snap(std::log2(1000.0));
        logFreq_[2].snap(std::log2(8000.0));
        q_.snap(0.7);
        outputDb_.snap(0.0);
        updateCoefficients();
    }

    const char* name() const override { return "Equalizer"; }

    // Low shelf: 20..1000 Hz (default 120), gain -18..+18 dB (default 0).
    void setLow(double frequency, double gainDb) {
        setBand(0, frequency, 20.0, 1000.0, gainDb);
    }
    // Mid peak: 100..10000 Hz (default 1000), gain +/-18 dB, Q 0.2..8 (0.7).
    void setMid(double frequency, double gainDb, double q = 0.7) {
        setBand(1, frequency, 100.0, 10000.0, gainDb);
        q_.setTarget(validated(q, 0.2, 8.0, q_.target()));
    }
    // High shelf: 1000..18000 Hz (default 8000), gain +/-18 dB.
    void setHigh(double frequency, double gainDb) {
        setBand(2, frequency, 1000.0, 18000.0, gainDb);
    }
    // Output gain -18..+18 dB (default 0).
    void setOutputGainDb(double db) {
        outputDb_.setTarget(validated(db, MinGainDb, MaxGainDb,
                                      outputDb_.target()));
    }

    double bandGainDb(std::size_t band) const {
        return band < 3 ? gainDb_[band].target() : 0.0;
    }
    double bandFrequency(std::size_t band) const {
        return band < 3 ? std::exp2(logFreq_[band].target()) : 0.0;
    }
    const BiquadCoefficients& coefficients(std::size_t band) const {
        return coeffs_[band < 3 ? band : 2];
    }

    StereoFrame tick(const StereoFrame& in) {
        bool changed = q_.smoothing();
        for (std::size_t b = 0; b < 3; ++b) {
            changed = changed || gainDb_[b].smoothing() ||
                      logFreq_[b].smoothing();
        }
        if (changed) {
            q_.next();
            for (std::size_t b = 0; b < 3; ++b) {
                gainDb_[b].next();
                logFreq_[b].next();
            }
            updateCoefficients();
        }
        double l = in.left;
        double r = in.right;
        for (std::size_t b = 0; b < 3; ++b) {
            if (gainDb_[b].current() != 0.0) {
                l = left_[b].process(l, coeffs_[b]);
                r = right_[b].process(r, coeffs_[b]);
            } else {
                // Flat band: skipped (identity); clear its state so it
                // restarts cleanly when its gain moves away from 0 dB.
                left_[b].reset();
                right_[b].reset();
            }
        }
        const double outDb = outputDb_.next();
        if (outDb != 0.0) {
            const double g = dbToGain(outDb);
            l *= g;
            r *= g;
        }
        return {l, r};
    }

protected:
    void resetState() override {
        for (std::size_t b = 0; b < 3; ++b) {
            left_[b].reset();
            right_[b].reset();
            gainDb_[b].snapToTarget();
            logFreq_[b].snapToTarget();
        }
        q_.snapToTarget();
        outputDb_.snapToTarget();
        updateCoefficients();
    }

private:
    std::array<Biquad, 3> left_{};
    std::array<Biquad, 3> right_{};
    std::array<BiquadCoefficients, 3> coeffs_{};
    std::array<SmoothedValue, 3> gainDb_{};
    std::array<SmoothedValue, 3> logFreq_{};
    SmoothedValue q_;
    SmoothedValue outputDb_;

    void setBand(std::size_t band, double frequency, double lo, double hi,
                 double gainDb) {
        const double f = validated(frequency, lo, std::min(hi, MaxFrequency),
                                   std::exp2(logFreq_[band].target()));
        logFreq_[band].setTarget(std::log2(f));
        gainDb_[band].setTarget(
            validated(gainDb, MinGainDb, MaxGainDb, gainDb_[band].target()));
    }

    void updateCoefficients() {
        coeffs_[0] = designBiquad(BiquadShape::LowShelf,
                                  std::exp2(logFreq_[0].current()),
                                  gainDb_[0].current(), 0.7071);
        coeffs_[1] = designBiquad(BiquadShape::Peak,
                                  std::exp2(logFreq_[1].current()),
                                  gainDb_[1].current(), q_.current());
        coeffs_[2] = designBiquad(BiquadShape::HighShelf,
                                  std::exp2(logFreq_[2].current()),
                                  gainDb_[2].current(), 0.7071);
    }
};

} // namespace databreaks
