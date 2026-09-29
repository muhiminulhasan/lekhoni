#include "lekhoni/InputEngine.h"

#include "lekhoni/PhoneticTransliterator.h"
#include "lekhoni/Utf.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace lekho::bn {

bool InputEngine::loadDictionary(std::string bytes) {
    return dictionary_.load(std::move(bytes));
}

bool InputEngine::loadPhrases(std::string bytes) {
    if (bytes.empty()) {
        phrases_.clear();
        return true;
    }
    return phrases_.load(std::move(bytes));
}

void InputEngine::reset() noexcept {
    rawInput_.clear();
    directOutput_.clear();
    candidates_.clear();
    // The committed-word context deliberately survives reset(): the chain is
    // what carries next-word prediction across the commit that triggered it.
    // clearContext() is the explicit privacy reset.
}

void InputEngine::clearContext() noexcept {
    contextWord_.clear();
    contextPrevWord_.clear();
}

void InputEngine::setContextWord(std::string_view word) {
    contextPrevWord_ = contextWord_;
    contextWord_ = canonicalizeBangla(word);
}

void InputEngine::loadTrigrams(std::string bytes) {
    trigrams_.load(std::move(bytes));
}

bool InputEngine::appendCodePoint(char32_t value) {
    if (value == 0 || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return false;
    }

    const std::size_t oldSize = rawInput_.size();
    try {
        appendUtf8(rawInput_, value);
        refreshCandidates();
        return rawInput_.size() > oldSize;
    } catch (...) {
        rawInput_.resize(oldSize);
        refreshCandidates();
        return false;
    }
}

bool InputEngine::backspace() noexcept {
    if (rawInput_.empty()) {
        return false;
    }

    std::size_t start = rawInput_.size() - 1;
    while (start > 0 &&
           (static_cast<unsigned char>(rawInput_[start]) & 0xC0U) == 0x80U) {
        --start;
    }
    rawInput_.resize(start);
    try {
        refreshCandidates();
    } catch (...) {
        reset();
    }
    return true;
}

void InputEngine::refreshCandidates() {
    candidates_.clear();
    directOutput_.clear();
    if (rawInput_.empty()) {
        return;
    }

    PhoneticTransliterator converter;
    directOutput_ = canonicalizeBangla(converter.transliterate(rawInput_));
    // The as-typed layout conversion always stays selectable: dictionary and
    // phrase hits may outrank it, but they must not erase what the layout
    // itself produced for this keystroke sequence.
    const std::string layoutOutput = directOutput_;
    // Whole-word predictions (trigram statistics and static hints) only
    // surface while they still match what the user is typing; injected
    // mid-word they would be stale noise crowding out real completions.
    const auto predictionFits = [&](std::string_view word) {
        return !layoutOutput.empty() && word.size() >= layoutOutput.size() &&
               word.compare(0, layoutOutput.size(), layoutOutput) == 0;
    };

    // An uppercase letter is a disambiguation signal (valObasI must stay
    // ভালোবাসী). It bypasses the phrase layer, whose keys are matched
    // case-insensitively, and it also demotes the typo-correction fallback:
    // with a large corpus a one- or two-edit neighbour exists for almost any
    // spelling, and the user who types capitals has asked for an exact
    // spelling, not for the nearest word.
    const bool explicitCase =
        std::any_of(rawInput_.begin(), rawInput_.end(), [](const char value) {
            return value >= 'A' && value <= 'Z';
        });

    std::unordered_set<std::string> seen;
    const auto addCandidate = [this, &seen](const bn::Suggestion& match, bool direct) {
        if (match.word.empty() || !seen.insert(match.word).second ||
            candidates_.size() >= 9) {
            return;
        }
        candidates_.push_back({match.word, rawInput_, match.wordId, match.score, direct});
    };
    const auto addTextCandidate = [this, &seen](std::string text, bool direct) {
        text = canonicalizeBangla(text);
        if (text.empty() || !seen.insert(text).second || candidates_.size() >= 9) {
            return;
        }
        candidates_.push_back({std::move(text), rawInput_, 0, 0, direct});
    };

    std::vector<Suggestion> matches;
    std::vector<Suggestion> fuzzy;
    std::vector<Suggestion> phraseMatches;
    if (isPhoneticPrefix()) {
        matches = dictionary_.lookup(SearchChannel::Phonetic, rawInput_, 9);
        if (matches.empty() &&
            std::any_of(rawInput_.begin(), rawInput_.end(), [](char c) {
                return c >= 'A' && c <= 'Z';
            })) {
            // Friendly-case fallback: BANGLA still finds bangla when the
            // exact-case spelling has no dictionary entry.
            std::string folded(rawInput_);
            std::transform(folded.begin(), folded.end(), folded.begin(), [](char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
            });
            matches = dictionary_.lookup(SearchChannel::Phonetic, folded, 9);
        }
        if (phrases_.isLoaded()) {
            // Capitals mark a deliberate spelling: phrase keys that
            // contain capitals answer only to exact-case input.
            phraseMatches = phrases_.lookup(SearchChannel::Phonetic, rawInput_, 9);
        }
        fuzzy = dictionary_.lookupFuzzyPhonetic(rawInput_, 9);
    }

    // A curated phrase is the strongest signal there is: the author wrote the
    // key down specifically, so it outranks the general corpus and the
    // transliteration. This is what lets the phrase layer fix a spelling the
    // core dictionary gets wrong without regenerating the core dictionary.
    const auto phraseExact =
        std::find_if(phraseMatches.begin(), phraseMatches.end(), [](const auto& match) {
            return match.exact;
        });
    const auto exact = std::find_if(matches.begin(), matches.end(), [](const auto& match) {
        return match.exact;
    });
    if (phraseExact != phraseMatches.end()) {
        directOutput_ = canonicalizeBangla(phraseExact->word);
    } else if (exact != matches.end()) {
        // A reviewed dictionary spelling is the canonical direct result. This
        // makes corrections such as dhaka -> ঢাকা visible immediately while
        // typing, instead of showing ধাকা until the user opens the list.
        directOutput_ = canonicalizeBangla(exact->word);
    }
    // If the typed text has no prefix hit, a close dictionary match is more
    // useful than committing a raw phonetic conversion such as `bangaldesh`
    // -> `বাংলদেশ`. This must not fire when an exact phrase or core match
    // already exists, or the fuzzy guess would take candidate slot zero and
    // win the commit over the exact spelling.
    const bool hasExactPhrase = phraseExact != phraseMatches.end();
    const bool hasExactCore = exact != matches.end();
    // A direct conversion that is itself a dictionary word is what the user
    // asked for (rokte -> রক্ত); the fuzzy fallback must then stay out of
    // slot zero, because a two-edit neighbour (রুট) is noise by comparison.
    const bool directIsKnownWord =
        !directOutput_.empty() && dictionary_.isKnownWord(directOutput_);
    // One edit is a plausible typo (bangaldesh -> bangladesh); two edits are
    // speculation that must not outrank the direct conversion. And the
    // correction must be a well-known word: an obscure neighbour (rokte ->
    // রোখে) is noise next to the direct conversion (রক্তে).
    //
    // "Well-known" is the image's own median phonetic score, not a constant.
    // A fixed cutoff of 5000 worked only for the project starter dictionary:
    // the shipped Avro-derived corpus scores 166623 of its 171835 rows below
    // 5000 and packs 150k of them into 987..997, so an absolute floor left the
    // documented bangaldesh -> বাংলাদেশ correction unreachable for almost the
    // entire dictionary. The median is also the robust choice: the curated band
    // sits far above the corpus plateau, so a mean would exclude most words.
    const bool plausibleTypo = !fuzzy.empty() &&
                               fuzzy.front().editDistance <= 1 &&
                               fuzzy.front().score >= dictionary_.scoreFloor();
    if (matches.empty() && !hasExactPhrase && !hasExactCore && !fuzzy.empty() &&
        !explicitCase && !directIsKnownWord && plausibleTypo) {
        addCandidate(fuzzy.front(), false);
    }
    if (!directOutput_.empty()) {
        addTextCandidate(directOutput_, true);
    }
    if (!layoutOutput.empty() && layoutOutput != directOutput_) {
        addTextCandidate(layoutOutput, false);
    }
    for (const auto& match : phraseMatches) {
        addCandidate(match, false);
    }

    // Context predictions get reserved slots. Simply appending them after the
    // dictionary tail never surfaced anything: with the full corpus any
    // realistic prefix already fills all nine slots at the first keystroke.
    // The dictionary instead yields up to two of its weakest ranks so the
    // statistically likely next word is actually reachable. Direct output,
    // layout output, phrase hits, and the fuzzy-typo fallback keep their
    // places ahead of the reserved slots, and selection-memory promotion
    // still reorders everything afterwards.
    std::vector<std::string> contextPredictions;
    if (!contextWord_.empty()) {
        const auto collectPredictions = [&](const std::vector<std::string>& predictions) {
            for (const std::string& prediction : predictions) {
                if (contextPredictions.size() >= 2) {
                    break;
                }
                if (predictionFits(prediction) &&
                    std::find(contextPredictions.begin(), contextPredictions.end(),
                              prediction) == contextPredictions.end()) {
                    contextPredictions.push_back(prediction);
                }
            }
        };
        // Trigram statistics know what actually follows this word; the static
        // hint lists only know what the author once attached to it. The core
        // dictionary image carries no prediction lists at all (see the
        // build notes in build.ps1), so in the shipped build this comes from
        // the trigram table and the phrase layer.
        if (trigrams_.isLoaded()) {
            collectPredictions(trigrams_.next(contextPrevWord_, contextWord_, 3));
        }
        collectPredictions(dictionary_.predictions(contextWord_, 3));
        if (phrases_.isLoaded()) {
            collectPredictions(phrases_.predictions(contextWord_, 3));
        }
    }
    const std::size_t dictionaryBudget = 9 - contextPredictions.size();
    for (const auto& match : matches) {
        if (candidates_.size() >= dictionaryBudget) {
            break;
        }
        addCandidate(match, false);
    }
    for (const auto& prediction : contextPredictions) {
        addTextCandidate(prediction, false);
    }
    for (const auto& match : fuzzy) {
        addCandidate(match, false);
    }

    // The user's own past picks for this key outrank every default ranking,
    // in recency-weighted order, provided each word is still among the
    // candidates. The key is folded, so the memory applies whatever case the
    // user typed; a deliberate capital is itself remembered through the same
    // path. Promoted words never leapfrog anything outside the list, so the
    // phrase-layer and dictionary priority chain stays intact.
    if (!candidates_.empty()) {
        const std::size_t promoteUpTo = candidates_.size();
        std::size_t promote = 0;
        bool promoted = false;
        for (const std::string& remembered : selections_.find(rawInput_, userDay_)) {
            if (promote >= promoteUpTo) {
                break;
            }
            if (candidates_[promote].utf8 == remembered) {
                ++promote;
                continue;
            }
            bool moved = false;
            for (std::size_t i = promote + 1; i < candidates_.size(); ++i) {
                if (candidates_[i].utf8 == remembered) {
                    InputCandidate picked = candidates_[i];
                    candidates_.erase(candidates_.begin() + static_cast<std::ptrdiff_t>(i));
                    candidates_.insert(candidates_.begin() + static_cast<std::ptrdiff_t>(promote),
                                       std::move(picked));
                    ++promote;
                    promoted = true;
                    moved = true;
                    break;
                }
            }
            if (!moved) {
                // Not offered for this key; skip without consuming a slot.
            }
        }
        if (promoted && !candidates_.empty()) {
            directOutput_ = candidates_.front().utf8;
        }
    }
}

void InputEngine::setUserDay(std::uint32_t day) noexcept {
    userDay_ = day;
}

void InputEngine::recordSelection(std::string_view key, std::string_view word,
                                  std::uint32_t dayStamp) {
    selections_.record(key, word, dayStamp);
}

void InputEngine::loadUserSelections(std::string bytes) {
    selections_.load(std::move(bytes));
}

std::string InputEngine::userSelectionsBytes() const {
    return selections_.save();
}

std::vector<InputCandidate> InputEngine::suggestBangla(std::string_view prefix,
                                                       std::size_t limit) const {
    std::vector<InputCandidate> result;
    if (prefix.empty() || !containsBangla(prefix)) {
        return result;
    }
    const auto matches = dictionary_.lookup(SearchChannel::Bangla, prefix, limit);
    result.reserve(matches.size());
    for (const auto& match : matches) {
        result.push_back({match.word, match.hint, match.wordId, match.score, false});
    }
    // Same read side as the phonetic path: the user's remembered picks for
    // this Bangla prefix float to the top in recency-weighted order.
    std::size_t promote = 0;
    for (const std::string& remembered : selections_.find(prefix, userDay_)) {
        if (promote >= result.size()) {
            break;
        }
        if (result[promote].utf8 == remembered) {
            ++promote;
            continue;
        }
        for (std::size_t i = promote + 1; i < result.size(); ++i) {
            if (result[i].utf8 == remembered) {
                InputCandidate picked = result[i];
                result.erase(result.begin() + static_cast<std::ptrdiff_t>(i));
                result.insert(result.begin() + static_cast<std::ptrdiff_t>(promote),
                              std::move(picked));
                ++promote;
                break;
            }
        }
    }
    return result;
}

bool InputEngine::isPhoneticPrefix() const noexcept {
    if (rawInput_.empty()) {
        return false;
    }
    for (const char value : rawInput_) {
        if (!isAsciiLetter(value) && value != '\'' && value != '-') {
            return false;
        }
    }
    return true;
}

} // namespace lekho::bn
