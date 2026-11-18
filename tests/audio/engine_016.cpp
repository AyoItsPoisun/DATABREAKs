
#include "sample_slice_map.h"

#include <cstdlib>
#include <iostream>

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

int main() {
    std::cout << "DATABREAKs Engine 016 - Sample Slice Map\n\n";

    SampleSliceMap map;

    check(map.build(16, 4),
          "Build four slices");

    check(map.sliceCount() == 4,
          "Correct slice count");

    check(map.sampleFrames() == 16,
          "Correct sample length");

    SampleSlice slice;

    check(map.getSlice(0, slice) &&
          slice.start == 0 && slice.end == 4,
          "First slice boundaries");

    check(map.getSlice(1, slice) &&
          slice.start == 4 && slice.end == 8,
          "Second slice boundaries");

    check(map.getSlice(2, slice) &&
          slice.start == 8 && slice.end == 12,
          "Third slice boundaries");

    check(map.getSlice(3, slice) &&
          slice.start == 12 && slice.end == 16,
          "Fourth slice boundaries");

    check(map.build(10, 3),
          "Build uneven three-slice sample");

    check(map.getSlice(0, slice) &&
          slice.start == 0 && slice.end == 3,
          "Uneven slice one");

    check(map.getSlice(1, slice) &&
          slice.start == 3 && slice.end == 6,
          "Uneven slice two");

    check(map.getSlice(2, slice) &&
          slice.start == 6 && slice.end == 10,
          "Uneven slice three");

    std::size_t totalCovered = 0;
    bool contiguous = true;

    for (std::size_t i = 0; i < map.sliceCount(); ++i) {
        const SampleSlice* current = map.sliceAt(i);

        if (current == nullptr || !current->valid()) {
            contiguous = false;
            break;
        }

        totalCovered += current->length();

        if (i > 0) {
            const SampleSlice* previous = map.sliceAt(i - 1);

            if (previous->end != current->start) {
                contiguous = false;
            }
        }
    }

    check(contiguous,
          "Slices are contiguous and non-empty");

    check(totalCovered == 10,
          "Every sample frame is covered");

    check(map.setBoundary(1, 4),
          "Move first custom boundary");

    check(map.getSlice(0, slice) &&
          slice.start == 0 && slice.end == 4,
          "Updated left slice");

    check(map.getSlice(1, slice) &&
          slice.start == 4 && slice.end == 6,
          "Updated right slice");

    check(!map.setBoundary(0, 2),
          "Reject first boundary index");

    check(!map.setBoundary(3, 5),
          "Reject boundary beyond slice count");

    check(!map.setBoundary(1, 0),
          "Reject boundary before slice");

    check(!map.setBoundary(1, 6),
          "Reject boundary at neighboring slice end");

    check(!map.getSlice(99, slice),
          "Reject invalid slice index");

    check(map.sliceAt(99) == nullptr,
          "Invalid slice pointer returns null");

    check(!map.build(0, 4),
          "Reject empty sample");

    check(!map.build(16, 0),
          "Reject zero slices");

    check(!map.build(3, 4),
          "Reject more slices than sample frames");

    check(map.empty(),
          "Invalid build leaves map empty");

    check(map.build(48000, 16),
          "Build sixteen slices from one second of audio");

    check(map.sliceCount() == 16,
          "Sixteen-slice map has correct count");

    check(map.sliceAt(15)->end == 48000,
          "Final slice ends at exact sample boundary");

    map.clear();

    check(map.empty() && map.sampleFrames() == 0,
          "Clear resets slice map");

    std::cout << "\n------------------------------\n";
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';

    if (failed != 0) {
        std::cout << "ENGINE 016 FAILED\n";
        return EXIT_FAILURE;
    }

    std::cout << "ENGINE 016 PASSED\n";
    return EXIT_SUCCESS;
}
