#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lekho::bn {

// A compact word-bigram/trigram next-word model. Loaded from an optional
// BGTG1 image (tools/build_trigrams.py) that maps a context word pair to its
// most frequent continuations, sorted by frequency at build time.
//
// The model is a ranking aid only: a missing or corrupt file disables the
// predictions and never blocks typing. Hashes are stored, but the context
// words themselves are reconstructible from the file by design — this model
// ships curated corpus statistics, not user data; user personalization lives
// in SelectionMemory (BGS2) and never here.
class TrigramModel final {
public:
    bool load(std::string bytes);
    bool isLoaded() const noexcept { return loaded_; }

    // Up to `limit` continuations for the context (w1, w2), best first.
    // Falls back from the trigram context to the bigram tail: a hit for
    // (w2) is returned when (w1, w2) is unknown. Either word may be empty;
    // with only w2 set this behaves as a pure bigram lookup.
    [[nodiscard]] std::vector<std::string> next(std::string_view w1,
                                                std::string_view w2,
                                                std::size_t limit) const;

private:
    struct Context {
        std::uint64_t hash = 0;
        std::uint32_t offset = 0;  // entry-list offset in the entries region
        std::uint8_t count = 0;
    };

    // Returns continuations from one sorted region, or an empty vector.
    [[nodiscard]] std::vector<std::string> lookupRegion(const std::vector<Context>& region,
                                                        std::uint64_t hash,
                                                        std::size_t limit) const;

    std::vector<Context> bigrams_;   // sorted by hash
    std::vector<Context> trigrams_;  // sorted by hash
    std::string entries_;            // concatenated (len,word) continuation lists
    bool loaded_ = false;
};

} // namespace lekho::bn
