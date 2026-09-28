#pragma once

#include "lekhoni/Dictionary.h"
#include "lekhoni/SelectionMemory.h"
#include "lekhoni/TrigramModel.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lekho::bn {

struct InputCandidate {
    std::string utf8;
    std::string phonetic;
    std::uint32_t wordId = 0;
    std::uint32_t score = 0;
    bool direct = false;
};

// Holds only the current composition. It is independent from TSF and can be
// reused by desktop, mobile, or test frontends.
class InputEngine final {
public:
    bool loadDictionary(std::string bytes);
    // Loads an optional curated phrase layer consulted before the main
    // dictionary. Phrases are project-authored and MIT-licensed, unlike the
    // core dictionary, which carries its upstream (GPL-derived) license.
    bool loadPhrases(std::string bytes);
    void reset() noexcept;
    // Sets the last committed word used for context-aware next-word
    // prediction. Consecutive calls build a two-word chain; an empty value
    // clears the chain.
    void setContextWord(std::string_view word);
    void clearContext() noexcept;
    void loadTrigrams(std::string bytes);
    [[nodiscard]] bool trigramsLoaded() const noexcept { return trigrams_.isLoaded(); }

    bool appendCodePoint(char32_t value);
    bool backspace() noexcept;

    void refreshCandidates();
    [[nodiscard]] std::vector<InputCandidate> suggestBangla(std::string_view prefix,
                                                            std::size_t limit = 9) const;

    [[nodiscard]] const std::string& rawInput() const noexcept { return rawInput_; }
    [[nodiscard]] const std::string& directOutput() const noexcept { return directOutput_; }
    [[nodiscard]] const std::string& contextWord() const noexcept { return contextWord_; }
    [[nodiscard]] const std::vector<InputCandidate>& candidates() const noexcept { return candidates_; }
    [[nodiscard]] bool dictionaryLoaded() const noexcept { return dictionary_.isLoaded(); }
    [[nodiscard]] bool phrasesLoaded() const noexcept { return phrases_.isLoaded(); }
    // Image statistics, exposed so an accuracy report can name the corpus it
    // measured rather than leaving the numbers unattributed.
    [[nodiscard]] std::size_t dictionaryWordCount() const noexcept {
        return dictionary_.wordCount();
    }
    [[nodiscard]] std::size_t dictionaryFormCount() const noexcept {
        return dictionary_.formCount();
    }
    [[nodiscard]] std::uint32_t dictionaryScoreFloor() const noexcept {
        return dictionary_.scoreFloor();
    }

    // User selection memory: remembers which candidates the user picked for a
    // typed key so those words rank up first next time. dayStamp is a coarse
    // day counter supplied by the platform layer; setUserDay feeds the same
    // clock to the read side so recency decay works while typing.
    void setUserDay(std::uint32_t day) noexcept;
    void recordSelection(std::string_view key, std::string_view word, std::uint32_t dayStamp);
    void loadUserSelections(std::string bytes);
    [[nodiscard]] std::string userSelectionsBytes() const;
    [[nodiscard]] std::size_t userSelectionCount() const noexcept { return selections_.size(); }

private:
    [[nodiscard]] bool isPhoneticPrefix() const noexcept;

    Dictionary dictionary_;
    Dictionary phrases_;
    SelectionMemory selections_;
    TrigramModel trigrams_;
    std::string rawInput_;
    std::string directOutput_;
    std::string contextWord_;
    std::string contextPrevWord_;
    std::vector<InputCandidate> candidates_;
    std::uint32_t userDay_ = 0;
};

} // namespace lekho::bn
