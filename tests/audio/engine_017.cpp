
#include "sample_voice_manager.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace databreaks;

static int passed = 0;
static int failed = 0;

void check(bool condition, const char* description) {
    if (condition) {
        ++passed;
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++failed;
        std::cout << "[FAIL] " << description << '\n';
    }
}

bool near(double a, double b, double tolerance = 0.0001) {
    return std::abs(a - b) <= tolerance;
}

int main() {
    std::cout << "DATABREAKs Engine 017 - Multi-Voice Sample Player\n\n";

    std::vector<StereoFrame> sampleA(8, {0.25, 0.5});
    std::vector<StereoFrame> sampleB(8, {0.75, 0.125});

    SampleVoiceManager<4> manager;

    check(manager.activeVoiceCount() == 0,
          "Starts with no active voices");

    check(manager.trigger(&sampleA),
          "Trigger first sample");

    check(manager.trigger(&sampleB),
          "Trigger second sample");

    check(manager.activeVoiceCount() == 2,
          "Two samples play simultaneously");

    StereoFrame output[2]{};

    manager.processBlock(output, 2);

    check(near(output[0].left, 1.0),
          "Simultaneous voices mix left channel");

    check(near(output[0].right, 0.625),
          "Simultaneous voices mix right channel");

    check(manager.activeVoiceCount() == 2,
          "Voices remain active during playback");

    manager.stopAll();

    check(manager.activeVoiceCount() == 0,
          "Stop all voices");

    check(manager.trigger(&sampleA, 0, 8, 0.5),
          "Trigger sample with half volume");

    StereoFrame quiet[1]{};
    manager.processBlock(quiet, 1);

    check(near(quiet[0].left, 0.125),
          "Individual voice volume is applied");

    manager.stopAll();

    check(manager.trigger(&sampleA, 0, 8, 1.0, 2.0),
          "Trigger sample with adjusted playback rate");

    check(manager.activeVoiceCount() == 1,
          "Pitch-adjusted voice is active");

    manager.stopAll();

    check(manager.trigger(&sampleA, 0, 8, 1.0, 1.0, true),
          "Trigger reverse sample");

    manager.stopAll();

    check(!manager.trigger(nullptr),
          "Reject null sample");

    check(!manager.trigger(&sampleA, 6, 6),
          "Reject empty playback region");

    check(manager.activeVoiceCount() == 0,
          "Invalid trigger does not leave active voice");

    SampleVoiceManager<2> smallManager;

    check(smallManager.trigger(&sampleA),
          "Voice allocation first slot");

    check(smallManager.trigger(&sampleB),
          "Voice allocation second slot");

    check(smallManager.activeVoiceCount() == 2,
          "Voice pool reaches capacity");

    check(smallManager.trigger(&sampleA),
          "Third trigger steals oldest voice");

    check(smallManager.activeVoiceCount() == 2,
          "Voice stealing preserves pool capacity");

    check(SampleVoiceManager<16>::capacity() == 16,
          "Sixteen-voice manager capacity");

    std::cout << "\n------------------------------\n";
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';

    if (failed != 0) {
        std::cout << "ENGINE 017 FAILED\n";
        return EXIT_FAILURE;
    }

    std::cout << "ENGINE 017 PASSED\n";
    return EXIT_SUCCESS;
}
