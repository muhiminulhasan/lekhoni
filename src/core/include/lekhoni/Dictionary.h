#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lekho::bn {

enum class SearchChannel : std::uint8_t {
    Phonetic = 1,
    Bangla = 2,
};

struct Suggestion {
    std::uint32_t wordId = 0;
    std::string word;
    std::string hint;
    std::uint32_t score = 0;
    // Match metadata used for deterministic ranking. `exact` is true only
    // for a phonetic key equal to the complete query.
    std::uint32_t matchedKeyLength = 0;
    std::uint8_t editDistance = 0;
    bool exact = false;
};

// Builds a deterministic, portable BGDX1 dictionary. The generated file does
// not contain pointers, native structures, size_t values, or wchar_t values.
class DictionaryBuilder final {
public:
    // `hint` is an optional pipe-separated list of local Bangla prediction
    // words. It is stored with the display word and never leaves the device.
    bool add(std::string_view phonetic,
             std::string_view bangla,
             std::uint32_t score,
             std::string_view hint = {});

    [[nodiscard]] std::string build() const;
    [[nodiscard]] std::size_t wordCount() const noexcept;
    [[nodiscard]] std::size_t formCount() const noexcept;

private:
    struct Word {
        std::string display;
        std::string hint;
    };

    struct Form {
        std::string key;
        std::uint32_t wordId = 0;
        std::uint32_t score = 0;
        SearchChannel channel = SearchChannel::Phonetic;
    };

    std::vector<Word> words_;
    std::vector<Form> forms_;
    std::map<std::string, std::uint32_t> wordIds_;
};

// Validates and searches an immutable BGDX1 byte image. The object owns a copy
// so callers may safely unmap or close the source file after load().
class Dictionary final {
public:
    bool load(std::string bytes);
    void clear() noexcept;

    [[nodiscard]] bool isLoaded() const noexcept { return loaded_; }
    [[nodiscard]] std::size_t wordCount() const noexcept { return wordCount_; }
    [[nodiscard]] std::size_t formCount() const noexcept { return formCount_; }
    [[nodiscard]] const std::string& lastError() const noexcept { return lastError_; }

    // Phonetic lookup is case-sensitive: capital-bearing keys (guR -> গুড়,
    // gUR -> গূঢ়) answer only to their exact spelling. Friendly-case input
    // (BANGLA) is the caller's folded-fallback query.
    [[nodiscard]] std::vector<Suggestion> lookup(SearchChannel channel,
                                                   std::string_view prefix,
                                                   std::size_t limit = 9) const;

    // Returns a small, deterministic set of phonetic words within a bounded
    // edit distance. This is a fallback for typos; prefix hits should be
    // queried with lookup() first.
    [[nodiscard]] std::vector<Suggestion> lookupFuzzyPhonetic(
        std::string_view value, std::size_t limit = 9) const;

    // True when the word (canonicalized) exists as a dictionary entry. Used
    // by the input engine to tell a correctly spelled direct conversion apart
    // from a typo that deserves fuzzy correction.
    [[nodiscard]] bool isKnownWord(std::string_view word) const;

    // The median phonetic-form score in this image: the dividing line between a
    // word the corpus considers common and one it considers obscure.
    //
    // This has to be derived from the loaded image rather than hardcoded. The
    // scoring units in different corpora are not comparable -- the Avro-derived
    // corpus packs 150k of its 171k forms into the ten values 987..997 and tops
    // out at 997, while the project starter dictionary runs 5000..11000 -- so
    // any absolute threshold is either dead (too high for a flat corpus, so
    // typo correction never reaches slot zero) or useless (too low for the
    // starter set, so every obscure neighbour gets promoted).
    [[nodiscard]] std::uint32_t scoreFloor() const noexcept { return scoreFloor_; }

    // Returns the optional prediction list stored on an exact Bangla word.
    // The list is a local, author-controlled hint such as "আমি|আমার".
    [[nodiscard]] std::vector<std::string> predictions(
        std::string_view word, std::size_t limit = 4) const;

private:
    struct Section {
        std::uint64_t offset = 0;
        std::uint64_t bytes = 0;
        std::uint64_t records = 0;
        std::uint16_t version = 0;
        std::uint16_t elementSize = 0;
        std::uint32_t id = 0;
    };

    struct FormRecord {
        std::uint32_t wordId = 0;
        std::uint32_t keyOffset = 0;
        std::uint32_t keyLength = 0;
        std::uint8_t channel = 0;
        std::uint32_t score = 0;
    };

    struct WordRecord {
        std::uint32_t displayOffset = 0;
        std::uint32_t displayLength = 0;
        std::uint32_t hintOffset = 0;
        std::uint32_t hintLength = 0;
    };

    [[nodiscard]] bool validate(std::string& error);
    [[nodiscard]] bool readForm(std::size_t index, FormRecord& form) const;
    [[nodiscard]] bool readWord(std::uint32_t wordId, WordRecord& word) const;
    [[nodiscard]] std::string_view blobString(std::uint32_t offset,
                                               std::uint32_t length) const noexcept;

    std::string bytes_;
    Section meta_;
    Section form_;
    Section word_;
    Section blob_;
    std::size_t wordCount_ = 0;
    std::size_t formCount_ = 0;
    std::uint64_t blobBytes_ = 0;
    std::uint32_t scoreFloor_ = 0;
    bool loaded_ = false;
    std::string lastError_;
};

} // namespace lekho::bn
