
// DATABREAKs Engine 019 - 27-pad drum voice integration tests.
//
// Host-side assertion tests for core/audio/drum_engine.h and the Engine 019
// extensions to SampleVoiceManager. Exits non-zero if any check fails.

#include "drum_engine.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <vector>

// Count every heap allocation so the test can prove that trigger() and
// processBlock() do not allocate.
namespace {
std::size_t g_allocations = 0;
}

void* operator new(std::size_t size) {
    ++g_allocations;
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

std::vector<StereoFrame> constant(std::size_t frames,
                                  double left,
                                  double right) {
    return std::vector<StereoFrame>(frames, StereoFrame{left, right});
}

// left = i, right = -i
std::vector<StereoFrame> ramp(std::size_t frames) {
    std::vector<StereoFrame> sample(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        sample[i] = {static_cast<double>(i), -static_cast<double>(i)};
    }
    return sample;
}

template <typename Engine>
std::vector<StereoFrame> render(Engine& engine, std::size_t frames) {
    std::vector<StereoFrame> out(frames);
    engine.processBlock(out.data(), frames);
    return out;
}

constexpr std::size_t npos = static_cast<std::size_t>(-1);

// Long samples so voices stay alive unless something stops them.
const auto kLeftOne = constant(4800, 1.0, 0.0);   // audible on left only
const auto kRightOne = constant(4800, 0.0, 1.0);  // audible on right only
const auto kBoth = constant(4800, 0.5, 0.25);
const auto kShort = constant(4, 1.0, 1.0);
const auto kRamp16 = ramp(16);

// ---------------------------------------------------------------------------
void basicIntegration() {
    std::cout << "\n-- Basic integration --\n";

    DrumEngine<16> engine;

    // 1
    check(DrumEngine<16>::padCount() == 27 &&
          DrumEngine<16>::voiceCapacity() == 16,
          "01 Engine has 27 pads and a separate 16-voice capacity");

    // 2
    bool allAssigned = true;
    for (std::size_t pad = 0; pad < 27; ++pad) {
        allAssigned = engine.assignSample(pad, &kBoth) && allAssigned;
    }
    check(allAssigned && !engine.assignSample(27, &kBoth),
          "02 Every pad 0-26 accepts a sample; pad 27 is rejected");

    // 3
    const std::size_t slot = engine.trigger(5, 127);
    auto out = render(engine, 1);
    check(slot != npos && near(out[0].left, 0.5) && near(out[0].right, 0.25),
          "03 Triggering a valid pad produces audio");
    engine.stopAll();

    // 4
    check(engine.trigger(27, 127) == npos &&
          engine.trigger(npos, 127) == npos &&
          engine.activeVoiceCount() == 0,
          "04 Invalid pad indices fail without creating a voice");

    // 5
    DrumEngine<16> empty;
    check(empty.trigger(3, 127) == npos && empty.activeVoiceCount() == 0 &&
          !empty.bank().isActive(3),
          "05 Unassigned pad does not create a voice");

    // 6
    check(engine.trigger(0, 0) == npos && engine.trigger(0, -5) == npos &&
          engine.activeVoiceCount() == 0 && !engine.isPadActive(0),
          "06 Velocity 0 (and negative) does not create a voice");

    // 7
    DrumEngine<16> vel;
    vel.assignSample(0, &kLeftOne);
    vel.setVolume(0, 0.8);
    vel.setVelocitySensitivity(0, 1.0);
    vel.trigger(0, 64);
    out = render(vel, 1);
    const bool scaled = near(out[0].left, 0.8 * 64.0 / 127.0);
    vel.stopAll();
    vel.setVelocitySensitivity(0, 0.0);
    vel.trigger(0, 10);
    out = render(vel, 1);
    check(scaled && near(out[0].left, 0.8),
          "07 Velocity sensitivity and pad volume scale the output");
    vel.stopAll();
    vel.setVelocitySensitivity(0, 1.0);
    vel.trigger(0, 500);  // clamped to 127 by DrumPadBank
    out = render(vel, 1);
    check(near(out[0].left, 0.8),
          "07b Velocity above 127 is clamped to 127");

    // 8
    DrumEngine<16> pitch;
    pitch.assignSample(0, &kRamp16);
    pitch.setPitch(0, 12.0);
    pitch.trigger(0, 127);
    out = render(pitch, 4);
    const bool octaveUp = near(out[0].left, 0.0) && near(out[1].left, 2.0) &&
                          near(out[3].left, 6.0) && near(out[3].right, -6.0);
    pitch.stopAll();
    pitch.setPitch(0, 0.0);
    pitch.setReverse(0, true);
    pitch.trigger(0, 127);
    out = render(pitch, 3);
    check(octaveUp && near(out[0].left, 15.0) && near(out[1].left, 14.0) &&
          near(out[2].left, 13.0),
          "08 Pitch (+12 = double rate) and reverse reach the voice");
    pitch.stopAll();

    // 9
    DrumEngine<16> region;
    region.assignSample(0, &kRamp16, 4, 8);
    region.trigger(0, 127);
    out = render(region, 6);
    check(near(out[0].left, 4.0) && near(out[3].left, 7.0) &&
          near(out[4].left, 0.0) && near(out[5].left, 0.0) &&
          !region.isPadActive(0),
          "09 Sample region [4, 8) is played exactly and then stops");
}

// ---------------------------------------------------------------------------
void ownership() {
    std::cout << "\n-- Pad-to-voice ownership --\n";

    DrumEngine<16> engine;
    engine.assignSample(3, &kLeftOne);
    engine.assignSample(7, &kRightOne);

    // 10
    const std::size_t a = engine.trigger(3, 127);
    check(a != npos && engine.voiceOwner(a) == 3 &&
          engine.padVoiceCount(3) == 1,
          "10 Triggered voice is owned by the pad that started it");

    // 11
    const std::size_t b = engine.trigger(7, 127);
    auto out = render(engine, 1);
    check(b != npos && b != a && engine.voiceOwner(b) == 7 &&
          engine.isPadActive(3) && engine.isPadActive(7) &&
          near(out[0].left, 1.0) && near(out[0].right, 1.0),
          "11 Two different pads play simultaneously");
    engine.stopAll();

    // 12
    DrumEngine<16> finish;
    finish.assignSample(0, &kShort);
    const std::size_t s = finish.trigger(0, 127);
    render(finish, 4);
    check(finish.voiceOwner(s) == npos && !finish.isPadActive(0) &&
          !finish.bank().isActive(0) && finish.activeVoiceCount() == 0,
          "12 Ownership and activity clear after the sample ends");

    // 13
    DrumEngine<2> small;
    small.assignSample(0, &kLeftOne);
    small.assignSample(1, &kLeftOne);
    small.assignSample(2, &kLeftOne);
    const std::size_t s0 = small.trigger(0, 127);
    small.trigger(1, 127);
    const std::size_t s2 = small.trigger(2, 127);  // pool full -> steal
    check(s2 == s0 && small.voiceOwner(s2) == 2 &&
          !small.isPadActive(0) && !small.bank().isActive(0) &&
          small.isPadActive(1) && small.isPadActive(2),
          "13 Voice stealing clears the previous pad's activity");

    // 14
    DrumEngine<1> reuse;
    reuse.assignSample(0, &kShort);
    reuse.assignSample(1, &kLeftOne);
    reuse.trigger(0, 127);
    render(reuse, 4);  // pad 0 finishes; slot 0 is free
    const std::size_t r = reuse.trigger(1, 127);
    check(r == 0 && reuse.voiceOwner(0) == 1 &&
          reuse.padVoiceCount(0) == 0 && !reuse.isPadActive(0) &&
          reuse.voices().voiceGeneration(0) == 2,
          "14 Reusing a slot leaves no stale ownership (generation 2)");

    // 15
    DrumEngine<4> multi;
    multi.assignSample(0, &kLeftOne);
    multi.setReleaseFadeFrames(10);
    multi.trigger(0, 127);
    render(multi, 5);
    multi.trigger(0, 127);  // old voice fades, new voice plays
    std::size_t slots[4]{};
    const std::size_t n = multi.padVoices(0, slots, 4);
    check(n == 2 && multi.padReleasingVoiceCount(0) == 1 &&
          multi.isPadActive(0),
          "15 A pad with a fading and a playing voice reports both");
    multi.stopPad(0);
}

// ---------------------------------------------------------------------------
void choke() {
    std::cout << "\n-- Choke groups --\n";

    DrumEngine<8> engine;
    engine.setReleaseFadeFrames(4);
    engine.assignSample(0, &kLeftOne);   // open hat (left)
    engine.assignSample(1, &kRightOne);  // closed hat (right)
    engine.assignSample(2, &kLeftOne);
    engine.assignSample(3, &kRightOne);

    // 16
    check(engine.setChokeGroup(0, 1) && engine.setChokeGroup(1, 1) &&
          engine.getPad(0)->chokeGroup == 1 &&
          engine.getPad(1)->chokeGroup == 1,
          "16 Two pads share nonzero choke group 1");

    engine.trigger(0, 127);
    render(engine, 2);

    // 17
    engine.trigger(1, 127);
    check(engine.padReleasingVoiceCount(0) == 1 && engine.isPadReleasing(0),
          "17 Triggering pad 1 starts the release of pad 0's voice");

    // 18 + 19
    auto out = render(engine, 6);
    check(near(out[0].left, 1.0) && near(out[1].left, 0.75) &&
          near(out[2].left, 0.5) && near(out[3].left, 0.25) &&
          near(out[4].left, 0.0) && near(out[5].left, 0.0),
          "18 Choked voice fades 1, 0.75, 0.5, 0.25, 0 (4-frame linear)");
    check(near(out[0].right, 1.0) && near(out[5].right, 1.0) &&
          engine.isPadPlaying(1) && !engine.isPadActive(0),
          "19 The choking pad plays at full level from its first frame");
    engine.stopAll();

    // 20
    engine.setChokeGroup(2, 2);
    engine.setChokeGroup(3, 3);
    engine.trigger(2, 127);
    engine.trigger(3, 127);
    check(engine.isPadPlaying(2) && engine.isPadPlaying(3) &&
          engine.padReleasingVoiceCount(2) == 0,
          "20 Pads in different choke groups do not choke each other");
    engine.stopAll();

    // 21
    engine.setChokeGroup(2, 0);
    engine.setChokeGroup(3, 0);
    engine.trigger(0, 127);  // group 1
    engine.trigger(2, 127);  // group 0
    engine.trigger(3, 127);  // group 0
    check(engine.isPadPlaying(0) && engine.isPadPlaying(2) &&
          engine.isPadPlaying(3),
          "21 Group 0 pads neither choke nor get choked");
    engine.stopAll();

    // 22
    engine.setChokeGroup(2, 1);
    engine.trigger(0, 127);
    engine.trigger(1, 127);
    engine.trigger(2, 127);
    engine.trigger(0, 127);
    engine.trigger(1, 127);
    const bool onlyLastPlays =
        engine.isPadPlaying(1) && !engine.isPadPlaying(0) &&
        !engine.isPadPlaying(2) && engine.padVoiceCount(1) == 2 &&
        engine.padReleasingVoiceCount(1) == 1;
    render(engine, 10);
    check(onlyLastPlays && engine.activeVoiceCount() == 1 &&
          engine.isPadActive(1) && !engine.isPadActive(0) &&
          !engine.isPadActive(2),
          "22 Rapid triggers in one group leave only the last pad sounding");

    // 22b: the bank's active flag is cleared by assignSample while a voice
    // is still playing; the choke must still reach that voice.
    engine.stopAll();
    engine.trigger(0, 127);
    engine.assignSample(0, &kLeftOne);
    engine.trigger(1, 127);
    check(engine.isPadReleasing(0),
          "22b Choke reaches a playing pad even after its sample was reassigned");
    engine.stopAll();
}

// ---------------------------------------------------------------------------
void retrigger() {
    std::cout << "\n-- Same-pad retrigger --\n";

    DrumEngine<8> engine;
    engine.setReleaseFadeFrames(4);
    engine.assignSample(0, &kLeftOne);

    check(engine.retriggerPolicy() == RetriggerPolicy::FadeAndRetrigger,
          "Default retrigger policy is FadeAndRetrigger");

    const std::size_t first = engine.trigger(0, 127);
    render(engine, 3);

    // 23 + 24
    const std::size_t second = engine.trigger(0, 127);
    check(second != first && engine.voices().isVoiceReleasing(first),
          "23 Retrigger starts the release of the previous voice");
    auto out = render(engine, 1);
    check(near(out[0].left, 2.0) && !engine.voices().isVoiceReleasing(second),
          "24 New voice sounds immediately (old at 1.0 + new at 1.0)");

    // 25
    render(engine, 3);
    check(!engine.voices().isVoiceActive(first) &&
          engine.voiceOwner(second) == 0 && engine.isPadPlaying(0) &&
          engine.bank().isActive(0),
          "25 New voice stays active after the old voice finishes fading");

    // 26
    for (int i = 0; i < 20; ++i) {
        engine.trigger(0, 127);
        render(engine, 2);
    }
    render(engine, 4);
    check(engine.padVoiceCount(0) == 1 && engine.activeVoiceCount() == 1 &&
          engine.padReleasingVoiceCount(0) == 0,
          "26 Repeated retriggers leave exactly one live, owned voice");
    engine.stopAll();

    // 27: documented fallback with a 2-voice pool:
    //   free slot -> oldest releasing voice -> oldest voice (hard steal).
    DrumEngine<2> pool;
    pool.setReleaseFadeFrames(100);
    pool.assignSample(0, &kLeftOne);
    pool.assignSample(1, &kLeftOne);
    pool.assignSample(2, &kLeftOne);
    const std::size_t p0 = pool.trigger(0, 127);   // slot 0
    const std::size_t p0b = pool.trigger(0, 127);  // slot 1, slot 0 fades
    const std::size_t p1 = pool.trigger(1, 127);   // pool full: takes the
                                                   // releasing slot 0
    const bool stoleReleasing = p0 == 0 && p0b == 1 && p1 == 0 &&
                                pool.padVoiceCount(0) == 1 &&
                                pool.voiceOwner(0) == 1;
    const std::size_t p2 = pool.trigger(2, 127);   // none releasing: oldest
                                                   // voice (slot 1, pad 0)
    check(stoleReleasing && p2 == 1 && pool.voiceOwner(1) == 2 &&
          !pool.isPadActive(0) && pool.isPadActive(1) &&
          pool.isPadActive(2),
          "27 Exhausted pool: releasing voice stolen first, then oldest");

    // Alternate policy is selectable and deterministic.
    DrumEngine<4> overlap;
    overlap.setRetriggerPolicy(RetriggerPolicy::Overlap);
    overlap.assignSample(0, &kLeftOne);
    overlap.trigger(0, 127);
    overlap.trigger(0, 127);
    check(overlap.padVoiceCount(0) == 2 &&
          overlap.padReleasingVoiceCount(0) == 0,
          "Overlap policy layers voices without fading");
}

// ---------------------------------------------------------------------------
void activity() {
    std::cout << "\n-- Activity and stopping --\n";

    DrumEngine<8> engine;
    engine.setReleaseFadeFrames(8);
    engine.assignSample(0, &kLeftOne);
    engine.assignSample(1, &kRightOne);
    engine.assignSample(2, &kShort);

    // 28
    engine.trigger(0, 127);
    render(engine, 16);
    check(engine.isPadActive(0) && engine.isPadPlaying(0) &&
          engine.bank().isActive(0),
          "28 Pad is active during normal playback");

    // 29
    engine.releasePad(0);
    render(engine, 4);
    check(engine.isPadActive(0) && engine.isPadReleasing(0) &&
          !engine.isPadPlaying(0) && !engine.bank().isActive(0),
          "29 Pad stays active (releasing) during its fade");

    // 30
    render(engine, 4);
    check(!engine.isPadActive(0) && engine.padVoiceCount(0) == 0,
          "30 Pad becomes inactive when its last voice ends");

    // 31
    engine.trigger(0, 127);
    engine.trigger(1, 127);
    engine.stopPad(0);
    auto out = render(engine, 1);
    check(!engine.isPadActive(0) && !engine.bank().isActive(0) &&
          engine.isPadActive(1) && near(out[0].left, 0.0) &&
          near(out[0].right, 1.0),
          "31 stopPad silences only that pad and clears its state");

    // 32
    engine.trigger(0, 127);
    engine.stopAll();
    bool allClear = engine.activeVoiceCount() == 0;
    for (std::size_t pad = 0; pad < 27; ++pad) {
        allClear = allClear && !engine.isPadActive(pad) &&
                   !engine.bank().isActive(pad);
    }
    for (std::size_t slot = 0; slot < 8; ++slot) {
        allClear = allClear && engine.voiceOwner(slot) == npos;
    }
    check(allClear, "32 stopAll clears all ownership and pad activity");

    // 33
    for (std::size_t pad = 0; pad < 27; ++pad) {
        engine.assignSample(pad, &kShort);
    }
    for (std::size_t pad = 0; pad < 27; ++pad) {
        engine.trigger(pad, 100);  // 27 hits, 8 voices
    }
    render(engine, 8);
    bool noneActive = engine.activeVoiceCount() == 0;
    for (std::size_t pad = 0; pad < 27; ++pad) {
        noneActive = noneActive && !engine.isPadActive(pad) &&
                     !engine.bank().isActive(pad);
    }
    check(noneActive, "33 Finished samples leave no active pads behind");

    // releaseAll fades every pad
    engine.assignSample(0, &kLeftOne);
    engine.assignSample(1, &kRightOne);
    engine.trigger(0, 127);
    engine.trigger(1, 127);
    engine.releaseAll();
    const bool bothReleasing =
        engine.isPadReleasing(0) && engine.isPadReleasing(1);
    render(engine, 8);
    check(bothReleasing && engine.activeVoiceCount() == 0,
          "releaseAll fades every pad to silence");
}

// ---------------------------------------------------------------------------
void audio() {
    std::cout << "\n-- Audio correctness and safety --\n";

    // 34
    DrumEngine<4> stereo;
    stereo.assignSample(0, &kBoth);
    stereo.trigger(0, 127);
    auto out = render(stereo, 2);
    check(near(out[1].left, 0.5) && near(out[1].right, 0.25),
          "34 Left and right channels carry their own sample values");

    // 35 + 36
    DrumEngine<4> fade;
    fade.assignSample(0, &kLeftOne);
    check(fade.releaseFadeFrames() == 240,
          "Default release fade is 240 frames (5 ms at 48 kHz)");
    fade.trigger(0, 127);
    render(fade, 10);
    fade.releasePad(0);
    out = render(fade, 300);
    bool monotonic = true;
    double maxStep = 0.0;
    std::size_t lastNonZero = 0;
    for (std::size_t i = 1; i < out.size(); ++i) {
        if (out[i].left > out[i - 1].left + 1e-12) {
            monotonic = false;
        }
        maxStep = std::max(maxStep, std::abs(out[i].left - out[i - 1].left));
        if (out[i].left > 0.0) {
            lastNonZero = i;
        }
    }
    check(monotonic && near(out[0].left, 1.0) && near(out[299].left, 0.0) &&
          lastNonZero == 239 && !fade.isPadActive(0),
          "35 Fade gain falls monotonically and reaches zero after 240 frames");
    check(maxStep <= 1.0 / 240.0 + 1e-9,
          "36 Largest step during the fade is 1/240 (no click)");

    // Repeated releases never make a voice louder and never slow the fade.
    SampleVoiceManager<2> manager;
    manager.trigger(&kLeftOne);
    manager.releaseVoice(0, 10);
    StereoFrame frame[3]{};
    manager.processBlock(frame, 3);       // gain now 0.7
    const double before = manager.voiceReleaseGain(0);
    manager.releaseVoice(0, 1000);        // slower request: ignored
    StereoFrame next[1]{};
    manager.processBlock(next, 1);
    const bool notSlower = near(manager.voiceReleaseGain(0), before - 0.1);
    manager.releaseVoice(0, 1);           // faster request: adopted
    StereoFrame last[2]{};
    manager.processBlock(last, 2);
    check(near(before, 0.7) && notSlower && near(next[0].left, 0.7) &&
          near(last[0].left, 0.6) && near(last[1].left, 0.0) &&
          !manager.isVoiceActive(0),
          "35b Re-release keeps the steeper slope and never raises gain");

    // 37
    DrumEngine<4> mix;
    mix.assignSample(0, &kBoth);
    mix.assignSample(1, &kBoth);
    mix.setVolume(1, 0.5);
    mix.trigger(0, 127);
    mix.trigger(1, 127);
    out = render(mix, 1);
    check(near(out[0].left, 0.75) && near(out[0].right, 0.375),
          "37 Simultaneous voices sum with their own volumes");

    // processBlock adds into the buffer like SampleVoiceManager
    std::vector<StereoFrame> preset(1, StereoFrame{0.25, 0.25});
    mix.processBlock(preset.data(), 1);
    check(near(preset[0].left, 1.0) && near(preset[0].right, 0.625),
          "37b processBlock mixes into existing buffer contents");

    // 38: same scenario rendered with different block sizes
    auto scenario = [](std::size_t blockSize) {
        DrumEngine<4> e;
        e.setReleaseFadeFrames(7);
        e.assignSample(0, &kRamp16);
        e.assignSample(1, &kBoth);
        e.setChokeGroup(0, 1);
        e.setChokeGroup(1, 1);
        e.trigger(0, 100);
        e.trigger(1, 90);
        std::vector<StereoFrame> outBuffer(64);
        for (std::size_t pos = 0; pos < outBuffer.size(); pos += blockSize) {
            const std::size_t n =
                std::min(blockSize, outBuffer.size() - pos);
            e.processBlock(outBuffer.data() + pos, n);
        }
        return outBuffer;
    };
    const auto reference = scenario(64);
    bool identical = true;
    for (std::size_t size : {1u, 3u, 7u, 16u, 63u}) {
        const auto other = scenario(size);
        for (std::size_t i = 0; i < reference.size(); ++i) {
            identical = identical &&
                        reference[i].left == other[i].left &&
                        reference[i].right == other[i].right;
        }
    }
    check(identical, "38 Block sizes 1, 3, 7, 16, 63, 64 give identical audio");

    // 39
    DrumEngine<4> done;
    done.assignSample(0, &kShort);
    done.trigger(0, 127);
    render(done, 4);
    out = render(done, 32);
    bool silent = true;
    for (const auto& f : out) {
        silent = silent && f.left == 0.0 && f.right == 0.0;
    }
    check(silent, "39 Processing after completion produces silence");

    // 40
    DrumEngine<4> edge;
    const bool badRegion = !edge.assignSample(0, &kRamp16, 8, 17) &&
                           !edge.assignSample(0, &kRamp16, 8, 8);
    edge.assignSample(0, &kRamp16, 15, 16);      // last frame only
    edge.setReverse(0, true);
    edge.setPitch(0, 48.0);                      // fastest rate
    edge.trigger(0, 127);
    out = render(edge, 3);
    const bool lastFrame = near(out[0].left, 15.0) && near(out[1].left, 0.0);
    edge.processBlock(nullptr, 16);              // ignored
    edge.processBlock(out.data(), 0);            // ignored
    std::size_t slots[2]{};
    check(badRegion && lastFrame &&
          edge.padVoices(0, nullptr, 2) == 0 &&
          edge.padVoices(99, slots, 2) == 0 &&
          edge.voiceOwner(99) == npos &&
          !edge.isPadActive(99),
          "40 Invalid regions, last-frame playback and bad arguments are safe");

    // 41: real-time safety - no heap allocation while triggering, choking,
    // retriggering, fading and processing.
    {
        DrumEngine<8> rt;
        rt.setReleaseFadeFrames(16);
        for (std::size_t pad = 0; pad < 27; ++pad) {
            rt.assignSample(pad, &kBoth);
            rt.setChokeGroup(pad, pad % 3);
        }
        StereoFrame block[32]{};
        const std::size_t before = g_allocations;
        for (int round = 0; round < 50; ++round) {
            rt.trigger(static_cast<std::size_t>(round) % 27, 100);
            rt.trigger(static_cast<std::size_t>(round * 7) % 27, 60);
            rt.processBlock(block, 32);
        }
        rt.releaseAll();
        rt.processBlock(block, 32);
        rt.stopAll();
        // Read the counter before check() builds its std::string argument.
        const bool noAllocation = g_allocations == before;
        check(noAllocation,
              "41 trigger/processBlock/release/stop perform no heap allocation");
    }

    // Fade length is clamped to at least one frame.
    edge.setReleaseFadeFrames(0);
    check(edge.releaseFadeFrames() == 1,
          "Release fade length is clamped to at least one frame");
}

}  // namespace

int main() {
    std::cout << "DATABREAKs Engine 019 - 27-Pad Drum Voice Integration\n";
    std::cout << "(host-side test; not FM-1 firmware)\n";

    basicIntegration();
    ownership();
    choke();
    retrigger();
    activity();
    audio();

    std::cout << "\n------------------------------\n";
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';

    if (failed != 0) {
        std::cout << "ENGINE 019 FAILED\n";
        return EXIT_FAILURE;
    }

    std::cout << "ENGINE 019 PASSED\n";
    return EXIT_SUCCESS;
}
