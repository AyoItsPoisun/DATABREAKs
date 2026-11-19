
#pragma once

// DATABREAKs Engine 023 - stereo feedback-delay-network reverb (host-side).
//
// Original design:
//   input  : mid (L + R) / 2 -> DC blocker (~10 Hz) -> 4 series allpass
//            diffusers (fixed lengths 142/107/379/277 samples, g 0.6);
//            side (L - R) / 2 -> DC blocker, injected undiffused, so
//            stereo (even fully out-of-phase) input still excites the tank
//   tank   : 8 delay lines (base lengths 1427..3517 samples, mutually
//            prime, scaled by room size 0.25..1.0). Each line's output goes
//            through a one-pole damping low-pass and a decay gain
//            g_i = 10^(-3 L_i / (RT60 x fs)), then all lines are mixed by a
//            Householder matrix H = I - (2/8) 1 1^T and written back with
//            the input added: diffused mid with signs + - + - ..., side
//            with signs + + - - ... (orthogonal patterns).
//   output : two different +/- sign patterns over the 8 line outputs give
//            left and right, scaled by 1/sqrt(8); width blends mid/side;
//            out = dry x input + wet x reverb.
//
// Stability: H is orthogonal (energy preserving), every decay gain is < 1
// and the damping filter has gain <= 1 at every frequency, so the loop is
// strictly contractive for every parameter value: no unstable feedback.
// DC: blocked at the input; the loop gain at DC is g_i < 1.
// Silence: state values below 1e-25 are flushed to 0, so a finished tail
// becomes exact digital silence (also avoids denormals).
// Room size changes glide over 20 ms with linearly interpolated delay
// reads (no clicks; a brief pitch bend of the tail while gliding).
//
// Memory (all fixed std::array storage inside the object, no allocation):
//   tank  : (19414 + 8 x 4) doubles = 19446 x 8 B = 155,568 bytes
//   diffusers : 905 doubles = 7,240 bytes
//   total : about 163 KB in double precision (about 81 KB if ported to
//   float). The FM-1's RAM is undocumented; this may need shrinking there.

#include "audio_effect.h"

#include <array>
#include <cmath>
#include <cstddef>

namespace databreaks {

namespace reverb_detail {

constexpr std::size_t Lines = 8;
constexpr std::array<std::size_t, Lines> BaseLengths = {
    1427, 1777, 1993, 2237, 2539, 2803, 3121, 3517};
constexpr std::array<std::size_t, 4> DiffuserLengths = {142, 107, 379, 277};

constexpr std::size_t lineCapacity(std::size_t i) {
    return BaseLengths[i] + 4;  // max length + interpolation margin
}

constexpr std::size_t tankSamples() {
    std::size_t total = 0;
    for (std::size_t i = 0; i < Lines; ++i) total += lineCapacity(i);
    return total;
}

constexpr std::size_t diffuserSamples() {
    std::size_t total = 0;
    for (std::size_t len : DiffuserLengths) total += len;
    return total;
}

} // namespace reverb_detail

class Reverb : public EffectBase<Reverb> {
public:
    static constexpr std::size_t Lines = reverb_detail::Lines;
    static constexpr std::array<std::size_t, Lines> BaseLengths =
        reverb_detail::BaseLengths;
    static constexpr std::array<std::size_t, 4> DiffuserLengths =
        reverb_detail::DiffuserLengths;
    static constexpr double MinSize = 0.25;
    static constexpr double MaxSize = 1.0;

    static constexpr std::size_t lineCapacity(std::size_t i) {
        return reverb_detail::lineCapacity(i);
    }
    static constexpr std::size_t tankSamples() {
        return reverb_detail::tankSamples();
    }
    static constexpr std::size_t diffuserSamples() {
        return reverb_detail::diffuserSamples();
    }
    static constexpr std::size_t delayMemoryBytes() {
        return (tankSamples() + diffuserSamples()) * sizeof(double);
    }

    Reverb() {
        size_.snap(0.7);
        decay_.snap(1.8);
        damping_.snap(0.4);
        wet_.snap(0.3);
        dry_.snap(1.0);
        width_.snap(1.0);
        std::size_t offset = 0;
        for (std::size_t i = 0; i < Lines; ++i) {
            lineOffset_[i] = offset;
            offset += lineCapacity(i);
        }
        offset = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            diffuserOffset_[k] = offset;
            offset += DiffuserLengths[k];
        }
        updateGains();
    }

    const char* name() const override { return "Reverb"; }

    // Room size 0.25..1.0 (default 0.7): scales the 8 delay lengths.
    void setRoomSize(double size) {
        size_.setTarget(validated(size, MinSize, MaxSize, size_.target()));
    }
    // Decay time RT60 0.2..10 s (default 1.8).
    void setDecaySeconds(double seconds) {
        decay_.setTarget(validated(seconds, 0.2, 10.0, decay_.target()));
    }
    // High-frequency damping 0..1 (default 0.4).
    void setDamping(double damping) {
        damping_.setTarget(validated(damping, 0.0, 1.0, damping_.target()));
    }
    // Wet level 0..1 (default 0.3), dry level 0..1 (default 1.0).
    void setWetLevel(double level) {
        wet_.setTarget(validated(level, 0.0, 1.0, wet_.target()));
    }
    void setDryLevel(double level) {
        dry_.setTarget(validated(level, 0.0, 1.0, dry_.target()));
    }
    // Stereo width of the reverb 0 (mono) .. 1 (default 1).
    void setWidth(double width) {
        width_.setTarget(validated(width, 0.0, 1.0, width_.target()));
    }

    double roomSize() const { return size_.target(); }
    double decaySeconds() const { return decay_.target(); }
    double damping() const { return damping_.target(); }
    double wetLevel() const { return wet_.target(); }
    double dryLevel() const { return dry_.target(); }
    double width() const { return width_.target(); }

    StereoFrame tick(const StereoFrame& in) {
        if (size_.smoothing() || decay_.smoothing()) {
            size_.next();
            decay_.next();
            updateGains();
        }
        const double damp = 0.9 * damping_.next();
        const double wet = wet_.next();
        const double dry = dry_.next();
        const double width = width_.next();

        // Input: mid (DC blocked, diffused) and side (DC blocked).
        const double mid = 0.5 * (in.left + in.right);
        double x = mid - dcX1_ + DcPole * dcY1_;
        dcX1_ = mid;
        dcY1_ = flush(x);
        x = dcY1_;
        for (std::size_t k = 0; k < 4; ++k) {
            x = allpass(k, x);
        }
        const double sideIn = 0.5 * (in.left - in.right);
        const double side = flush(sideIn - sideX1_ + DcPole * sideY1_);
        sideX1_ = sideIn;
        sideY1_ = side;

        // Tank.
        std::array<double, Lines> y{};
        std::array<double, Lines> u{};
        double sum = 0.0;
        for (std::size_t i = 0; i < Lines; ++i) {
            y[i] = readLine(i, lengths_[i]);
            lowpass_[i] = flush(y[i] + damp * (lowpass_[i] - y[i]));
            u[i] = gains_[i] * lowpass_[i];
            sum += u[i];
        }
        sum *= 2.0 / static_cast<double>(Lines);
        for (std::size_t i = 0; i < Lines; ++i) {
            const double input = ((i % 2 == 0) ? x : -x) +
                                 ((i % 4 < 2) ? side : -side);
            writeLine(i, flush(u[i] - sum + input));
        }

        // Output taps and width.
        double l = 0.0;
        double r = 0.0;
        for (std::size_t i = 0; i < Lines; ++i) {
            l += (LeftSigns[i] ? y[i] : -y[i]);
            r += (RightSigns[i] ? y[i] : -y[i]);
        }
        l *= OutputScale;
        r *= OutputScale;
        const double outMid = 0.5 * (l + r);
        const double outSide = 0.5 * (l - r) * width;

        return {dry * in.left + wet * (outMid + outSide),
                dry * in.right + wet * (outMid - outSide)};
    }

protected:
    void resetState() override {
        tank_.fill(0.0);
        diffusers_.fill(0.0);
        lowpass_.fill(0.0);
        diffuserIndex_.fill(0);
        writeIndex_.fill(0);
        dcX1_ = dcY1_ = 0.0;
        sideX1_ = sideY1_ = 0.0;
        size_.snapToTarget();
        decay_.snapToTarget();
        damping_.snapToTarget();
        wet_.snapToTarget();
        dry_.snapToTarget();
        width_.snapToTarget();
        updateGains();
    }

private:
    static constexpr double DcPole = 1.0 - 2.0 * PI * 10.0 / SAMPLE_RATE;
    static constexpr double DiffuserGain = 0.6;
    static constexpr double OutputScale = 0.35355339059327373;  // 1/sqrt(8)
    static constexpr std::array<bool, Lines> LeftSigns = {
        true, false, true, false, true, false, true, false};
    static constexpr std::array<bool, Lines> RightSigns = {
        true, true, false, false, true, true, false, false};

    std::array<double, reverb_detail::tankSamples()> tank_{};
    std::array<double, reverb_detail::diffuserSamples()> diffusers_{};
    std::array<std::size_t, Lines> lineOffset_{};
    std::array<std::size_t, 4> diffuserOffset_{};
    std::array<std::size_t, 4> diffuserIndex_{};
    std::array<double, Lines> lowpass_{};
    std::array<double, Lines> lengths_{};
    std::array<double, Lines> gains_{};
    std::array<std::size_t, Lines> writeIndex_{};  // per line, < capacity
    double dcX1_ = 0.0;
    double dcY1_ = 0.0;
    double sideX1_ = 0.0;
    double sideY1_ = 0.0;

    SmoothedValue size_;
    SmoothedValue decay_;
    SmoothedValue damping_;
    SmoothedValue wet_;
    SmoothedValue dry_;
    SmoothedValue width_;

    static double flush(double v) { return std::abs(v) < 1e-25 ? 0.0 : v; }

    void updateGains() {
        const double rt60 = decay_.current();
        for (std::size_t i = 0; i < Lines; ++i) {
            lengths_[i] = static_cast<double>(BaseLengths[i]) * size_.current();
            gains_[i] = std::pow(10.0, -3.0 * lengths_[i] / (rt60 * SAMPLE_RATE));
        }
    }

    double readLine(std::size_t i, double length) const {
        const std::size_t cap = lineCapacity(i);
        const std::size_t whole = static_cast<std::size_t>(length);
        const double frac = length - static_cast<double>(whole);
        const std::size_t w = writeIndex_[i];
        const std::size_t i0 = (w + cap - whole) % cap;
        const std::size_t i1 = (w + cap - whole - 1) % cap;
        const double a = tank_[lineOffset_[i] + i0];
        const double b = tank_[lineOffset_[i] + i1];
        return a + (b - a) * frac;
    }

    // Write the next sample and advance this line's own circular index.
    void writeLine(std::size_t i, double value) {
        tank_[lineOffset_[i] + writeIndex_[i]] = value;
        writeIndex_[i] = (writeIndex_[i] + 1) % lineCapacity(i);
    }

    double allpass(std::size_t k, double x) {
        double& stored = diffusers_[diffuserOffset_[k] + diffuserIndex_[k]];
        const double delayed = stored;
        const double v = flush(x + DiffuserGain * delayed);
        stored = v;
        diffuserIndex_[k] = (diffuserIndex_[k] + 1) % DiffuserLengths[k];
        return delayed - DiffuserGain * v;
    }
};

} // namespace databreaks
