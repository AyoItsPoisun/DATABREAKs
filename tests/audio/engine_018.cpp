
#include <cmath>
#include <iostream>
#include <vector>

#include "drum_pad_bank.h"

using databreaks::DrumPadBank;
using databreaks::StereoFrame;

static int failures = 0;

static void check(bool condition, const char* message) {
    if (condition) {
        std::cout << "[PASS] " << message << '\n';
    } else {
        std::cout << "[FAIL] " << message << '\n';
        ++failures;
    }
}

static bool near(double a, double b, double epsilon = 0.0001) {
    return std::abs(a - b) <= epsilon;
}

int main() {
    using Bank = DrumPadBank<27>;
    Bank bank;

    std::vector<StereoFrame> kick(100, {0.5, 0.5});
    std::vector<StereoFrame> snare(200, {0.25, 0.25});
    std::vector<StereoFrame> hat(300, {0.1, 0.1});

    check(Bank::padCount() == 27, "FM-1 layout contains 27 pads");

    check(
        bank.assignSample(0, &kick),
        "Assign sample to first pad"
    );

    check(
        bank.assignSample(26, &snare, 20, 120),
        "Assign sample to final pad with custom region"
    );

    check(
        !bank.assignSample(27, &kick),
        "Reject pad index outside 27-pad layout"
    );

    check(
        !bank.assignSample(1, &kick, 90, 20),
        "Reject invalid sample region"
    );

    Bank::TriggerEvent event;

    check(
        bank.trigger(0, 127, event),
        "Trigger first pad at full velocity"
    );

    check(
        event.padIndex == 0 &&
        event.sample == &kick &&
        event.startFrame == 0 &&
        event.endFrame == 100,
        "Trigger event contains correct sample and region"
    );

    check(
        near(event.volume, 1.0) &&
        near(event.playbackRate, 1.0),
        "Full velocity uses configured volume and normal pitch"
    );

    bank.setVolume(0, 0.8);
    bank.setVelocitySensitivity(0, 1.0);

    check(
        bank.trigger(0, 64, event),
        "Trigger pad at partial velocity"
    );

    check(
        near(event.volume, 0.8 * (64.0 / 127.0)),
        "Velocity sensitivity scales pad volume"
    );

    bank.setVelocitySensitivity(0, 0.0);

    check(
        bank.trigger(0, 64, event) && near(event.volume, 0.8),
        "Zero velocity sensitivity ignores velocity changes"
    );

    bank.setPitch(0, 12.0);
    check(
        bank.trigger(0, 127, event) &&
        near(event.playbackRate, 2.0),
        "Positive octave pitch doubles playback rate"
    );

    bank.setPitch(0, -12.0);
    check(
        bank.trigger(0, 127, event) &&
        near(event.playbackRate, 0.5),
        "Negative octave pitch halves playback rate"
    );

    bank.setReverse(0, true);
    check(
        bank.trigger(0, 127, event) && event.reverse,
        "Reverse setting appears in trigger event"
    );

    check(
        bank.assignSample(1, &snare, 20, 120),
        "Assign snare sample to second pad"
    );
    check(
        bank.assignSample(2, &hat),
        "Assign hi-hat sample to third pad"
    );

    bank.setChokeGroup(1, 1);
    bank.setChokeGroup(2, 1);

    check(
        bank.trigger(1, 100, event),
        "Trigger first pad in choke group"
    );
    check(bank.isActive(1), "First choke-group pad becomes active");

    check(
        bank.trigger(2, 100, event),
        "Trigger second pad in same choke group"
    );

    check(
        event.chokedPads[1] && !bank.isActive(1) && bank.isActive(2),
        "New trigger chokes the previously active pad"
    );

    check(
        bank.trigger(0, 100, event) && !event.chokedPads[2],
        "Pad outside choke group does not choke another pad"
    );

    check(
        !bank.trigger(0, 0, event),
        "MIDI velocity zero does not trigger a sound"
    );

    bank.stopPad(2);
    check(!bank.isActive(2), "Stop an individual pad");

    bank.trigger(1, 100, event);
    bank.stopAll();
    check(
        !bank.isActive(1) && !bank.isActive(2) && !bank.isActive(0),
        "Stop all tracked pads"
    );

    check(
        bank.setRegion(0, 10, 90),
        "Update an assigned pad's sample region"
    );

    check(
        bank.trigger(0, 127, event) &&
        event.startFrame == 10 &&
        event.endFrame == 90,
        "Updated region is used by the next trigger"
    );

    bank.clearSample(0);
    check(
        !bank.trigger(0, 127, event),
        "Cleared pad cannot be triggered"
    );

    check(
        !bank.setVolume(27, 1.0),
        "Reject settings for invalid pad index"
    );

    if (failures == 0) {
        std::cout << "\nEngine 018: all tests passed.\n";
        return 0;
    }

    std::cout << "\nEngine 018: " << failures
              << " test(s) failed.\n";
    return 1;
}
