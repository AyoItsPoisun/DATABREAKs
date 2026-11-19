
// DATABREAKs Engine 020 - drum engine performance and reliability tests.
//
// Host-side assertion tests for click-free voice stealing, the fixed-capacity
// event queue, sustained polyphony, real-time safety and determinism of
// core/audio/drum_engine.h. Exits non-zero if any check fails.
// Not FM-1 firmware; nothing here runs on hardware.

#include "drum_engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <thread>
#include <vector>

// ---- Allocation counter -----------------------------------------------------
namespace {
std::atomic<std::size_t> g_allocations{0};
}

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace databreaks;

namespace {

int passed = 0;
int failed = 0;

void check(bool condition, const std::string& description) {
    if (condition) {
        ++passed;
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++failed;
        std::cout << "[FAIL] " << description << '\n';
    }
}

bool near(double a, double b, double tolerance = 1e-9) {
    return std::abs(a - b) <= tolerance;
}

constexpr std::size_t npos = static_cast<std::size_t>(-1);

std::vector<StereoFrame> constant(std::size_t frames, double l, double r) {
    return std::vector<StereoFrame>(frames, StereoFrame{l, r});
}

// Deterministic per-pad samples of varying length and content.
std::vector<StereoFrame> padSample(std::size_t pad) {
    const std::size_t length = 200 + pad * 37;
    std::vector<StereoFrame> s(length);
    for (std::size_t i = 0; i < length; ++i) {
        const double v =
            0.5 * std::sin(0.01 * static_cast<double>((pad + 1) * (i + 1)));
        s[i] = {v, -0.5 * v};
    }
    return s;
}

const auto kLeftOne = constant(48000, 1.0, 0.0);
const auto kRightOne = constant(48000, 0.0, 1.0);
const auto kRightQuarter = constant(48000, 0.0, 0.25);

template <typename Engine>
std::vector<StereoFrame> render(Engine& engine, std::size_t frames) {
    std::vector<StereoFrame> out(frames);
    engine.processBlock(out.data(), frames);
    return out;
}

// Ownership/activity consistency rules that must hold after processBlock.
template <typename Engine>
bool invariantsHold(const Engine& e) {
    constexpr std::size_t voices = Engine::voiceCapacity();
    if (e.activeVoiceCount() > voices) {
        return false;
    }

    std::size_t ownedSlots = 0;
    for (std::size_t slot = 0; slot < voices; ++slot) {
        const std::size_t owner = e.voiceOwner(slot);
        if (owner != npos) {
            ++ownedSlots;
            if (owner >= 27 || !e.isPadActive(owner)) {
                return false;
            }
        }
    }

    std::size_t perPad = 0;
    for (std::size_t pad = 0; pad < 27; ++pad) {
        const std::size_t total = e.padVoiceCount(pad);
        perPad += total;
        if (e.padReleasingVoiceCount(pad) + e.padPendingVoiceCount(pad) >
            total) {
            return false;
        }
        if (e.bank().isActive(pad) != e.isPadPlaying(pad)) {
            return false;
        }
    }
    return perPad == ownedSlots && perPad <= voices;
}

bool allSilent(const std::vector<StereoFrame>& out) {
    for (const auto& f : out) {
        if (f.left != 0.0 || f.right != 0.0) {
            return false;
        }
    }
    return true;
}

bool allFinite(const std::vector<StereoFrame>& out) {
    for (const auto& f : out) {
        if (!std::isfinite(f.left) || !std::isfinite(f.right)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
void stealing() {
    std::cout << "\n-- Click-free voice stealing --\n";

    // S1: every voice playing -> oldest voice gets the steal fade, the new
    // sound starts on the next frame in the same slot.
    {
        DrumEngine<2> e;
        e.setStealFadeFrames(4);
        e.assignSample(0, &kLeftOne);
        e.assignSample(1, &kLeftOne);
        e.assignSample(2, &kRightOne);
        e.trigger(0, 127);
        e.trigger(1, 127);
        render(e, 10);

        const std::size_t slot = e.trigger(2, 127);
        const bool kindOk =
            e.lastAllocation() == DrumEngine<2>::Allocation::StolePlaying;
        const bool ownerOk = slot == 0 && e.voiceOwner(0) == 2 &&
                             !e.isPadActive(0) && !e.bank().isActive(0) &&
                             e.padPendingVoiceCount(2) == 1 &&
                             e.isPadActive(2);
        const auto out = render(e, 6);
        check(kindOk && ownerOk,
              "S1a All voices busy: oldest is stolen, ownership moves at once");
        check(near(out[0].left, 2.0) && near(out[1].left, 1.75) &&
              near(out[2].left, 1.5) && near(out[3].left, 1.25) &&
              near(out[4].left, 1.0) && near(out[5].left, 1.0),
              "S1b Stolen voice fades 1, .75, .5, .25 (no hard cut)");
        check(near(out[3].right, 0.0) && near(out[4].right, 1.0) &&
              near(out[5].right, 1.0) && e.padPendingVoiceCount(2) == 0 &&
              e.isPadPlaying(2) && e.stolenVoiceCount() == 1 &&
              e.activeVoiceCount() == 2,
              "S1c New sound starts on the frame after the fade, capacity kept");
    }

    // S2: a releasing voice is preferred and its fade is shortened.
    {
        DrumEngine<2> e;
        e.setReleaseFadeFrames(100);
        e.setStealFadeFrames(4);
        e.assignSample(0, &kLeftOne);
        e.assignSample(1, &kLeftOne);
        e.assignSample(2, &kRightOne);
        e.trigger(0, 127);
        e.trigger(1, 127);
        render(e, 10);
        e.releasePad(0);
        render(e, 20);  // pad 0 gain is now 0.8
        const std::size_t slot = e.trigger(2, 127);
        const bool kindOk =
            e.lastAllocation() == DrumEngine<2>::Allocation::StoleReleasing;
        const auto out = render(e, 6);
        check(kindOk && slot == 0 && near(out[0].left, 1.8) &&
              near(out[1].left, 1.6) && near(out[2].left, 1.4) &&
              near(out[3].left, 1.2) && near(out[4].left, 1.0) &&
              near(out[3].right, 0.0) && near(out[4].right, 1.0),
              "S2 Releasing voice stolen first; 0.8 fades out in 4 frames");
    }

    // S3: among releasing voices the quietest is chosen, even if newer.
    {
        DrumEngine<3> e;
        for (std::size_t pad = 0; pad < 4; ++pad) {
            e.assignSample(pad, &kLeftOne);
        }
        e.trigger(0, 127);
        e.trigger(1, 127);
        e.trigger(2, 127);
        e.setReleaseFadeFrames(100);
        e.releasePad(0);
        render(e, 50);              // pad 0 gain 0.5
        e.setReleaseFadeFrames(10);
        e.releasePad(1);
        render(e, 8);               // pad 1 gain 0.2, pad 0 gain 0.42
        const std::size_t slot = e.trigger(3, 127);
        check(slot == 1 &&
              e.lastAllocation() == DrumEngine<3>::Allocation::StoleReleasing,
              "S3 Quietest releasing voice is stolen before an older louder one");
    }

    // S4: every slot already mid-steal -> earliest queued sound is replaced.
    {
        DrumEngine<2> e;
        e.setStealFadeFrames(10);
        e.assignSample(0, &kLeftOne);
        e.assignSample(1, &kLeftOne);
        e.assignSample(2, &kRightOne);
        e.assignSample(3, &kRightOne);
        e.assignSample(4, &kRightQuarter);
        e.trigger(0, 127);
        e.trigger(1, 127);
        e.trigger(2, 127);  // queued on slot 0
        e.trigger(3, 127);  // queued on slot 1
        e.trigger(4, 127);  // replaces pad 2's queued sound
        const bool state =
            e.lastAllocation() == DrumEngine<2>::Allocation::ReplacedPending &&
            e.droppedPendingSoundCount() == 1 && e.voiceOwner(0) == 4 &&
            !e.isPadActive(2) && !e.bank().isActive(2) &&
            e.isPadActive(3) && e.isPadActive(4);
        const auto out = render(e, 12);
        check(state && near(out[0].left, 2.0) && near(out[9].left, 0.2) &&
              near(out[10].left, 0.0) && near(out[9].right, 0.0) &&
              near(out[10].right, 1.25) && near(out[11].right, 1.25),
              "S4 Fully mid-steal pool replaces the earliest queued sound");
    }

    // S5: a queued sound that gets choked never sounds.
    {
        DrumEngine<1> e;
        e.setStealFadeFrames(4);
        e.setReleaseFadeFrames(100);
        e.assignSample(0, &kLeftOne);
        e.assignSample(1, &kRightOne);
        e.assignSample(2, &kRightQuarter);
        e.setChokeGroup(1, 5);
        e.setChokeGroup(2, 5);
        e.trigger(0, 127);
        render(e, 2);
        e.trigger(1, 127);  // queued behind pad 0's steal fade
        e.trigger(2, 127);  // chokes pad 1's queued sound
        const bool cancelled = !e.isPadActive(1) && e.isPadActive(2);
        const auto out = render(e, 6);
        check(cancelled && near(out[0].left, 1.0) && near(out[3].left, 0.25) &&
              near(out[4].left, 0.0) && near(out[3].right, 0.0) &&
              near(out[4].right, 0.25) && near(out[5].right, 0.25),
              "S5 Choking a queued sound cancels it before it starts");
    }

    // S6: same-pad retrigger while its sound is still queued.
    {
        DrumEngine<1> e;
        e.setStealFadeFrames(8);
        e.assignSample(0, &kLeftOne);
        e.assignSample(1, &kRightOne);
        e.trigger(0, 127);
        e.trigger(1, 127);
        e.trigger(1, 127);
        check(e.padVoiceCount(1) == 1 && e.padPendingVoiceCount(1) == 1 &&
              e.activeVoiceCount() == 1,
              "S6 Retriggering a queued pad keeps exactly one queued sound");
    }
}

// ---------------------------------------------------------------------------
void eventQueue() {
    std::cout << "\n-- Event queue and trigger timing --\n";

    {
        DrumEngine<4> e;
        e.assignSample(0, &kLeftOne);
        e.postTrigger(0, 127, 10);
        const auto out = render(e, 16);
        check(near(out[9].left, 0.0) && near(out[10].left, 1.0) &&
              near(out[15].left, 1.0) && e.processedEventCount() == 1,
              "Q1 Posted trigger starts exactly at its frame offset (10)");
    }
    {
        DrumEngine<4> e;
        e.assignSample(0, &kLeftOne);
        e.postTrigger(0, 127, 100);
        const auto out = render(e, 16);
        check(near(out[14].left, 0.0) && near(out[15].left, 1.0),
              "Q2 Offset beyond the block is clamped to the last frame");
    }
    {
        DrumEngine<4> e;
        e.assignSample(0, &kLeftOne);
        e.assignSample(1, &kRightOne);
        e.postTrigger(0, 127, 8);
        e.postTrigger(1, 127, 3);  // earlier offset, later in FIFO
        const auto out = render(e, 12);
        check(near(out[7].right, 0.0) && near(out[8].right, 1.0) &&
              near(out[8].left, 1.0),
              "Q3 Events apply in FIFO order (out-of-order offset waits)");
    }
    {
        DrumEngine<4> e;
        e.setReleaseFadeFrames(4);
        e.assignSample(0, &kLeftOne);
        e.assignSample(1, &kRightOne);
        e.trigger(0, 127);
        e.trigger(1, 127);
        e.postReleasePad(0, 2);
        e.postStopPad(1, 5);
        const auto out = render(e, 8);
        const bool releaseOk = near(out[2].left, 1.0) &&
                               near(out[3].left, 0.75) &&
                               near(out[5].left, 0.25) &&
                               near(out[6].left, 0.0);
        const bool stopOk = near(out[4].right, 1.0) && near(out[5].right, 0.0);
        e.trigger(0, 127);
        e.postStopAll(1);
        const auto out2 = render(e, 4);
        check(releaseOk && stopOk && near(out2[0].left, 1.0) &&
              near(out2[1].left, 0.0) && e.activeVoiceCount() == 0,
              "Q4 Posted release, stop-pad and stop-all act at their frames");
    }
    {
        DrumEngine<4, 8> e;  // usable capacity 7
        e.assignSample(0, &kLeftOne);
        int accepted = 0;
        for (int i = 0; i < 10; ++i) {
            accepted += e.postTrigger(0, 100, 0) ? 1 : 0;
        }
        const bool invalid = !e.postTrigger(27, 100) && !e.postTrigger(0, 0) &&
                             !e.postReleasePad(99) && !e.postStopPad(27);
        render(e, 4);
        check(accepted == 7 && e.droppedEventCount() == 3 && invalid &&
              e.processedEventCount() == 7 &&
              DrumEngine<4, 8>::eventCapacity() == 7,
              "Q5 Full queue drops and counts events; invalid ones are rejected");
    }
    {
        DrumEngine<8> e;
        for (std::size_t pad = 0; pad < 6; ++pad) {
            e.assignSample(pad, &kLeftOne);
            e.setChokeGroup(pad, 1);
        }
        for (std::size_t frame = 0; frame < 64; ++frame) {
            e.postTrigger(frame % 6, 100, frame);
        }
        std::vector<StereoFrame> out(64);
        e.processBlock(out.data(), 64);
        std::size_t playing = 0;
        for (std::size_t pad = 0; pad < 6; ++pad) {
            playing += e.isPadPlaying(pad) ? 1 : 0;
        }
        check(e.processedEventCount() == 64 && playing == 1 &&
              e.isPadPlaying(63 % 6) && invariantsHold(e) && allFinite(out),
              "08/Q6 Rapid sequence: 64 triggers, one per frame, in one choke group");
    }
}

// ---------------------------------------------------------------------------
void stress() {
    std::cout << "\n-- Sustained polyphony and stress --\n";

    std::vector<std::vector<StereoFrame>> samples;
    for (std::size_t pad = 0; pad < 27; ++pad) {
        samples.push_back(padSample(pad));
    }

    // 1: repeated triggering across all 27 pads
    {
        DrumEngine<16> e;
        for (std::size_t pad = 0; pad < 27; ++pad) {
            e.assignSample(pad, &samples[pad]);
        }
        bool ok = true;
        std::vector<StereoFrame> block(32);
        for (int round = 0; round < 3000; ++round) {
            e.trigger(static_cast<std::size_t>(round) % 27,
                      1 + round % 127);
            std::fill(block.begin(), block.end(), StereoFrame{});
            e.processBlock(block.data(), block.size());
            ok = ok && invariantsHold(e) && allFinite(block);
        }
        check(ok, "01 3000 triggers cycling all 27 pads keep every invariant");
    }

    // 2: more triggers than voices
    {
        DrumEngine<8> e;
        for (std::size_t pad = 0; pad < 27; ++pad) {
            e.assignSample(pad, &kLeftOne);
        }
        for (std::size_t i = 0; i < 100; ++i) {
            e.trigger(i % 27, 100);
        }
        const bool capped = e.activeVoiceCount() <= 8;
        render(e, 64);
        check(capped && e.activeVoiceCount() <= 8 &&
              e.stolenVoiceCount() > 0 && invariantsHold(e),
              "02 100 triggers on 8 voices never exceed capacity");
    }

    // 3: repeated same-pad triggering
    {
        DrumEngine<8> e;
        e.assignSample(0, &samples[0]);
        bool bounded = true;
        for (int i = 0; i < 1000; ++i) {
            e.trigger(0, 100);
            render(e, 4);
            bounded = bounded && e.padVoiceCount(0) <= 8 &&
                      e.padReleasingVoiceCount(0) + 1 >= e.padVoiceCount(0) &&
                      invariantsHold(e);
        }
        render(e, 4000);
        check(bounded && !e.isPadActive(0) && e.activeVoiceCount() == 0,
              "03 1000 same-pad hits: one playing voice, then silence");
    }

    // 4: multiple simultaneous choke groups
    {
        DrumEngine<16> e;
        for (std::size_t pad = 0; pad < 9; ++pad) {
            e.assignSample(pad, &kLeftOne);
            e.setChokeGroup(pad, 1 + static_cast<unsigned int>(pad / 3));
        }
        bool onePerGroup = true;
        for (int i = 0; i < 500; ++i) {
            e.trigger(static_cast<std::size_t>(i * 5) % 9, 90);
            render(e, 3);
            for (std::size_t g = 0; g < 3; ++g) {
                std::size_t playing = 0;
                for (std::size_t p = g * 3; p < g * 3 + 3; ++p) {
                    playing += e.isPadPlaying(p) ? 1 : 0;
                }
                onePerGroup = onePerGroup && playing <= 1;
            }
            onePerGroup = onePerGroup && invariantsHold(e);
        }
        check(onePerGroup, "04 Three choke groups: at most one pad plays per group");
    }

    // 5 + 6 + 12: stealing while fading / all active, fades under stress.
    // Long constant samples: any downward step larger than the fade slopes
    // would be a hard cut.
    {
        DrumEngine<8> e;
        e.setStealFadeFrames(48);
        e.setReleaseFadeFrames(240);
        for (std::size_t pad = 0; pad < 27; ++pad) {
            e.assignSample(pad, &kLeftOne);
        }
        double prev = 0.0;
        double worstDrop = 0.0;
        bool ok = true;
        std::uint64_t releasingSteals = 0;
        std::uint64_t playingSteals = 0;
        for (int round = 0; round < 400; ++round) {
            const std::size_t pad = static_cast<std::size_t>(round * 7) % 27;
            if (round % 5 == 0) {
                e.releasePad(static_cast<std::size_t>(round * 3) % 27);
            }
            e.trigger(pad, 127);
            if (e.lastAllocation() ==
                DrumEngine<8>::Allocation::StoleReleasing) {
                ++releasingSteals;
            }
            if (e.lastAllocation() ==
                DrumEngine<8>::Allocation::StolePlaying) {
                ++playingSteals;
            }
            const auto out = render(e, 24);
            for (const auto& f : out) {
                worstDrop = std::max(worstDrop, prev - f.left);
                prev = f.left;
                ok = ok && f.right == 0.0 && f.left <= 8.0 + 1e-9;
            }
            ok = ok && invariantsHold(e);
        }
        // At most 8 voices fall at once, each by at most 1/48 per frame.
        check(releasingSteals > 0,
              "05 Stress run steals voices that are already fading");
        check(playingSteals > 0,
              "06 Stress run steals voices while all are playing");
        check(ok && worstDrop <= 8.0 / 48.0 + 1e-9,
              "12 Largest per-frame drop under stress is within fade slopes");
    }

    // 7: long-running playback (60 s of audio)
    {
        DrumEngine<16> e;
        for (std::size_t pad = 0; pad < 27; ++pad) {
            e.assignSample(pad, &samples[pad]);
            e.setChokeGroup(pad, static_cast<unsigned int>(pad % 4));
        }
        const std::size_t totalFrames = 60 * static_cast<std::size_t>(SAMPLE_RATE);
        const std::size_t blockSize = 128;
        std::vector<StereoFrame> block(blockSize);
        bool ok = true;
        double peak = 0.0;
        for (std::size_t pos = 0, n = 0; pos < totalFrames;
             pos += blockSize, ++n) {
            if (n % 3 == 0) {
                e.trigger((n * 11) % 27, static_cast<int>(1 + n % 127));
            }
            std::fill(block.begin(), block.end(), StereoFrame{});
            e.processBlock(block.data(), blockSize);
            for (const auto& f : block) {
                peak = std::max(peak, std::max(std::abs(f.left),
                                               std::abs(f.right)));
            }
            ok = ok && allFinite(block) && invariantsHold(e);
        }
        render(e, 8000);
        bool clear = e.activeVoiceCount() == 0;
        for (std::size_t pad = 0; pad < 27; ++pad) {
            clear = clear && !e.isPadActive(pad) && !e.bank().isActive(pad);
        }
        // Each pad sample peaks at 0.5; 16 voices at most.
        check(ok && peak > 0.0 && peak <= 16 * 0.5 + 1e-9 && clear,
              "07 60 s of playback: finite, bounded, consistent, then idle");
    }

    // 9: repeated stopPad / stopAll
    {
        DrumEngine<8> e;
        for (std::size_t pad = 0; pad < 27; ++pad) {
            e.assignSample(pad, &kLeftOne);
        }
        bool ok = true;
        for (int i = 0; i < 500; ++i) {
            const std::size_t pad = static_cast<std::size_t>(i * 13) % 27;
            e.trigger(pad, 100);
            e.trigger((pad + 1) % 27, 100);
            render(e, 2);
            e.stopPad(pad);
            ok = ok && !e.isPadActive(pad) && !e.bank().isActive(pad) &&
                 invariantsHold(e);
            if (i % 50 == 49) {
                e.stopAll();
                const auto out = render(e, 4);
                ok = ok && allSilent(out) && e.activeVoiceCount() == 0;
            }
        }
        check(ok, "09 500 stopPad and 10 stopAll calls keep state consistent");
    }

    // 10: ownership after extensive slot reuse
    {
        DrumEngine<4> e;
        e.setStealFadeFrames(2);
        for (std::size_t pad = 0; pad < 27; ++pad) {
            e.assignSample(pad, &samples[pad]);
        }
        bool ok = true;
        std::uint64_t lastId = 0;
        for (int i = 0; i < 10000; ++i) {
            const std::size_t pad = static_cast<std::size_t>(i * 17) % 27;
            const std::size_t slot = e.trigger(pad, 100);
            const std::uint64_t id = e.voices().hasPending(slot)
                ? e.voices().pendingSoundId(slot)
                : e.voices().voiceSoundId(slot);
            ok = ok && slot < 4 && e.voiceOwner(slot) == pad && id > lastId;
            lastId = id;
            render(e, 1 + static_cast<std::size_t>(i % 5));
            ok = ok && invariantsHold(e);
        }
        check(ok, "10 10000 triggers: owner always correct, sound ids unique");
    }

    // 11: stereo correctness
    {
        DrumEngine<8> left;
        DrumEngine<8> right;
        for (std::size_t pad = 0; pad < 27; ++pad) {
            left.assignSample(pad, &kLeftOne);
            right.assignSample(pad, &kRightOne);
        }
        bool isolated = true;
        for (int i = 0; i < 300; ++i) {
            const std::size_t pad = static_cast<std::size_t>(i * 5) % 27;
            left.trigger(pad, 1 + i % 127);
            right.trigger(pad, 1 + i % 127);
            const auto l = render(left, 8);
            const auto r = render(right, 8);
            for (std::size_t f = 0; f < 8; ++f) {
                isolated = isolated && l[f].right == 0.0 &&
                           r[f].left == 0.0 && l[f].left == r[f].right;
            }
        }
        check(isolated,
              "11 Channels stay isolated and mirror exactly under stress");
    }

    // 13: silence after everything finishes
    {
        DrumEngine<16> e;
        for (std::size_t pad = 0; pad < 27; ++pad) {
            e.assignSample(pad, &samples[pad]);
            e.trigger(pad, 127);
        }
        render(e, 2000);
        const auto out = render(e, 512);
        bool clear = allSilent(out) && e.activeVoiceCount() == 0;
        for (std::size_t slot = 0; slot < 16; ++slot) {
            clear = clear && e.voiceOwner(slot) == npos &&
                    !e.voices().hasPending(slot);
        }
        for (std::size_t pad = 0; pad < 27; ++pad) {
            clear = clear && !e.isPadActive(pad) && !e.bank().isActive(pad);
        }
        check(clear, "13 Silence and no active state once all voices finish");
    }
}

// ---------------------------------------------------------------------------
// One scripted performance, rendered with a given block size. Events are
// posted with offsets so that each lands on the same absolute frame.
std::vector<StereoFrame> scripted(std::size_t blockSize,
                                  const std::vector<std::vector<StereoFrame>>&
                                      samples,
                                  std::uint64_t* stats) {
    DrumEngine<6> e;
    e.setStealFadeFrames(16);
    e.setReleaseFadeFrames(64);
    for (std::size_t pad = 0; pad < 27; ++pad) {
        e.assignSample(pad, &samples[pad]);
        e.setChokeGroup(pad, static_cast<unsigned int>(pad % 3));
    }
    const std::size_t total = 20000;
    std::vector<StereoFrame> out(total);
    for (std::size_t pos = 0; pos < total; pos += blockSize) {
        const std::size_t n = std::min(blockSize, total - pos);
        for (std::size_t frame = pos; frame < pos + n; ++frame) {
            if (frame % 37 == 0) {
                e.postTrigger((frame / 37) % 27,
                              static_cast<int>(1 + frame % 127), frame - pos);
            }
            if (frame % 401 == 0) {
                e.postReleasePad((frame / 401) % 27, frame - pos);
            }
        }
        e.processBlock(out.data() + pos, n);
    }
    if (stats != nullptr) {
        stats[0] = e.stolenVoiceCount();
        stats[1] = e.processedEventCount();
        stats[2] = e.droppedPendingSoundCount();
    }
    return out;
}

void determinismAndSafety() {
    std::cout << "\n-- Determinism, allocation and threading --\n";

    std::vector<std::vector<StereoFrame>> samples;
    for (std::size_t pad = 0; pad < 27; ++pad) {
        samples.push_back(padSample(pad));
    }

    // 15: identical sequences give identical results, for any block size.
    std::uint64_t refStats[3]{};
    const auto reference = scripted(64, samples, refStats);
    bool identical = true;
    for (std::size_t size : {1u, 7u, 64u, 256u, 1000u}) {
        std::uint64_t stats[3]{};
        const auto other = scripted(size, samples, stats);
        for (std::size_t i = 0; i < reference.size(); ++i) {
            identical = identical && reference[i].left == other[i].left &&
                        reference[i].right == other[i].right;
        }
        identical = identical && stats[0] == refStats[0] &&
                    stats[1] == refStats[1] && stats[2] == refStats[2];
    }
    check(identical && refStats[0] > 0 && refStats[1] > 0,
          "15 Same script is bit-identical across runs and block sizes 1-1000");

    // 14: no heap allocation in trigger, post, processBlock, release, stop.
    {
        DrumEngine<8> e;
        e.setStealFadeFrames(8);
        for (std::size_t pad = 0; pad < 27; ++pad) {
            e.assignSample(pad, &samples[pad]);
            e.setChokeGroup(pad, static_cast<unsigned int>(pad % 4));
        }
        StereoFrame block[64]{};
        const std::size_t before = g_allocations.load();
        for (int i = 0; i < 2000; ++i) {
            e.trigger(static_cast<std::size_t>(i * 7) % 27, 100);
            e.postTrigger(static_cast<std::size_t>(i * 3) % 27, 90,
                          static_cast<std::size_t>(i) % 64);
            if (i % 9 == 0) {
                e.postReleasePad(static_cast<std::size_t>(i) % 27, 5);
            }
            if (i % 31 == 0) {
                e.stopPad(static_cast<std::size_t>(i) % 27);
            }
            e.processBlock(block, 64);
        }
        e.releaseAll();
        e.postStopAll();
        e.processBlock(block, 64);
        e.stopAll();
        const std::size_t after = g_allocations.load();
        const bool noAllocation = after == before;
        check(noAllocation,
              "14 No heap allocation across 2000 rounds of the real-time API");
    }

    // T: one producer thread posts while the audio thread processes.
    {
        static DrumEngine<16, 64> e;  // static: keep it off the small stack
        for (std::size_t pad = 0; pad < 27; ++pad) {
            e.assignSample(pad, &samples[pad]);
        }
        constexpr int kPosts = 20000;
        std::atomic<bool> producerDone{false};
        std::atomic<int> accepted{0};

        std::thread producer([&] {
            int ok = 0;
            for (int i = 0; i < kPosts; ++i) {
                if (e.postTrigger(static_cast<std::size_t>(i) % 27,
                                  1 + i % 127,
                                  static_cast<std::size_t>(i) % 32)) {
                    ++ok;
                }
            }
            accepted.store(ok);
            producerDone.store(true);
        });

        std::vector<StereoFrame> block(32);
        bool ok = true;
        // Keep draining until the producer has finished and nothing is left.
        for (int spins = 0; spins < 2000000; ++spins) {
            std::fill(block.begin(), block.end(), StereoFrame{});
            e.processBlock(block.data(), block.size());
            ok = ok && allFinite(block) && invariantsHold(e);
            if (producerDone.load() &&
                e.processedEventCount() ==
                    static_cast<std::uint64_t>(accepted.load())) {
                break;
            }
        }
        producer.join();
        check(ok && accepted.load() > 0 &&
              e.processedEventCount() + e.droppedEventCount() ==
                  static_cast<std::uint64_t>(kPosts),
              "T Two threads: every posted event is applied or counted dropped");
    }
}

}  // namespace

int main() {
    std::cout << "DATABREAKs Engine 020 - Drum Engine Performance and Reliability\n";
    std::cout << "(host-side test; not FM-1 firmware)\n";

    stealing();
    eventQueue();
    stress();
    determinismAndSafety();

    std::cout << "\n------------------------------\n";
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';

    if (failed != 0) {
        std::cout << "ENGINE 020 FAILED\n";
        return EXIT_FAILURE;
    }

    std::cout << "ENGINE 020 PASSED\n";
    return EXIT_SUCCESS;
}
