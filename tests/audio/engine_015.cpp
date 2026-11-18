
#include "audio_engine.h"
#include "sample_player.h"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

using namespace databreaks;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) {
        std::cout << "[PASS] " << message << '\n';
    } else {
        std::cout << "[FAIL] " << message << '\n';
        ++failures;
    }
}

bool closeTo(double a, double b, double tolerance = 0.000001) {
    return std::abs(a - b) <= tolerance;
}

std::vector<StereoFrame> makeSample(std::size_t count) {
    std::vector<StereoFrame> sample;

    for (std::size_t i = 0; i < count; ++i) {
        const double value = static_cast<double>(i);
        sample.push_back({value, -value});
    }

    return sample;
}

} // namespace

int main() {
    const auto sample = makeSample(8);
    SamplePlayer player;

    player.attachSample(sample);
    player.trigger();

    StereoFrame output[8]{};
    const std::size_t produced =
        player.processBlock(output, 8);

    check(produced == 8, "Full sample playback produces every frame");
    check(closeTo(output[0].left, 0.0) &&
          closeTo(output[7].left, 7.0),
          "Forward playback preserves sample order");
    check(closeTo(output[3].right, -3.0),
          "Stereo channels remain independent");
    check(!player.isPlaying(),
          "Player stops after reaching the sample end");

    player.setRegion(1, 4);
    player.setReverse(false);
    player.trigger();

    StereoFrame regionOutput[5]{};
    player.processBlock(regionOutput, 5);

    check(closeTo(regionOutput[0].left, 1.0) &&
          closeTo(regionOutput[1].left, 2.0) &&
          closeTo(regionOutput[2].left, 3.0),
          "Start and end points restrict playback");
    check(closeTo(regionOutput[3].left, 0.0),
          "Playback outputs silence after the region ends");

    player.setReverse(true);
    player.trigger();

    StereoFrame reverseOutput[4]{};
    player.processBlock(reverseOutput, 4);

    check(closeTo(reverseOutput[0].left, 3.0) &&
          closeTo(reverseOutput[1].left, 2.0) &&
          closeTo(reverseOutput[2].left, 1.0),
          "Reverse playback reads the region backwards");
    check(closeTo(reverseOutput[3].left, 0.0),
          "Reverse playback stops at the region start");

    player.setRegion(0, 8);
    player.setReverse(false);
    player.setPitchSemitones(12.0);
    player.trigger();

    StereoFrame pitchedOutput[4]{};
    player.processBlock(pitchedOutput, 4);

    check(closeTo(pitchedOutput[0].left, 0.0) &&
          closeTo(pitchedOutput[1].left, 2.0) &&
          closeTo(pitchedOutput[2].left, 4.0),
          "One-octave pitch increase doubles playback rate");

    player.setPitchSemitones(0.0);
    player.setPlaybackRate(0.5);
    player.trigger();

    StereoFrame interpolatedOutput[4]{};
    player.processBlock(interpolatedOutput, 4);

    check(closeTo(interpolatedOutput[0].left, 0.0) &&
          closeTo(interpolatedOutput[1].left, 0.5) &&
          closeTo(interpolatedOutput[2].left, 1.0) &&
          closeTo(interpolatedOutput[3].left, 1.5),
          "Fractional playback rate uses linear interpolation");

    player.stop();
    check(!player.isPlaying(), "Manual stop works");

    player.setRegion(5, 5);
    player.trigger();

    check(!player.isPlaying(), "Empty playback region is rejected");

    player.setRegion(0, sample.size());
    player.trigger();
    player.detachSample();

    check(!player.isPlaying(), "Detaching a sample stops playback");

    SamplePlayer emptyPlayer;
    emptyPlayer.trigger();

    check(!emptyPlayer.isPlaying(),
          "Triggering without a sample is safe");

    check(emptyPlayer.processBlock(nullptr, 8) == 0,
          "Null output pointer is handled safely");

    std::cout << "\nEngine 015 tests complete. Failures: "
              << failures << '\n';

    return failures == 0 ? 0 : 1;
}
