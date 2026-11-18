#include "../../core/audio/audio_engine.h"
#include "../../core/audio/tape_machine.h"

#include <cmath>
#include <iostream>
#include <vector>

using namespace databreaks;

bool approximatelyEqual(double a, double b) {
    return std::abs(a - b) < 1e-9;
}

int main() {
    constexpr int testSamples = SAMPLE_RATE * 2;

    int failures = 0;

    auto check = [&](bool condition, const char* message) {
        if (condition) {
            std::cout << "[PASS] " << message << '\n';
        } else {
            std::cout << "[FAIL] " << message << '\n';
            ++failures;
        }
    };

    // Test 1: Exact bypass.
    {
        TapeMachine tape;
        tape.setEnabled(false);

        StereoFrame input{0.35, -0.25};
        StereoFrame output = tape.process(input);

        check(
            approximatelyEqual(output.left, input.left) &&
            approximatelyEqual(output.right, input.right),
            "Bypass preserves the original stereo signal"
        );
    }

    // Test 2: Output remains finite.
    {
        TapeMachine tape;

        tape.setWow(1.0);
        tape.setFlutter(1.0);
        tape.setHiss(1.0);
        tape.setSaturation(1.0);
        tape.setHeadBump(1.0);

        bool finite = true;

        for (int i = 0; i < testSamples; ++i) {
            double sample = std::sin(
                2.0 * PI * 220.0 * i / SAMPLE_RATE
            );

            StereoFrame output = tape.process({
                sample * 0.8,
                sample * 0.6
            });

            if (!std::isfinite(output.left) ||
                !std::isfinite(output.right)) {
                finite = false;
                break;
            }
        }

        check(finite, "Maximum tape settings produce finite audio");
    }

    // Test 3: Saturation changes the signal.
    {
        TapeMachine clean;
        TapeMachine saturated;

        saturated.setSaturation(1.0);

        double difference = 0.0;

        for (int i = 0; i < testSamples; ++i) {
            double sample = std::sin(
                2.0 * PI * 440.0 * i / SAMPLE_RATE
            ) * 0.9;

            StereoFrame input{sample, sample};

            auto a = clean.process(input);
            auto b = saturated.process(input);

            difference += std::abs(a.left - b.left);
        }

        check(
            difference > 1.0,
            "Saturation changes the audio waveform"
        );
    }

    // Test 4: Wow and flutter affect the signal.
    {
        TapeMachine stable;
        TapeMachine modulated;

        modulated.setWow(1.0);
        modulated.setFlutter(1.0);

        double difference = 0.0;

        for (int i = 0; i < testSamples; ++i) {
            double sample = std::sin(
                2.0 * PI * 440.0 * i / SAMPLE_RATE
            );

            StereoFrame input{sample, sample};

            auto a = stable.process(input);
            auto b = modulated.process(input);

            difference += std::abs(a.left - b.left);
        }

        check(
            difference > 0.01,
            "Wow and flutter produce modulation differences"
        );
    }

    // Test 5: Speed settings change the response.
    {
        TapeMachine slow;
        TapeMachine normal;
        TapeMachine fast;

        slow.setSpeed(TapeSpeed::IPS_7_5);
        normal.setSpeed(TapeSpeed::IPS_15);
        fast.setSpeed(TapeSpeed::IPS_30);

        slow.setHiss(0.0);
        normal.setHiss(0.0);
        fast.setHiss(0.0);

        double slowEnergy = 0.0;
        double normalEnergy = 0.0;
        double fastEnergy = 0.0;

        for (int i = 0; i < testSamples; ++i) {
            double sample = std::sin(
                2.0 * PI * 9000.0 * i / SAMPLE_RATE
            );

            StereoFrame input{sample, sample};

            slowEnergy += std::abs(slow.process(input).left);
            normalEnergy += std::abs(normal.process(input).left);
            fastEnergy += std::abs(fast.process(input).left);
        }

        check(
            slowEnergy < normalEnergy &&
            normalEnergy < fastEnergy,
            "Higher tape speeds preserve more high-frequency energy"
        );
    }

    // Test 6: Reset restores a clean processing state.
    {
        TapeMachine tape;

        tape.setHiss(0.0);
        tape.setSaturation(0.0);

        for (int i = 0; i < 1000; ++i) {
            tape.process({0.5, 0.5});
        }

        tape.reset();

        StereoFrame output = tape.process({0.0, 0.0});

        check(
            std::abs(output.left) < 1e-9 &&
            std::abs(output.right) < 1e-9,
            "Reset clears the tape processing state"
        );
    }

    std::cout << "\nTape tests complete. Failures: "
              << failures << '\n';

    return failures == 0 ? 0 : 1;
}