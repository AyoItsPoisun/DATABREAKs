
#pragma once

// DATABREAKs Engine 025 - musical clock (host-side C++17).
//
// Maps musical ticks (musical_time.h: 960 per quarter note) to audio
// samples exactly, so long playback does not drift.
//
// Tempo is stored as an integer number of milli-BPM (BPM x 1000, so
// 0.001 BPM resolution; range 20.000..300.000 BPM). With sample rate fs:
//   samples per tick = fs x 60 / (BPM x 960) = N / D,
//   N = fs x 60000,  D = milliBPM x 960            (both integers)
// Within one tempo segment starting at tick T0 whose exact sample time is
// O / D samples (O an integer), tick t has the exact sample time
//   x(t) = (O + (t - T0) x N) / D
// and is DELIVERED at the first sample s with s >= x(t):
//   sampleOfTick(t) = ceil(x(t))
// computed in 128-bit integers. No per-tick rounding is ever accumulated:
// tick 10^9 is placed as exactly as tick 1.
//
// Tempo changes take effect at the first tick boundary at or after the
// current position. The new segment starts there; its origin is the old
// segment's exact time of that tick, rounded once to the new 1/D grid
// (error < 1/D of a sample, i.e. far below 1e-6 samples, once per change).
//
// Sample rates 8000..192000 Hz are accepted (the AudioPipeline runs at
// SAMPLE_RATE = 48000; the clock itself is rate-agnostic and tested at
// 44.1 and 48 kHz).
//
// Engine 026 adds FINE TICKS: 1/1024 of a tick (FinePerTick), used to
// timestamp recorded performances. The same exact formula is used with
// O, T0 and t scaled by 1024, so sampleOfFine(t x 1024) == sampleOfTick(t).
// The spacing of fine ticks is N / (1024 D) samples, below one sample for
// every supported rate and tempo (worst case 192 kHz at 20 BPM: 0.59), so
// every sample s has a fine position f = fineAtSample(s) (the latest fine
// tick at or before s) with sampleOfFine(f) == s exactly: a recording
// replays on its captured sample.
//
// Uses ExactInt128 (exact_int128.h): native __int128 where available,
// otherwise a portable implementation with identical results (Engine 029).
//
// Threading: audio thread only (Sequencer::postTransport() offers a queue
// for start/stop/tempo from one other thread).

#include "exact_int128.h"
#include "musical_time.h"

#include <cmath>
#include <cstdint>

namespace databreaks {

// Exact 128-bit signed integer (Engine 029: exact_int128.h). The native
// __int128 on 64-bit hosts (unchanged behaviour and speed); a portable
// two-word implementation with identical results on 32-bit targets, where
// __int128 does not exist.
using SequencerInt128 = ExactInt128;

// The clock is written against an exact 128-bit integer type `Int`
// (Engine 029 made it a template so the native and the portable integer
// can be compared through the real clock in one test binary). Everything
// uses MusicalClock = BasicMusicalClock<ExactInt128>.
template <typename Int>
class BasicMusicalClock {
public:
    using i128 = Int;

    static constexpr std::int64_t MinMilliBpm = 20000;
    static constexpr std::int64_t MaxMilliBpm = 300000;
    static constexpr int MinSampleRate = 8000;
    static constexpr int MaxSampleRate = 192000;
    static constexpr std::int64_t FinePerTick = 1024;  // Engine 026

    explicit BasicMusicalClock(int sampleRate = 48000) {
        setSampleRate(sampleRate);
    }

    // ---- Configuration -------------------------------------------------------

    // Changes the sample rate and resets the position (a running clock at a
    // different rate has no meaningful continuation). Invalid rates ignored.
    bool setSampleRate(int sampleRate) {
        if (sampleRate < MinSampleRate || sampleRate > MaxSampleRate) {
            return false;
        }
        sampleRate_ = sampleRate;
        reset();
        return true;
    }

    // BPM 20..300; non-finite or out-of-range values are rejected (false).
    // Stored as round(bpm x 1000).
    bool setTempo(double bpm) {
        if (!std::isfinite(bpm)) {
            return false;
        }
        const double milli = std::round(bpm * 1000.0);
        if (milli < static_cast<double>(MinMilliBpm) ||
            milli > static_cast<double>(MaxMilliBpm)) {
            return false;
        }
        return setTempoMilli(static_cast<std::int64_t>(milli));
    }

    bool setTempoMilli(std::int64_t milliBpm) {
        if (milliBpm < MinMilliBpm || milliBpm > MaxMilliBpm) {
            return false;
        }
        if (milliBpm == milliBpm_) {
            return true;
        }
        // New segment at the first tick delivered at or after position_.
        const std::int64_t t = firstTickAtOrAfter(position_);
        const i128 oldNum = originNum_ + static_cast<i128>(t - originTick_) * n();
        const std::int64_t newD = milliBpm * TicksPerQuarter;
        // Exact old time oldNum / d() expressed on the new grid, rounded.
        const i128 scaled = oldNum * newD;
        const i128 halfD = d() / 2;
        originNum_ = (scaled + halfD) / d();
        originTick_ = t;
        milliBpm_ = milliBpm;
        return true;
    }

    bool setTimeSignature(int numerator, int denominator) {
        if (!TimeSignature::valid(numerator, denominator)) {
            return false;
        }
        timeSignature_ = {numerator, denominator};
        return true;
    }

    // ---- Transport ---------------------------------------------------------------

    void start() { playing_ = true; }
    void stop() { playing_ = false; }

    // Back to sample 0 / tick 0 (keeps tempo, signature and play state).
    void reset() {
        position_ = 0;
        originTick_ = 0;
        originNum_ = 0;
    }

    // Moves the play position forward by `frames` samples (only while
    // playing). Returns the number of samples advanced.
    std::int64_t advance(std::int64_t frames) {
        if (!playing_ || frames <= 0) {
            return 0;
        }
        position_ += frames;
        return frames;
    }

    // ---- Queries ---------------------------------------------------------------------

    bool playing() const { return playing_; }
    int sampleRate() const { return sampleRate_; }
    double tempo() const { return static_cast<double>(milliBpm_) / 1000.0; }
    std::int64_t tempoMilli() const { return milliBpm_; }
    const TimeSignature& timeSignature() const { return timeSignature_; }

    // Next sample to be rendered (samples since reset).
    std::int64_t samplePosition() const { return position_; }

    // Sample at which `tick` is delivered (ticks before the current tempo
    // segment are extrapolated with the current tempo).
    std::int64_t sampleOfTick(std::int64_t tick) const {
        const i128 num = originNum_ + static_cast<i128>(tick - originTick_) * n();
        return static_cast<std::int64_t>(ceilDiv(num, d()));
    }

    // First tick delivered at or after `sample`.
    std::int64_t firstTickAtOrAfter(std::int64_t sample) const {
        // Smallest t with (O + (t - T0) N) / D > sample - 1, i.e.
        // (t - T0) N > (sample - 1) D - O.
        const i128 bound = static_cast<i128>(sample - 1) * d() - originNum_;
        return originTick_ + static_cast<std::int64_t>(floorDiv(bound, n()) + 1);
    }

    // ---- Fine ticks (Engine 026): 1/1024 tick ----------------------------------

    // Sample at which fine tick `fine` is delivered (first sample at or
    // after its exact time).
    std::int64_t sampleOfFine(std::int64_t fine) const {
        const i128 num = originNum_ * FinePerTick +
                         static_cast<i128>(fine - originTick_ * FinePerTick) * n();
        return static_cast<std::int64_t>(ceilDiv(num, d() * FinePerTick));
    }

    // First fine tick delivered at or after `sample`; for this f,
    // sampleOfFine(f) == sample (fine spacing is below one sample).
    std::int64_t firstFineAtOrAfter(std::int64_t sample) const {
        const i128 bound = static_cast<i128>(sample - 1) * d() * FinePerTick -
                           originNum_ * FinePerTick;
        return originTick_ * FinePerTick +
               static_cast<std::int64_t>(floorDiv(bound, n()) + 1);
    }

    // Fine position of sample `sample`: the LATEST fine tick whose exact
    // time is <= sample. It is delivered at `sample` too (fine spacing is
    // below one sample), i.e. sampleOfFine(fineAtSample(s)) == s, and it is
    // the fine tick closest to the sample's exact time without exceeding it
    // (fineAtSample(0) == 0; a sample on an exact tick time maps to that
    // tick x 1024). This is how recorded input is timestamped (Engine 026).
    std::int64_t fineAtSample(std::int64_t sample) const {
        return firstFineAtOrAfter(sample + 1) - 1;
    }

    // Musical position (ticks, rounded down) at the current sample.
    std::int64_t tickPosition() const {
        const i128 num = static_cast<i128>(position_) * d() - originNum_;
        return originTick_ + static_cast<std::int64_t>(floorDiv(num, n()));
    }

    BarBeatTick barBeatTick() const {
        return toBarBeatTick(tickPosition(), timeSignature_);
    }

    // Exact samples per tick as a double (for reporting only).
    double samplesPerTick() const {
        // n() <= 1.15e10 and d() <= 2.9e8 fit int64 and convert exactly.
        return static_cast<double>(static_cast<std::int64_t>(n())) /
               static_cast<double>(static_cast<std::int64_t>(d()));
    }

private:
    int sampleRate_ = 48000;
    std::int64_t milliBpm_ = 120000;
    TimeSignature timeSignature_{};
    bool playing_ = false;
    std::int64_t position_ = 0;
    std::int64_t originTick_ = 0;
    i128 originNum_ = 0;  // exact sample time of originTick_, x D

    i128 n() const { return static_cast<i128>(sampleRate_) * 60000; }
    i128 d() const { return static_cast<i128>(milliBpm_) * TicksPerQuarter; }

    static i128 floorDiv(i128 a, i128 b) {
        i128 q = a / b;
        if ((a % b != 0) && ((a < 0) != (b < 0))) {
            --q;
        }
        return q;
    }

    static i128 ceilDiv(i128 a, i128 b) { return -floorDiv(-a, b); }
};

using MusicalClock = BasicMusicalClock<SequencerInt128>;

} // namespace databreaks
