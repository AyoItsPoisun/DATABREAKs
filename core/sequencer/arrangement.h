
#pragma once

// DATABREAKs Engine 028 - song arrangement foundation (host-side C++17).
//
// An Arrangement is an ordered chain of pattern references, each played a
// number of times (repeats = whole loops of that pattern). It is a
// FOUNDATION for song mode, not a song editor: no per-entry tempo, mutes,
// automation or audio.
//
// Playback rules (implemented by the Sequencer):
//   * entry i plays pattern `pattern` for `repeats` loops, starting at that
//     pattern's beginning; the transition to entry i + 1 happens exactly on
//     the loop boundary after the last repeat (sample-exact, independent of
//     the block size);
//   * consecutive entries referring to the same pattern simply continue
//     looping it (no restart, no reload);
//   * at the end of the last entry, `end` decides:
//       Stop      - sequenced notes are released at the end boundary and the
//                   transport stops exactly there (the clock is left on the
//                   end sample);
//       Loop      - the arrangement starts again at entry 0;
//       HoldLast  - the last pattern keeps looping; the arrangement is
//                   finished (no further transitions).
//
// Validation: at least one entry, at most MaxEntries; every pattern id
// valid and stored (used) in the bank; repeats 1..MaxRepeats.
//
// Fixed storage: MaxEntries x 4 bytes + header. No allocation.

#include <array>
#include <cstddef>
#include <cstdint>

namespace databreaks {

enum class ArrangementEnd : std::uint8_t { Stop, Loop, HoldLast };

struct ArrangementEntry {
    std::uint16_t pattern = 0;
    std::uint16_t repeats = 1;
};

class Arrangement {
public:
    static constexpr std::size_t MaxEntries = 64;
    static constexpr int MaxRepeats = 999;

    // Appends an entry; false (unchanged) if full or repeats out of range.
    // The pattern id is checked against the bank by validate().
    bool append(int pattern, int repeats) {
        if (count_ == MaxEntries || pattern < 0 || pattern > 65535 || repeats < 1 ||
            repeats > MaxRepeats) {
            return false;
        }
        entries_[count_++] = {static_cast<std::uint16_t>(pattern),
                              static_cast<std::uint16_t>(repeats)};
        return true;
    }

    void clear() { count_ = 0; }
    void setEnd(ArrangementEnd end) { end_ = end; }
    bool setEnd(int end) {
        if (end < 0 || end > static_cast<int>(ArrangementEnd::HoldLast)) return false;
        end_ = static_cast<ArrangementEnd>(end);
        return true;
    }

    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const ArrangementEntry& at(std::size_t i) const { return entries_[i]; }
    ArrangementEnd end() const { return end_; }

    // `isUsed(id)` tells whether a pattern id exists in the bank.
    template <typename IsUsed>
    bool validate(IsUsed isUsed) const {
        if (count_ == 0) return false;
        for (std::size_t i = 0; i < count_; ++i) {
            const ArrangementEntry& e = entries_[i];
            if (e.repeats < 1 || e.repeats > MaxRepeats || !isUsed(static_cast<int>(e.pattern))) {
                return false;
            }
        }
        return true;
    }

    bool operator==(const Arrangement& o) const {
        if (count_ != o.count_ || end_ != o.end_) return false;
        for (std::size_t i = 0; i < count_; ++i) {
            if (entries_[i].pattern != o.entries_[i].pattern ||
                entries_[i].repeats != o.entries_[i].repeats) {
                return false;
            }
        }
        return true;
    }

private:
    std::array<ArrangementEntry, MaxEntries> entries_{};
    std::size_t count_ = 0;
    ArrangementEnd end_ = ArrangementEnd::Stop;
};

} // namespace databreaks
