
#pragma once

// DATABREAKs Engine 024 - TapeMachine as an AudioEffect (host-side C++17).
//
// TapeEffect owns one TapeMachine (core/audio/tape_machine.h, unchanged
// DSP) by value: no pointers, no shared or temporary objects. Every sample
// goes through TapeMachine::process(), so the wet signal is exactly the
// existing tape DSP (tested bit-for-bit in engine_024). The adapter adds:
//   * the shared effect features: bypass with a 20 ms crossfade (bit-exact
//     when fully bypassed), wet/dry mix, reset;
//   * validation: TapeMachine's setters clamp to [0, 1] but let NaN through;
//     here non-finite values are ignored;
//   * smoothing: the ten continuous controls glide over 20 ms, applied to
//     the machine per sample while gliding (a jump in wow would otherwise
//     move the delay read position by up to ~72 samples at once);
//   * latency-aligned wet/dry: the machine delays its signal by a fixed
//     12 ms (576 frames at 48 kHz) plus wow/flutter modulation, so the dry
//     side of the mix is delayed by the same 576 frames (no comb filter).
//
// Speed and tape type are discrete. TapeMachine switches them instantly,
// which steps the output when saturation is high (the drive changes; up to
// ~0.13 in one sample, measured in engine_024). The adapter instead ducks:
// the tape signal glides to the aligned dry signal over 5 ms, the switch
// happens there, and the tape signal glides back over 5 ms (10 ms in all,
// no step). When fully bypassed, or on reset(), switches apply at once.
//
// State (delay line write position, wow/flutter phases, filters, envelope,
// print-through buffer, hiss generator) lives in the machine and carries
// across blocks; nothing is reset at block boundaries. This is a tape
// *insert* effect: the transport classes (TapeTransport, TapeStreamTransport,
// TapeStreamingEngine) play stored recordings and are not part of it.
//
// Memory: TapeMachine allocates its four buffers once, in its constructor
// (2 x 4,800 + 2 x 12,000 doubles = 268,800 bytes on the heap); the aligned
// dry line is a fixed 576-frame array (9,216 bytes) inside this object.
// Nothing is allocated while processing.

#include "audio_effect.h"
#include "../tape_machine.h"

#include <array>
#include <cstddef>

namespace databreaks {

class TapeEffect : public EffectBase<TapeEffect> {
public:
    static constexpr bool HasLatency = true;

    // TapeMachine's base delay: 0.012 s x 48 kHz.
    static constexpr std::size_t LatencyFrames =
        static_cast<std::size_t>(SAMPLE_RATE) * 12 / 1000;

    // Heap memory TapeMachine allocates in its constructor.
    static constexpr std::size_t machineBufferBytes() {
        return (2 * static_cast<std::size_t>(SAMPLE_RATE / 10) +
                2 * static_cast<std::size_t>(SAMPLE_RATE / 4)) *
               sizeof(double);
    }

    TapeEffect() {
        // TapeMachine's own defaults.
        wow_.snap(0.0);
        flutter_.snap(0.0);
        hiss_.snap(0.0);
        saturation_.snap(0.0);
        headBump_.snap(0.0);
        bias_.snap(0.5);
        compression_.snap(0.0);
        crosstalk_.snap(0.0);
        printThrough_.snap(0.0);
        age_.snap(0.0);
        applyToMachine();
    }

    const char* name() const override { return "Tape"; }
    std::size_t latencyFrames() const override { return LatencyFrames; }

    // ---- Controls (same ranges and meaning as TapeMachine) ------------------

    // Discrete: applied through a 5 ms + 5 ms duck (see above). Setting
    // the current value again does nothing.
    void setSpeed(TapeSpeed speed) {
        if (speed == speed_) {
            return;
        }
        speed_ = speed;
        requestSwitch();
    }
    void setTapeType(TapeType type) {
        if (type == type_) {
            return;
        }
        type_ = type;
        requestSwitch();
    }

    // True while a speed/type switch is ducking the tape signal.
    bool switching() const { return switchPending_ || switchGain_.smoothing(); }
    // All of these: 0..1; non-finite ignored; glide over 20 ms.
    void setWow(double v) { set(wow_, v); }
    void setFlutter(double v) { set(flutter_, v); }
    void setHiss(double v) { set(hiss_, v); }
    void setSaturation(double v) { set(saturation_, v); }
    void setHeadBump(double v) { set(headBump_, v); }
    void setBias(double v) { set(bias_, v); }
    void setCompression(double v) { set(compression_, v); }
    void setCrosstalk(double v) { set(crosstalk_, v); }
    void setPrintThrough(double v) { set(printThrough_, v); }
    void setAge(double v) { set(age_, v); }

    TapeSpeed speed() const { return speed_; }
    TapeType tapeType() const { return type_; }
    double wow() const { return wow_.target(); }
    double flutter() const { return flutter_.target(); }
    double hiss() const { return hiss_.target(); }
    double saturation() const { return saturation_.target(); }
    double headBump() const { return headBump_.target(); }
    double bias() const { return bias_.target(); }
    double compression() const { return compression_.target(); }
    double crosstalk() const { return crosstalk_.target(); }
    double printThrough() const { return printThrough_.target(); }
    double age() const { return age_.target(); }

    // Read-only access to the owned machine.
    const TapeMachine& machine() const { return machine_; }

    // ---- Per-sample processing (called by EffectBase) ----------------------

    StereoFrame tick(const StereoFrame& in) {
        if (gliding_ > 0) {
            --gliding_;
            advanceSmoothing();
            applyToMachine();
        }
        aligned_ = dryLine_[dryIndex_];
        dryLine_[dryIndex_] = in;
        dryIndex_ = (dryIndex_ + 1) % LatencyFrames;
        const StereoFrame wet = machine_.process(in);

        if (!switchPending_ && !switchGain_.smoothing()) {
            return wet;  // exactly TapeMachine's output
        }
        const double g = switchGain_.next();
        if (switchPending_ && g == 0.0) {
            applySwitch();
            switchPending_ = false;
            switchGain_.setTarget(1.0);
        }
        return {aligned_.left + (wet.left - aligned_.left) * g,
                aligned_.right + (wet.right - aligned_.right) * g};
    }

    // The input delayed by LatencyFrames (valid after tick()).
    StereoFrame alignedDry() const { return aligned_; }

protected:
    void resetState() override {
        applySwitch();
        switchPending_ = false;
        switchGain_.snap(1.0);
        machine_.reset();
        dryLine_.fill(StereoFrame{});
        dryIndex_ = 0;
        aligned_ = {};
        for (SmoothedValue* v : all()) {
            v->snapToTarget();
        }
        gliding_ = 0;
        applyToMachine();
    }

private:
    TapeMachine machine_;
    TapeSpeed speed_ = TapeSpeed::IPS_15;
    TapeType type_ = TapeType::Ferric;

    SmoothedValue wow_;
    SmoothedValue flutter_;
    SmoothedValue hiss_;
    SmoothedValue saturation_;
    SmoothedValue headBump_;
    SmoothedValue bias_;
    SmoothedValue compression_;
    SmoothedValue crosstalk_;
    SmoothedValue printThrough_;
    SmoothedValue age_;
    std::size_t gliding_ = 0;  // frames left in which some control glides

    static constexpr std::size_t SwitchRampFrames =
        static_cast<std::size_t>(SAMPLE_RATE) / 200;  // 5 ms
    SmoothedValue switchGain_{1.0, SwitchRampFrames};
    bool switchPending_ = false;

    std::array<StereoFrame, LatencyFrames> dryLine_{};
    std::size_t dryIndex_ = 0;
    StereoFrame aligned_{};

    std::array<SmoothedValue*, 10> all() {
        return {&wow_, &flutter_, &hiss_, &saturation_, &headBump_,
                &bias_, &compression_, &crosstalk_, &printThrough_, &age_};
    }

    void requestSwitch() {
        if (fullyBypassed_) {
            applySwitch();  // nothing audible: switch at once
            return;
        }
        switchPending_ = true;
        switchGain_.setTarget(0.0);
    }

    void applySwitch() {
        machine_.setSpeed(speed_);
        machine_.setTapeType(type_);
    }

    void set(SmoothedValue& value, double v) {
        if (!std::isfinite(v)) {
            return;
        }
        value.setTarget(std::clamp(v, 0.0, 1.0));
        gliding_ = SmoothedValue::DefaultRampFrames;
    }

    void advanceSmoothing() {
        for (SmoothedValue* v : all()) {
            v->next();
        }
    }

    void applyToMachine() {
        machine_.setWow(wow_.current());
        machine_.setFlutter(flutter_.current());
        machine_.setHiss(hiss_.current());
        machine_.setSaturation(saturation_.current());
        machine_.setHeadBump(headBump_.current());
        machine_.setBias(bias_.current());
        machine_.setCompression(compression_.current());
        machine_.setCrosstalk(crosstalk_.current());
        machine_.setPrintThrough(printThrough_.current());
        machine_.setAge(age_.current());
    }
};

} // namespace databreaks
