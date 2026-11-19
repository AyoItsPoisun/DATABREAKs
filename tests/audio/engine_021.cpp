
// DATABREAKs Engine 021 - unified audio pipeline integration tests.
//
// Host-side assertion tests for core/audio/audio_pipeline.h: DrumEngine
// routed through the drum bus level and the StereoMixer master stage.
// Exits non-zero if any check fails. Not FM-1 firmware.

#include "audio_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <vector>

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

bool near(double a, double b, double tolerance = 1e-12) {
    return std::abs(a - b) <= tolerance;
}

std::vector<StereoFrame> constant(std::size_t frames, double l, double r) {
    return std::vector<StereoFrame>(frames, StereoFrame{l, r});
}

std::vector<StereoFrame> padSample(std::size_t pad) {
    const std::size_t length = 300 + pad * 41;
    std::vector<StereoFrame> s(length);
    for (std::size_t i = 0; i < length; ++i) {
        const double v =
            0.3 * std::sin(0.013 * static_cast<double>((pad + 2) * (i + 1)));
        s[i] = {v, 0.5 * v};
    }
    return s;
}

const auto kA = constant(48000, 0.5, 0.25);
const auto kLeftQuarter = constant(48000, 0.25, 0.0);
const auto kRightHalf = constant(48000, 0.0, 0.5);
const auto kEighth = constant(48000, 0.125, 0.125);
const auto kLeftOne = constant(48000, 1.0, 0.0);
const auto kRightOne = constant(48000, 0.0, 1.0);
const auto kShort = constant(3, 0.5, 0.5);

using Pipeline = AudioPipeline<16>;

template <typename P>
std::vector<StereoFrame> render(P& p, std::size_t frames) {
    std::vector<StereoFrame> out(frames);
    p.render(out.data(), frames);
    return out;
}

bool silent(const std::vector<StereoFrame>& out) {
    for (const auto& f : out) {
        if (f.left != 0.0 || f.right != 0.0) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
void routing() {
    std::cout << "\n-- Routing and levels --\n";

    // 1
    {
        auto pStorage = std::make_unique<Pipeline>();  // ~0.9 MB: not on the stack
        Pipeline& p = *pStorage;
        p.drums().assignSample(0, &kA);
        p.drums().trigger(0, 127);
        const auto out = render(p, 4);
        check(Pipeline::Drums::padCount() == 27 &&
              Pipeline::Drums::voiceCapacity() == 16 &&
              near(p.masterLevel(), 1.0) && near(p.drumLevel(), 1.0) &&
              near(out[0].left, 0.5) && near(out[0].right, 0.25) &&
              near(out[3].left, 0.5),
              "01 Drum audio reaches the unified output at unity gain");
    }

    // 2
    {
        auto pStorage = std::make_unique<Pipeline>();  // ~0.9 MB: not on the stack
        Pipeline& p = *pStorage;
        p.drums().assignSample(0, &kLeftQuarter);
        p.drums().assignSample(1, &kRightHalf);
        p.drums().assignSample(26, &kEighth);
        p.drums().trigger(0, 127);
        p.drums().trigger(1, 127);
        p.drums().trigger(26, 127);
        const auto out = render(p, 2);
        check(near(out[1].left, 0.375) && near(out[1].right, 0.625),
              "02 Pads 0, 1 and 26 sum exactly (0.375 L, 0.625 R)");
    }

    // 3
    {
        auto pStorage = std::make_unique<Pipeline>();  // ~0.9 MB: not on the stack
        Pipeline& p = *pStorage;
        p.setDrumLevel(0.7);
        p.setMasterLevel(0.9);
        p.drums().assignSample(0, &kLeftOne);
        p.drums().assignSample(1, &kEighth);
        p.drums().trigger(0, 100);
        const auto left = render(p, 8);
        p.drums().stopAll();
        p.drums().trigger(1, 77);
        const auto both = render(p, 8);
        bool ok = true;
        for (std::size_t i = 0; i < 8; ++i) {
            ok = ok && left[i].right == 0.0 && left[i].left > 0.0 &&
                 both[i].left == both[i].right;
        }
        check(ok, "03 Left-only stays left; equal channels stay exactly equal");
    }

    // 4
    {
        auto pStorage = std::make_unique<Pipeline>();  // ~0.9 MB: not on the stack
        Pipeline& p = *pStorage;
        p.drums().assignSample(0, &kA);
        p.drums().trigger(0, 127);
        p.setDrumLevel(0.5);
        auto out = render(p, 1);
        const bool half = near(out[0].left, 0.25) && near(out[0].right, 0.125);
        p.setDrumLevel(0.0);
        out = render(p, 1);
        const bool zero = out[0].left == 0.0 && out[0].right == 0.0;
        p.setDrumLevel(3.0);
        const bool clamped = near(p.drumLevel(), 2.0);
        p.setDrumLevel(std::numeric_limits<double>::quiet_NaN());
        check(half && zero && clamped && near(p.drumLevel(), 2.0),
              "04 Drum level scales drums; 0 mutes; clamps to 2; NaN ignored");
    }

    // 5
    {
        auto pStorage = std::make_unique<Pipeline>();  // ~0.9 MB: not on the stack
        Pipeline& p = *pStorage;
        p.drums().assignSample(0, &kA);
        p.drums().trigger(0, 127);
        p.setMasterLevel(0.5);
        auto out = render(p, 1);
        const bool half = near(out[0].left, 0.25) && near(out[0].right, 0.125);
        p.setDrumLevel(0.5);
        out = render(p, 1);
        const bool once = near(out[0].left, 0.125) && near(out[0].right, 0.0625);
        p.setMasterLevel(std::numeric_limits<double>::infinity());
        check(half && once && near(p.masterLevel(), 0.5),
              "05 Master scales the mix; drum x master applied exactly once");
    }

    // Gain staging including velocity.
    {
        auto pStorage = std::make_unique<Pipeline>();  // ~0.9 MB: not on the stack
        Pipeline& p = *pStorage;
        p.drums().assignSample(0, &kLeftOne);
        p.drums().setVolume(0, 0.8);
        p.drums().setVelocitySensitivity(0, 1.0);
        p.setDrumLevel(0.5);
        p.setMasterLevel(0.75);
        p.drums().trigger(0, 64);
        const auto out = render(p, 1);
        check(near(out[0].left, 1.0 * 0.8 * (64.0 / 127.0) * 0.5 * 0.75),
              "05b Gain staging: sample x pad x velocity x drum x master");
    }

    // 6
    {
        auto pStorage = std::make_unique<Pipeline>();  // ~0.9 MB: not on the stack
        Pipeline& p = *pStorage;
        p.drums().assignSample(0, &kA);
        p.drums().trigger(0, 127);
        p.setDrumLevel(0.5);
        p.setMasterLevel(0.5);
        std::vector<StereoFrame> bus(2, StereoFrame{0.1, -0.1});
        p.render(bus.data(), 2);
        const bool mastered = near(bus[0].left, (0.1 + 0.25) * 0.5) &&
                              near(bus[0].right, (-0.1 + 0.125) * 0.5);
        std::vector<StereoFrame> raw(2, StereoFrame{0.1, -0.1});
        p.mixSources(raw.data(), 2);
        const bool unmastered = near(raw[0].left, 0.1 + 0.25) &&
                                near(raw[0].right, -0.1 + 0.125);
        auto idleStorage = std::make_unique<Pipeline>();  // ~0.9 MB: not on the stack
        Pipeline& idle = *idleStorage;
        std::vector<StereoFrame> keep(2, StereoFrame{0.3, -0.2});
        idle.render(keep.data(), 2);
        check(mastered && unmastered && near(keep[1].left, 0.3) &&
              near(keep[1].right, -0.2),
              "06 Existing bus audio is summed with drums, never overwritten");
    }

    // Clipping only when the mix exceeds full scale, symmetric per channel.
    {
        auto pStorage = std::make_unique<Pipeline>();  // ~0.9 MB: not on the stack
        Pipeline& p = *pStorage;
        p.drums().assignSample(0, &kA);
        p.drums().trigger(0, 127);
        render(p, 64);
        const bool noClip = p.clippedSampleCount() == 0;
        p.drums().assignSample(1, &kLeftOne);
        p.drums().assignSample(2, &kLeftOne);
        p.drums().trigger(1, 127);
        p.drums().trigger(2, 127);
        const auto out = render(p, 2);
        check(noClip && near(out[0].left, 1.0) && near(out[0].right, 0.25) &&
              p.clippedSampleCount() == 2,
              "07b Master clips only above full scale (2.5 -> 1.0), and counts it");
    }

    // Invalid input
    {
        auto pStorage = std::make_unique<Pipeline>();  // ~0.9 MB: not on the stack
        Pipeline& p = *pStorage;
        std::vector<StereoFrame> bad(2, StereoFrame{
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity()});
        p.render(bad.data(), 2);
        p.render(nullptr, 64);
        p.render(bad.data(), 0);
        p.mixSources(nullptr, 8);
        p.applyMaster(nullptr, 8);
        check(bad[0].left == 0.0 && bad[0].right == 0.0 &&
              p.invalidSampleCount() == 4,
              "07c NaN/inf become 0 at the master; null and empty calls are safe");
    }
}

// ---------------------------------------------------------------------------
void timingAndBehaviour() {
    std::cout << "\n-- Events, block sizes and drum behaviour --\n";

    // 7
    {
        Pipeline p;
        p.setDrumLevel(0.5);
        p.setMasterLevel(0.5);
        p.drums().assignSample(3, &kLeftOne);
        p.drums().postTrigger(3, 127, 37);
        const auto out = render(p, 64);
        check(out[36].left == 0.0 && near(out[37].left, 0.25) &&
              near(out[63].left, 0.25) &&
              p.drums().processedEventCount() == 1,
              "07 Queued trigger sounds exactly at frame 37 through the pipeline");
    }

    // 8 + 12: same script, any block size, bit-identical
    std::vector<std::vector<StereoFrame>> samples;
    for (std::size_t pad = 0; pad < 27; ++pad) {
        samples.push_back(padSample(pad));
    }
    // Events are posted one block ahead, so a block may hold at most
    // eventCapacity() (255) of them; one event every 29 frames keeps every
    // tested block size (up to 4096 -> 142 events) within that limit.
    std::uint32_t dropped = 0;
    auto scripted = [&samples, &dropped](std::size_t blockSize) {
        static Pipeline p;  // reused; reset below
        const std::uint32_t droppedBefore = p.drums().droppedEventCount();
        p.drums().stopAll();
        p.resetDiagnostics();
        p.setDrumLevel(0.8);
        p.setMasterLevel(0.9);
        p.drums().setReleaseFadeFrames(64);
        p.drums().setStealFadeFrames(16);
        for (std::size_t pad = 0; pad < 27; ++pad) {
            p.drums().assignSample(pad, &samples[pad]);
            p.drums().setChokeGroup(pad, static_cast<unsigned int>(pad % 3));
        }
        const std::size_t total = 24000;
        std::vector<StereoFrame> out(total);
        for (std::size_t pos = 0; pos < total; pos += blockSize) {
            const std::size_t n = std::min(blockSize, total - pos);
            for (std::size_t f = pos; f < pos + n; ++f) {
                if (f % 29 == 0) {
                    p.drums().postTrigger((f / 29) % 27,
                                          static_cast<int>(1 + f % 127),
                                          f - pos);
                }
            }
            p.render(out.data() + pos, n);
        }
        // Let everything finish so the next run starts from silence.
        std::vector<StereoFrame> tail(4096);
        p.render(tail.data(), tail.size());
        dropped += p.drums().droppedEventCount() - droppedBefore;
        return out;
    };
    const auto reference = scripted(64);
    bool sameAcrossSizes = true;
    for (std::size_t size : {1u, 5u, 64u, 333u, 1024u, 4096u}) {
        const auto other = scripted(size);
        for (std::size_t i = 0; i < reference.size(); ++i) {
            sameAcrossSizes = sameAcrossSizes &&
                              reference[i].left == other[i].left &&
                              reference[i].right == other[i].right;
        }
    }
    double energy = 0.0;
    for (const auto& f : reference) {
        energy += f.left * f.left + f.right * f.right;
    }
    check(sameAcrossSizes && energy > 0.0 && dropped == 0,
          "08 Block sizes 1, 5, 64, 333, 1024 and 4096 give identical audio");

    // Posting more events than the queue holds before one render() is the
    // documented overflow case: extra events are dropped and counted.
    {
        Pipeline p;
        p.drums().assignSample(0, &kShort);
        int accepted = 0;
        for (int i = 0; i < 300; ++i) {
            accepted += p.drums().postTrigger(0, 100, 0) ? 1 : 0;
        }
        render(p, 8);
        check(accepted == 255 && p.drums().droppedEventCount() == 45 &&
              p.drums().processedEventCount() == 255,
              "08b Over-full queue drops and counts events (255 kept, 45 dropped)");
    }

    const auto again = scripted(64);
    bool repeat = true;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        repeat = repeat && reference[i].left == again[i].left &&
                 reference[i].right == again[i].right;
    }
    check(repeat, "12 Repeated rendering of the same script is bit-identical");

    // 9
    {
        Pipeline p;
        const bool idle = silent(render(p, 256));
        p.drums().assignSample(0, &kShort);
        p.drums().trigger(0, 127);
        render(p, 3);
        check(idle && silent(render(p, 256)) && p.clippedSampleCount() == 0 &&
              p.drums().activeVoiceCount() == 0,
              "09 Silence with no voices and after the last voice ends");
    }

    // 10: choke and retrigger through the pipeline, with both gains.
    {
        Pipeline p;
        p.setDrumLevel(0.5);
        p.setMasterLevel(0.5);
        p.drums().setReleaseFadeFrames(4);
        p.drums().assignSample(0, &kLeftOne);   // open hat
        p.drums().assignSample(1, &kRightOne);  // closed hat
        p.drums().setChokeGroup(0, 1);
        p.drums().setChokeGroup(1, 1);
        p.drums().trigger(0, 127);
        render(p, 2);
        p.drums().trigger(1, 127);  // chokes pad 0
        auto out = render(p, 6);
        const bool choke =
            near(out[0].left, 0.25) && near(out[1].left, 0.1875) &&
            near(out[2].left, 0.125) && near(out[3].left, 0.0625) &&
            out[4].left == 0.0 && near(out[0].right, 0.25) &&
            near(out[5].right, 0.25) && !p.drums().isPadActive(0);
        p.drums().trigger(1, 127);  // same-pad retrigger
        out = render(p, 5);
        const bool retrigger = near(out[0].right, 0.5) &&
                               near(out[1].right, 0.4375) &&
                               near(out[4].right, 0.25) &&
                               p.drums().padVoiceCount(1) == 1;
        check(choke, "10a Choke fades the open hat (0.25 -> 0) via the pipeline");
        check(retrigger,
              "10b Retrigger: old voice fades while the new one plays at once");
    }
}

// ---------------------------------------------------------------------------
void realTime() {
    std::cout << "\n-- Real-time safety --\n";

    std::vector<std::vector<StereoFrame>> samples;
    for (std::size_t pad = 0; pad < 27; ++pad) {
        samples.push_back(padSample(pad));
    }
    static Pipeline p;
    for (std::size_t pad = 0; pad < 27; ++pad) {
        p.drums().assignSample(pad, &samples[pad]);
        p.drums().setChokeGroup(pad, static_cast<unsigned int>(pad % 4));
    }
    StereoFrame block[128]{};
    const std::size_t before = g_allocations.load();
    for (int i = 0; i < 2000; ++i) {
        p.drums().postTrigger(static_cast<std::size_t>(i * 7) % 27, 100,
                              static_cast<std::size_t>(i) % 128);
        p.drums().trigger(static_cast<std::size_t>(i * 5) % 27, 60);
        p.setDrumLevel(0.5 + 0.25 * (i % 3));
        p.setMasterLevel(0.6 + 0.2 * (i % 2));
        std::fill(block, block + 128, StereoFrame{});
        p.render(block, 1 + static_cast<std::size_t>(i) % 128);
        p.mixSources(block, 16);
        p.applyMaster(block, 16);
    }
    const std::size_t after = g_allocations.load();
    const bool noAllocation = after == before;
    check(noAllocation,
          "11 No heap allocation in render/mixSources/applyMaster/post/levels");
}

}  // namespace

int main() {
    std::cout << "DATABREAKs Engine 021 - Unified Audio Pipeline\n";
    std::cout << "(host-side test; not FM-1 firmware)\n";

    routing();
    timingAndBehaviour();
    realTime();

    std::cout << "\n------------------------------\n";
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';

    if (failed != 0) {
        std::cout << "ENGINE 021 FAILED\n";
        return EXIT_FAILURE;
    }

    std::cout << "ENGINE 021 PASSED\n";
    return EXIT_SUCCESS;
}
