
#pragma once

// DATABREAKs Engine 025 - musical time units (host-side C++17).
//
// A TICK is 1/960 of a quarter note (PPQ = 960). 960 = 2^6 x 3 x 5, so
// straight notes down to 1/256, triplets and quintuplets are whole ticks
// (a 16th = 240 ticks, a 16th triplet = 160, a 32nd = 120).
//
// Time signature n/d (numerator 1..32, denominator 1, 2, 4, 8, 16 or 32):
//   ticks per beat = PPQ x 4 / d       (a "beat" is one 1/d note)
//   ticks per bar  = n x ticks per beat
// Bar/beat/tick positions here are 0-based (bar 0, beat 0, tick 0 is the
// very start); add 1 to bar and beat for the usual 1-based display.

#include <cstdint>

namespace databreaks {

constexpr std::int64_t TicksPerQuarter = 960;

struct TimeSignature {
    int numerator = 4;
    int denominator = 4;

    static bool valid(int numerator, int denominator) {
        if (numerator < 1 || numerator > 32) {
            return false;
        }
        switch (denominator) {
        case 1: case 2: case 4: case 8: case 16: case 32:
            return true;
        default:
            return false;
        }
    }

    bool valid() const { return valid(numerator, denominator); }

    std::int64_t ticksPerBeat() const {
        return TicksPerQuarter * 4 / denominator;
    }

    std::int64_t ticksPerBar() const {
        return ticksPerBeat() * numerator;
    }
};

struct BarBeatTick {
    std::int64_t bar = 0;
    std::int64_t beat = 0;
    std::int64_t tick = 0;  // within the beat
};

// Absolute tick (>= 0) to bar/beat/tick. Negative ticks map to bar 0.
inline BarBeatTick toBarBeatTick(std::int64_t absoluteTick,
                                 const TimeSignature& ts) {
    if (absoluteTick < 0) {
        absoluteTick = 0;
    }
    const std::int64_t perBar = ts.ticksPerBar();
    const std::int64_t perBeat = ts.ticksPerBeat();
    BarBeatTick out;
    out.bar = absoluteTick / perBar;
    const std::int64_t inBar = absoluteTick % perBar;
    out.beat = inBar / perBeat;
    out.tick = inBar % perBeat;
    return out;
}

// Bar/beat/tick to absolute tick. Beat and tick are not range-checked, so
// e.g. beat 5 in 4/4 simply means bar + 5 beats.
inline std::int64_t toAbsoluteTick(const BarBeatTick& position,
                                   const TimeSignature& ts) {
    return position.bar * ts.ticksPerBar() +
           position.beat * ts.ticksPerBeat() + position.tick;
}

} // namespace databreaks
