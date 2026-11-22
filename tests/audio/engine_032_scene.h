// Engine 032 - a deterministic pipeline scene (test helper).
//
// Plays drums, mono, FM and poly notes through a pipeline's sequencer and
// hashes the rendered 16-bit output (converted like the demo WAV writers).
// It uses only API that existed at Engine 031 (commit 1d36980), so the host
// fingerprints in engine_032.cpp were recorded by compiling this header
// against that commit.
#pragma once

#include "audio_pipeline.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace e032 {

using namespace databreaks;

inline const std::vector<StereoFrame>& kickSample() {
    static const std::vector<StereoFrame> s = [] {
        std::vector<StereoFrame> v(4800);
        for (std::size_t i = 0; i < v.size(); ++i) {
            const double t = static_cast<double>(i) / SAMPLE_RATE;
            const double env = std::exp(-t * 30.0);
            const double x = 0.6 * env * std::sin(2.0 * PI * (50.0 + 120.0 * env) * t);
            v[i] = StereoFrame{x, 0.9 * x};
        }
        return v;
    }();
    return s;
}

// Sets up the scene: 4 pads, 1 bar of 16ths at 120 BPM, a mono line, two FM
// notes and a three-note poly chord (left out when `poly` is false). Stays
// inside the FM-1 profile's limits (at most 8 drum voices, 2 FM and 4 poly
// notes sound at once).
template <typename Pipe>
void setUp(Pipe& p, bool poly = true) {
    for (std::size_t pad = 0; pad < 4; ++pad) {
        p.drums().assignSample(pad, &kickSample());
        p.drums().setPitch(pad, static_cast<double>(pad) * 3.0);
    }
    auto& pat = p.sequencer().pattern();
    pat.configure({4, 4}, 1, 240);
    for (std::size_t k = 0; k < 16; ++k) {
        if (k % 4 == 0) pat.setHit(0, k, 120);
        if (k % 4 == 2) pat.setHit(1, k, 90);
        if (k % 3 == 1) pat.setHit(2, k, 70);
        if (k == 7 || k == 15) pat.setHit(3, k, 100);
    }
    pat.addNote({true, SynthTarget::Mono, 36, 100, 100, 0, 900});
    pat.addNote({true, SynthTarget::Mono, 43, 90, 100, 1920, 900});
    pat.addNote({true, SynthTarget::Fm, 60, 80, 100, 480, 600});
    pat.addNote({true, SynthTarget::Fm, 67, 80, 100, 2400, 600});
    if (poly) {
        pat.addNote({true, SynthTarget::Poly, 64, 70, 100, 0, 1800});
        pat.addNote({true, SynthTarget::Poly, 67, 70, 100, 0, 1800});
        pat.addNote({true, SynthTarget::Poly, 71, 70, 100, 0, 1800});
    }
    p.sequencer().start();
}

struct Fnv {
    std::uint64_t h = 0xcbf29ce484222325ull;
    void byte(std::uint8_t b) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
};

// Renders `frames` frames in blocks of `block` and hashes the Int16 output.
template <typename Pipe>
std::uint64_t render(Pipe& p, std::size_t frames, std::size_t block, std::vector<std::int16_t>* keep = nullptr) {
    Fnv f;
    std::vector<StereoFrame> buf(block);
    for (std::size_t pos = 0; pos < frames; pos += block) {
        const std::size_t n = (frames - pos < block) ? frames - pos : block;
        std::fill(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n), StereoFrame{});
        p.render(buf.data(), n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto l = static_cast<std::int16_t>(std::lround(clampAudio(buf[i].left) * 32767.0));
            const auto r = static_cast<std::int16_t>(std::lround(clampAudio(buf[i].right) * 32767.0));
            for (std::int16_t s : {l, r}) {
                f.byte(static_cast<std::uint8_t>(s & 0xff));
                f.byte(static_cast<std::uint8_t>((s >> 8) & 0xff));
                if (keep != nullptr) keep->push_back(s);
            }
        }
    }
    return f.h;
}

// Host scene with the compressor, tape and reverb enabled (host only).
inline std::uint64_t hostWithEffects(std::size_t frames, std::size_t block) {
    auto p = std::make_unique<AudioPipeline<16>>();
    setUp(*p);
    p->compressor().setEnabled(true, true);
    p->tape().setEnabled(true, true);
    p->tape().setSaturation(0.4);
    p->tape().setWow(0.3);
    p->reverb().setEnabled(true, true);
    p->reverb().setMix(0.3);
    return render(*p, frames, block);
}

} // namespace e032
