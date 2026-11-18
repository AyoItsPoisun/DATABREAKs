
#pragma once

#include <cstddef>
#include <vector>

namespace databreaks {

struct SampleSlice {
    std::size_t start = 0;
    std::size_t end = 0;

    std::size_t length() const {
        return end - start;
    }

    bool valid() const {
        return end > start;
    }
};

class SampleSliceMap {
public:
    bool build(std::size_t sampleFrames, std::size_t sliceCount) {
        clear();

        if (sampleFrames == 0 ||
            sliceCount == 0 ||
            sliceCount > sampleFrames) {
            return false;
        }

        slices.resize(sliceCount);

        const std::size_t baseSize = sampleFrames / sliceCount;
        const std::size_t remainder = sampleFrames % sliceCount;

        for (std::size_t i = 0; i < sliceCount; ++i) {
            const std::size_t start =
                i * baseSize + (i * remainder) / sliceCount;

            const std::size_t next =
                (i + 1) * baseSize + ((i + 1) * remainder) / sliceCount;

            slices[i] = {start, next};
        }

        totalFrames = sampleFrames;
        return true;
    }

    bool setBoundary(std::size_t boundaryIndex,
                     std::size_t framePosition) {
        if (boundaryIndex == 0 ||
            boundaryIndex >= slices.size()) {
            return false;
        }

        SampleSlice& left = slices[boundaryIndex - 1];
        SampleSlice& right = slices[boundaryIndex];

        if (framePosition <= left.start ||
            framePosition >= right.end) {
            return false;
        }

        left.end = framePosition;
        right.start = framePosition;

        return true;
    }

    bool getSlice(std::size_t index, SampleSlice& result) const {
        if (index >= slices.size()) {
            return false;
        }

        result = slices[index];
        return true;
    }

    const SampleSlice* sliceAt(std::size_t index) const {
        if (index >= slices.size()) {
            return nullptr;
        }

        return &slices[index];
    }

    std::size_t sliceCount() const {
        return slices.size();
    }

    std::size_t sampleFrames() const {
        return totalFrames;
    }

    bool empty() const {
        return slices.empty();
    }

    void clear() {
        slices.clear();
        totalFrames = 0;
    }

private:
    std::vector<SampleSlice> slices;
    std::size_t totalFrames = 0;
};

} // namespace databreaks
