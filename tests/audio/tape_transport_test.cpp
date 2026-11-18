
#include "../../core/audio/audio_engine.h"
#include "../../core/audio/tape_transport.h"

#include <cmath>
#include <iostream>
#include <vector>

using namespace databreaks;

int main() {
    int failures = 0;

    auto check = [&](bool condition, const char* message) {
        if (condition) {
            std::cout << "[PASS] " << message << '\n';
        } else {
            std::cout << "[FAIL] " << message << '\n';
            ++failures;
        }
    };

    std::vector<StereoFrame> recording;

    for (int i = 0; i < SAMPLE_RATE; ++i) {
        const double sample =
            std::sin(2.0 * PI * 440.0 * i / SAMPLE_RATE);

        recording.push_back({sample, sample});
    }

    // 7.5 IPS: half speed, twice the duration.
    {
        TapeTransport transport;
        transport.setRecording(recording);
        transport.setSpeed(TapeSpeed::IPS_7_5);

        const auto output = transport.render();

        check(
            output.size() == recording.size() * 2,
            "7.5 IPS doubles playback duration"
        );

        check(
            std::abs(output[100].left - recording[50].left) < 1e-9,
            "7.5 IPS reads the recording at half speed"
        );
    }

    // 15 IPS: normal speed.
    {
        TapeTransport transport;
        transport.setRecording(recording);
        transport.setSpeed(TapeSpeed::IPS_15);

        const auto output = transport.render();

        check(
            output.size() == recording.size(),
            "15 IPS preserves playback duration"
        );

        check(
            std::abs(output[100].left - recording[100].left) < 1e-9,
            "15 IPS plays at the original speed"
        );
    }

    // 30 IPS: double speed, half the duration.
    {
        TapeTransport transport;
        transport.setRecording(recording);
        transport.setSpeed(TapeSpeed::IPS_30);

        const auto output = transport.render();

        check(
            output.size() == recording.size() / 2,
            "30 IPS halves playback duration"
        );

        check(
            std::abs(output[100].left - recording[200].left) < 1e-9,
            "30 IPS reads the recording at double speed"
        );
    }

    // Empty recording should produce empty playback.
    {
        TapeTransport transport;

        check(
            transport.render().empty(),
            "Empty tape produces no audio"
        );
    }

    // Wow and flutter must not create invalid output.
    {
        TapeTransport transport;

        transport.setRecording(recording);
        transport.setSpeed(TapeSpeed::IPS_15);
        transport.setWow(1.0);
        transport.setFlutter(1.0);

        const auto output = transport.render();

        bool finite = true;

        for (const auto& frame : output) {
            if (!std::isfinite(frame.left) ||
                !std::isfinite(frame.right)) {
                finite = false;
                break;
            }
        }

        check(
            finite,
            "Wow and flutter produce finite playback audio"
        );
    }

    std::cout
        << "\nTransport tests complete. Failures: "
        << failures << '\n';

    return failures == 0 ? 0 : 1;
}
