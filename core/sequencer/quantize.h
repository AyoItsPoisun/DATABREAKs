
#pragma once

// DATABREAKs Engine 027 - non-destructive quantization (host-side C++17).
//
// Quantization is a PLAYBACK TRANSFORM of the recorded track: the stored
// RecordedEvents (Engine 026) are never modified. Every played position is
// recomputed from the raw recorded position and the current settings, so
// changing the grid, strength or swing never compounds, and strength 0
// reproduces the original timing exactly.
//
// All arithmetic is integer, in fine ticks (1/1024 tick, musical_clock.h);
// there is no floating point and nothing accumulates between evaluations.
//
// Grid convention:
//   * Grid step G (ticks): whole 3840, half 1920, quarter 960, eighth 480,
//     sixteenth 240, thirty-second 120, sixty-fourth 60 (960 PPQ, so all
//     exact). Gf = G x 1024 fine ticks.
//   * Grid points are anchored to the LOOP START (recorded positions are
//     loop-relative): point k is at k x Gf, for every k with k x Gf < L
//     (L = loop length in fine ticks). The loop end L is also a grid point;
//     it is position 0 of the next loop. If L is not a multiple of Gf the
//     last cell is shorter.
//   * An event moves to its NEAREST grid point; at an exact tie it moves to
//     the later point (rounding half up, like RecordedEvent::nearestStep).
//
// Swing convention:
//   * swing S = 0..100 (percent). Odd-numbered grid points (k = 1, 3, 5...,
//     counted from the loop start, i.e. the second point of each pair) are
//     delayed by floor(S x Gf / 200) fine ticks; even points never move.
//     S = 0 is straight, S = 50 delays the off-beat by a quarter of a step
//     (pair ratio 62.5 : 37.5), S = 100 by half a step (75 : 25, dotted).
//     A triplet feel is S = 67 (66.67 exactly).
//   * A swung point that would land at or after the loop end does not
//     exist. The loop end itself (point 0 of the next loop) is never swung.
//     Parity restarts at every loop start, so every loop swings the same way.
//
// Strength P = 0..100 (percent): q = p + trunc((dest - p) x P / 100). The
// result never overshoots dest; P = 0 gives p exactly, P = 100 gives dest.
// q lies in [0, L]; q == L plays at the start of the next loop.
//
// Note lengths: by default the recorded length is kept (the note-off moves
// with the note-on). With quantizeNoteOff the absolute end p + length is
// quantized on the same grid (in its own loop) with the same strength, and
// length = quantized end - quantized start. If that is < 1 fine tick the
// recorded length is kept, so notes never get zero or negative length.
// Lengths are capped at Pattern::MaxNoteLengthTicks.
//
// Selection: `targets` (RecordTargets bits: drums, mono, FM, poly) and, for
// drums, `drumLanes` (bit p = pad p, 27 bits). Unselected events play at
// their raw timing. Velocity and every other field are never changed.

#include "pattern.h"
#include "recorded_track.h"

#include <cstdint>

namespace databreaks {

enum class QuantizeGrid : std::uint8_t {
    Whole, Half, Quarter, Eighth, Sixteenth, ThirtySecond, SixtyFourth
};

struct QuantizeSettings {
    static constexpr std::uint32_t AllDrumLanes = (1u << Pattern::Lanes) - 1u;

    QuantizeGrid grid = QuantizeGrid::Sixteenth;
    int strength = 0;                      // percent 0..100; 0 = off
    int swing = 0;                         // percent 0..100; 0 = straight
    unsigned targets = RecordAll;          // RecordTargets bits
    std::uint32_t drumLanes = AllDrumLanes;
    bool quantizeNoteOff = false;

    bool valid() const {
        return static_cast<unsigned>(grid) <= static_cast<unsigned>(QuantizeGrid::SixtyFourth) &&
               strength >= 0 && strength <= 100 && swing >= 0 && swing <= 100 &&
               (targets & ~static_cast<unsigned>(RecordAll)) == 0 &&
               (drumLanes & ~AllDrumLanes) == 0;
    }

    // True when the transform can move anything.
    bool active() const { return strength > 0 && targets != 0; }

    bool operator==(const QuantizeSettings& o) const {
        return grid == o.grid && strength == o.strength && swing == o.swing &&
               targets == o.targets && drumLanes == o.drumLanes &&
               quantizeNoteOff == o.quantizeNoteOff;
    }
    bool operator!=(const QuantizeSettings& o) const { return !(*this == o); }
};

namespace quantize {

constexpr std::int64_t FinePerTick = 1024;
constexpr std::int64_t MaxLengthFine = Pattern::MaxNoteLengthTicks * FinePerTick;

inline std::int64_t gridTicks(QuantizeGrid g) {
    switch (g) {
    case QuantizeGrid::Whole: return 3840;
    case QuantizeGrid::Half: return 1920;
    case QuantizeGrid::Quarter: return 960;
    case QuantizeGrid::Eighth: return 480;
    case QuantizeGrid::Sixteenth: return 240;
    case QuantizeGrid::ThirtySecond: return 120;
    case QuantizeGrid::SixtyFourth: return 60;
    }
    return 240;
}

inline std::int64_t swingOffsetFine(const QuantizeSettings& s) {
    return static_cast<std::int64_t>(s.swing) * gridTicks(s.grid) * FinePerTick / 200;
}

// Grid point k of a loop of length loopFine; returns false if it does not
// exist (a swung point at or beyond the loop end). Points with k x Gf >= L
// collapse onto the loop end L.
inline bool gridPoint(std::int64_t k, std::int64_t loopFine, const QuantizeSettings& s,
                      std::int64_t& out) {
    const std::int64_t gf = gridTicks(s.grid) * FinePerTick;
    if (k < 0) return false;
    const std::int64_t base = k * gf;
    if (base >= loopFine) {
        out = loopFine;
        return true;
    }
    const std::int64_t point = base + ((k & 1) ? swingOffsetFine(s) : 0);
    if (point >= loopFine) return false;
    out = point;
    return true;
}

// Nearest grid point to loop-relative position p (0 <= p < loopFine).
inline std::int64_t destination(std::int64_t p, std::int64_t loopFine, const QuantizeSettings& s) {
    const std::int64_t gf = gridTicks(s.grid) * FinePerTick;
    const std::int64_t k0 = p / gf;
    std::int64_t best = loopFine;  // the loop end always exists
    std::int64_t bestDistance = loopFine - p;
    for (std::int64_t k = k0 - 1; k <= k0 + 1; ++k) {
        std::int64_t point = 0;
        if (!gridPoint(k, loopFine, s, point)) continue;
        const std::int64_t d = point >= p ? point - p : p - point;
        // Nearest; on a tie the later point.
        if (d < bestDistance || (d == bestDistance && point > best)) {
            best = point;
            bestDistance = d;
        }
    }
    return best;
}

// Quantized position of loop-relative p: in [0, loopFine].
inline std::int64_t position(std::int64_t p, std::int64_t loopFine, const QuantizeSettings& s) {
    if (s.strength <= 0) return p;
    const std::int64_t dest = destination(p, loopFine, s);
    return p + (dest - p) * s.strength / 100;  // truncation: never overshoots
}

inline bool selected(const RecordedEvent& e, const QuantizeSettings& s) {
    if ((targetBit(e) & s.targets) == 0) return false;
    if (e.kind == RecordedEvent::Kind::Drum) {
        return e.value >= 0 && e.value < static_cast<int>(Pattern::Lanes) &&
               ((s.drumLanes >> e.value) & 1u) != 0;
    }
    return true;
}

// Played start (unwrapped, in [0, loopFine]) and length of an event.
inline void apply(const RecordedEvent& e, std::int64_t loopFine, const QuantizeSettings& s,
                  std::int64_t& start, std::int64_t& length) {
    start = e.startFine;
    length = e.lengthFine;
    if (!s.active() || !selected(e, s) || e.startFine < 0 || e.startFine >= loopFine) return;
    start = position(e.startFine, loopFine, s);
    if (e.kind != RecordedEvent::Kind::Note || !s.quantizeNoteOff || e.pending) return;
    const std::int64_t end = e.startFine + e.lengthFine;
    const std::int64_t loop = end / loopFine;  // end >= 0
    const std::int64_t qEnd = loop * loopFine + position(end - loop * loopFine, loopFine, s);
    const std::int64_t q = qEnd - start;
    length = q < 1 ? e.lengthFine : (q > MaxLengthFine ? MaxLengthFine : q);
}

} // namespace quantize
} // namespace databreaks
