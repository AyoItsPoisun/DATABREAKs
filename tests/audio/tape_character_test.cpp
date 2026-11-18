
#include "../../core/audio/audio_engine.h"
#include "../../core/audio/tape_machine.h"

#include <cmath>
#include <iostream>

using namespace databreaks;

int main() {
    int failures = 0;

    auto check = [&](bool condition, const char* name) {
        if (condition) {
            std::cout << "[PASS] " << name << '\n';
        } else {
            std::cout << "[FAIL] " << name << '\n';
            ++failures;
        }
    };

    constexpr int samples = SAMPLE_RATE;

    // Test 1: Bypass.
    {
        TapeMachine tape;
        tape.setEnabled(false);

        const StereoFrame input{0.5, 0.0};
        const auto output = tape.process(input);

        check(
            output.left == input.left &&
            output.right == input.right,
            "Bypass preserves the original signal"
        );
    }

    // Test 2: Crosstalk.
    {
        TapeMachine tape;
        tape.setCrosstalk(1.0);

        double rightEnergy = 0.0;

        for (int i = 0; i < samples; ++i) {
            const double value =
                std::sin(2.0 * PI * 440.0 * i / SAMPLE_RATE) * 0.5;

            const auto output = tape.process({value, 0.0});
            rightEnergy += std::abs(output.right);
        }

        check(
            rightEnergy > 0.01,
            "Crosstalk transfers signal between channels"
        );
    }

    // Test 3: Print-through.
    {
        TapeMachine reference;
        TapeMachine printed;

        printed.setPrintThrough(1.0);

        double difference = 0.0;

        for (int i = 0; i < samples; ++i) {
            const double value =
                std::sin(2.0 * PI * 440.0 * i / SAMPLE_RATE) * 0.5;

            const StereoFrame input{value, value};

            const auto a = reference.process(input);
            const auto b = printed.process(input);

            difference +=
                std::abs(a.left - b.left) +
                std::abs(a.right - b.right);
        }

        check(
            difference > 0.01,
            "Print-through changes the processed signal"
        );
    }

    // Test 4: Aging.
    {
        TapeMachine fresh;
        TapeMachine aged;

        fresh.setHiss(1.0);
        aged.setHiss(1.0);
        aged.setAge(1.0);

        double difference = 0.0;

        for (int i = 0; i < samples; ++i) {
            const double value =
                std::sin(2.0 * PI * 440.0 * i / SAMPLE_RATE) * 0.5;

            const StereoFrame input{value, value};

            const auto a = fresh.process(input);
            const auto b = aged.process(input);

            difference +=
                std::abs(a.left - b.left) +
                std::abs(a.right - b.right);
        }

        check(
            difference > 0.01,
            "Tape aging changes the processed signal"
        );
    }

    // Test 5: Reset.
    {
        TapeMachine tape;

        for (int i = 0; i < samples; ++i) {
            tape.process({0.5, -0.5});
        }

        tape.reset();

        tape.setHiss(0.0);
        tape.setSaturation(0.0);
        tape.setHeadBump(0.0);
        tape.setCompression(0.0);
        tape.setCrosstalk(0.0);
        tape.setPrintThrough(0.0);
        tape.setAge(0.0);

        const auto output = tape.process({0.0, 0.0});

        check(
            std::abs(output.left) < 1e-9 &&
            std::abs(output.right) < 1e-9,
            "Reset clears internal processing buffers"
        );
    }

    std::cout
        << "\nTape character tests complete. Failures: "
        << failures << '\n';

    return failures == 0 ? 0 : 1;
}
