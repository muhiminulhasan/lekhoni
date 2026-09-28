#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lekho::bn {

// Remembers which candidates the user chose for a typed key so those words
// rank up next time. Privacy contract (see docs/ARCHITECTURE.md): the typed
// key is stored only as a 128-bit hash, the chosen words as text, plus a
// counter and a coarse day stamp. No document text is ever stored. The memory
// is optional; a missing or corrupt file never blocks typing.
//
// Ranking model: every entry scores count x 2^(-age/halfLife) against the
// current day, so recent picks outrank old ones and stale habits fade. Each
// key keeps up to kWordsPerKey words; a full group evicts its lowest-scoring
// word when a newcomer arrives, and the global cap evicts the lowest-scoring
// entry anywhere.
class SelectionMemory final {
public:
    // dayStamp is a coarse day counter supplied by the caller (the core keeps
    // no clock). Re-recording a known word bumps its counter and refreshes
    // the stamp; an unknown word joins the key's group.
    void record(std::string_view key, std::string_view word, std::uint32_t dayStamp);

    // Words remembered for the key, best (highest decayed score) first.
    // currentDay drives the recency decay; passing the day the entries were
    // recorded disables decay.
    [[nodiscard]] std::vector<std::string> find(std::string_view key,
                                                std::uint32_t currentDay) const;

    // Binary image. BGS2 is the current format; a BGS1 image (one word per
    // key) is imported as count-preserving entries with no recency stamp.
    // load() clears the memory on any inconsistency.
    [[nodiscard]] std::string save() const;
    bool load(std::string bytes);

    // Number of stored (key, word) entries.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    static constexpr std::size_t kMaximumEntries = 20000;
    static constexpr std::size_t kWordsPerKey = 3;
    static constexpr std::uint32_t kDecayHalfLifeDays = 14;

private:
    struct Entry {
        std::uint64_t hashLow = 0;
        std::uint64_t hashHigh = 0;
        std::string word;
        std::uint32_t count = 0;
        std::uint32_t day = 0;
    };

    // A day stamp of 0 means "imported, no recency information": such
    // entries do not decay.
    [[nodiscard]] static double decayedScore(const Entry& entry, std::uint32_t currentDay);

    // entries_ is kept sorted by (hashHigh, hashLow) so every key's group is
    // contiguous.
    std::vector<Entry> entries_;
};

} // namespace lekho::bn
