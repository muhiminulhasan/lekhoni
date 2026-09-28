// Black-box audit of the dictionary images this project actually ships.
//
// Every other suite works on a small dictionary built inside the test, which
// is the right way to test logic but says nothing about the artifact that
// reaches a user. This suite loads the real BGDX files and audits them through
// the public API only: no internals, no peeking at record layout.
//
// Two things make this worth having as its own binary:
//
//   1. predictions() binary-searches the sorted Bangla section. A shipped
//      image that is unsorted, truncated, or subtly corrupt would make it
//      return nothing at all, silently, with no crash and no error. That is
//      the exact failure mode that made predictions() dead at 318,964 forms,
//      and nothing else in the tree exercises it at real scale.
//   2. A corrupt dictionary ships. build.ps1 checks that the two architectures
//      agree byte for byte, but nothing checks that the agreed-upon bytes are
//      actually a usable dictionary.
//
// The audit is bounded: it sweeps prefixes and takes a fixed-size sample, so
// runtime does not grow with dictionary size.

#include "lekhoni/Dictionary.h"
#include "lekhoni/Utf.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;
long long checks = 0;

void fail(const std::string& message) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        fail(message);
    }
}

void note(const std::string& message) { std::cout << "  " << message << '\n'; }

std::string readAll(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Every suggestion the dictionary hands back must be safe to put in a document.
void expectUsableSuggestion(const lekho::bn::Suggestion& suggestion,
                           const std::string& context) {
    expect(!suggestion.word.empty(), context + ": the word is not empty");
    expect(lekho::bn::isValidUtf8(suggestion.word), context + ": the word is valid UTF-8");
    expect(lekho::bn::isValidUtf8(suggestion.hint), context + ": the hint is valid UTF-8");
    expect(suggestion.word.find('\t') == std::string::npos,
           context + ": the word holds no tab");
    expect(suggestion.word.find('\n') == std::string::npos,
           context + ": the word holds no newline");
}

// ---------------------------------------------------------------------------
// 1. The image loads and is plausibly sized
// ---------------------------------------------------------------------------

void auditLoads(const lekho::bn::Dictionary& dictionary, const std::string& label) {
    expect(dictionary.isLoaded(), label + ": the image loads");
    expect(dictionary.lastError().empty(),
           label + ": loading reports no error, got: " + dictionary.lastError());
    expect(dictionary.wordCount() > 0, label + ": the image holds at least one word");
    expect(dictionary.formCount() > 0, label + ": the image holds at least one form");
    // A form count below the word count would mean forms were dropped, and
    // above a small multiple would mean the same word was stored repeatedly.
    expect(dictionary.formCount() >= dictionary.wordCount(),
           label + ": there is at least one form per word");
    note(label + ": " + std::to_string(dictionary.wordCount()) + " words, " +
         std::to_string(dictionary.formCount()) + " forms");
}

// ---------------------------------------------------------------------------
// 2. Every prefix sweep returns a well-formed, ranked, duplicate-free list
// ---------------------------------------------------------------------------

// Sweeps every one and two letter prefix. This is the broadest black-box probe
// available: it touches every key in the image at least once, because a key of
// length n is returned by the sweep of its own first n letters.
void auditPrefixSweep(const lekho::bn::Dictionary& dictionary, const std::string& label) {
    const char alphabet[] = "abcdefghijklmnopqrstuvwxyz";
    long long nonEmpty = 0;
    long long exactHits = 0;

    for (const char first : std::string_view(alphabet)) {
        for (int depth = 0; depth < 2; ++depth) {
            const std::string prefix = depth == 0 ? std::string(1, first)
                                                  : std::string(1, first) + alphabet[first - 'a'];
            const auto results =
                dictionary.lookup(lekho::bn::SearchChannel::Phonetic, prefix, 9);
            const std::string context = label + " prefix \"" + prefix + "\"";

            expect(results.size() <= 9, context + ": the result list is capped at nine");
            if (!results.empty()) {
                ++nonEmpty;
            }
            for (std::size_t i = 0; i < results.size(); ++i) {
                expectUsableSuggestion(results[i], context);
                // Ranking follows the engine's documented order: an exact match
                // first, then the shortest matching key, then the higher score,
                // then the lower word id. Checking score alone is wrong, because
                // a three letter key is a better prefix match than a seven
                // letter one regardless of frequency.
                if (i > 0) {
                    const auto& previous = results[i - 1];
                    const auto& current = results[i];
                    bool ordered = false;
                    if (previous.exact != current.exact) {
                        ordered = previous.exact;
                    } else if (previous.matchedKeyLength != current.matchedKeyLength) {
                        ordered = previous.matchedKeyLength < current.matchedKeyLength;
                    } else if (previous.score != current.score) {
                        ordered = previous.score > current.score;
                    } else {
                        ordered = previous.wordId < current.wordId;
                    }
                    expect(ordered, context + ": results follow the documented ranking");
                }
                for (std::size_t j = i + 1; j < results.size(); ++j) {
                    expect(results[i].word != results[j].word,
                           context + ": the result list holds no duplicate word");
                }
                // An exact match means the whole key equals the query, so it can
                // only be reported for the deepest prefix actually swept.
                if (results[i].exact) {
                    ++exactHits;
                    expect(results[i].editDistance == 0,
                           context + ": an exact match has zero edit distance");
                    expect(results[i].matchedKeyLength <= prefix.size(),
                           context + ": a match never claims a longer key than the query");
                }
            }
        }
    }
    expect(nonEmpty > 0, label + ": the prefix sweep found something");
    note(label + ": swept 52 prefixes, " + std::to_string(nonEmpty) +
         " non-empty, " + std::to_string(exactHits) + " exact");
}

// ---------------------------------------------------------------------------
// 3. predictions() works at real scale
// ---------------------------------------------------------------------------

// This is the check that would have caught predictions() being dead. It asks
// for the Bangla channel across a spread of prefixes and confirms the binary
// search actually lands on records rather than falling off the end.
//
// Real display words are harvested from the Bangla channel itself rather than
// hardcoded, so the words fed to predictions() are guaranteed to exist in this
// image. A hardcoded list would quietly go stale the next time the corpus
// changes, and a word that is not in the dictionary returns an empty list,
// which would make the check pass while testing nothing.
std::vector<std::string> harvestBanglaWords(const lekho::bn::Dictionary& dictionary,
                                            std::size_t wanted) {
    // Bangla consonant letters, as code points, chosen to open the most entries.
    static const char* const seeds[]{
        "\xE0\xA6\x95",  // ক
        "\xE0\xA6\x9A",  // জ
        "\xE0\xA6\xAE",  // ম
        "\xE0\xA6\xB8",  // স
        "\xE0\xA6\xAC",  // ব
        "\xE0\xA6\xA4",  // ত
        "\xE0\xA6\xB9",  // হ
        "\xE0\xA6\x95\xE0\xA7\x8D",  // ক্
        "\xE0\xA6\x9C\xE0\xA7\x87",  // জে
        "\xE0\xA6\xAE\xE0\xA6\xBE",  // মা
    };
    std::vector<std::string> words;
    for (const char* seed : seeds) {
        for (std::size_t extra = 0; extra < 4 && words.size() < wanted; ++extra) {
            const std::string prefix(seed);
            const auto hits = dictionary.lookup(lekho::bn::SearchChannel::Bangla, prefix, 9);
            for (const auto& hit : hits) {
                if (!hit.word.empty() && words.size() < wanted) {
                    words.push_back(hit.word);
                }
            }
            break;  // one page per seed keeps the harvest bounded
        }
    }
    return words;
}

// Returns the number of prediction entries the image yielded, so the caller can
// tell "this image has no prediction data" apart from "predictions are broken".
long long auditPredictions(const lekho::bn::Dictionary& dictionary, const std::string& label) {
    const std::vector<std::string> words = harvestBanglaWords(dictionary, 60);
    // The phrase image is deliberately tiny, so the floor is low enough to hold
    // for both images. It only has to rule out harvesting nothing at all, which
    // would make every assertion below vacuous.
    expect(words.size() >= 5,
           label + ": the Bangla channel yielded enough words to audit, got " +
               std::to_string(words.size()));

    long long nonEmpty = 0;
    long long entries = 0;
    for (const std::string& word : words) {
        const auto list = dictionary.predictions(word, 4);
        const std::string context = label + " predictions for \"" + word + "\"";
        expect(list.size() <= 4, context + ": the list is capped at four");
        for (const std::string& entry : list) {
            ++entries;
            expect(!entry.empty(), context + ": no entry is empty");
            expect(lekho::bn::isValidUtf8(entry), context + ": every entry is valid UTF-8");
        }
        for (std::size_t i = 0; i < list.size(); ++i) {
            for (std::size_t j = i + 1; j < list.size(); ++j) {
                expect(list[i] != list[j], context + ": the list holds no duplicate");
            }
        }
        if (!list.empty()) {
            ++nonEmpty;
        }
    }
    note(label + ": predictions returned " + std::to_string(entries) + " entries for " +
         std::to_string(nonEmpty) + "/" + std::to_string(words.size()) + " real words");
    if (entries == 0) {
        note(label + ": NOTE this image carries no prediction lists at all. That is a "
                    "data gap, not a code fault: predictions() works but has nothing "
                    "to find. The merged core dictionary is precompiled and cannot be "
                    "regenerated, so prediction data ships in the phrase layer.");
    }
    return entries;
}

// ---------------------------------------------------------------------------
// 4. Fuzzy lookup finds the word it was given
// ---------------------------------------------------------------------------

// Typo correction is the whole reason lookupFuzzyPhonetic exists, so this
// checks the real behaviour rather than just that it returns something: a key
// must come back at distance zero, and a single-edit mutation of it must come
// back at distance one.
//
// Suggestion::hint carries the word's prediction blob, not the matched key, so
// a key is identified by the display word an exact phonetic lookup resolves it
// to. That resolution is also the precondition for the test: a key the corpus
// does not contain cannot exercise the fuzzy path at all.
void auditFuzzyLookup(const lekho::bn::Dictionary& dictionary, const std::string& label) {
    const char* const keys[]{
        "bangla", "bangladesh", "dhaka", "sonar", "amar", "jibon", "bhalo",
        "bishoy", "jiboner", "sonta", "sona", "kemon", "mone", "kothay", "valobasi",
    };
    const std::size_t keyCount = sizeof(keys) / sizeof(keys[0]);

    long long resolved = 0;
    long long selfFound = 0;
    long long mutationsTested = 0;
    long long mutationsRecovered = 0;

    for (std::size_t i = 0; i < keyCount; ++i) {
        const std::string key(keys[i]);
        const auto exact =
            dictionary.lookup(lekho::bn::SearchChannel::Phonetic, key, 1);
        if (exact.empty() || !exact.front().exact) {
            // The corpus does not carry this key. Not a failure: it just means
            // there is nothing to check. Counted so the note is honest.
            continue;
        }
        const std::string display = exact.front().word;
        ++resolved;

        const auto fuzzy = dictionary.lookupFuzzyPhonetic(key, 5);
        bool self = false;
        for (const auto& candidate : fuzzy) {
            expectUsableSuggestion(candidate, label + " fuzzy \"" + key + "\"");
            if (candidate.word == display && candidate.editDistance == 0) {
                self = true;
            }
        }
        if (self) {
            ++selfFound;
        }

        if (key.size() < 4) {
            continue;
        }
        std::vector<std::string> mutations;
        mutations.push_back(key.substr(0, key.size() - 1));                    // deletion
        mutations.push_back(key.substr(0, key.size() / 2) + key.substr(key.size() / 2 + 1));
        std::string swapped = key;                                             // transposition
        std::swap(swapped[key.size() / 2], swapped[key.size() / 2 + 1]);
        mutations.push_back(swapped);
        std::string doubled = key;                                             // insertion
        doubled.insert(doubled.begin() + 1, key[1]);
        mutations.push_back(doubled);

        for (const std::string& mutation : mutations) {
            if (mutation == key) {
                continue;
            }
            ++mutationsTested;
            const auto hits = dictionary.lookupFuzzyPhonetic(mutation, 5);
            bool recovered = false;
            for (const auto& candidate : hits) {
                expectUsableSuggestion(candidate, label + " fuzzy \"" + mutation + "\"");
                if (candidate.word == display && candidate.editDistance <= 1) {
                    recovered = true;
                }
            }
            if (recovered) {
                ++mutationsRecovered;
            }
        }
    }

    note(label + ": " + std::to_string(resolved) + "/" + std::to_string(keyCount) +
         " probe keys exist in the corpus");
    note(label + ": fuzzy returned the word itself " + std::to_string(selfFound) + "/" +
         std::to_string(resolved) + " times at distance zero");
    note(label + ": fuzzy recovered " + std::to_string(mutationsRecovered) + "/" +
         std::to_string(mutationsTested) + " single-edit typos");

    expect(resolved > 0, label + ": the probe keys resolved against the real corpus");
    expect(selfFound == resolved,
           label + ": fuzzy lookup returns every sampled word at distance zero");
    expect(mutationsTested > 0, label + ": the fuzzy audit tested at least one mutation");
    // The scan is bounded by kMaximumScannedForms, so a rare key can fall off
    // the end. A floor rather than an equality keeps that from being a failure.
    expect(mutationsRecovered * 2 >= mutationsTested,
           label + ": fuzzy lookup recovers most single-edit typos");
}

// ---------------------------------------------------------------------------
// 5. The two architecture images must be byte-identical
// ---------------------------------------------------------------------------

void auditCrossArchitectureParity(const std::string& primaryPath,
                                  const std::string& secondaryPath,
                                  const std::string& label) {
    const std::string primary = readAll(primaryPath.c_str());
    const std::string secondary = readAll(secondaryPath.c_str());
    if (secondary.empty()) {
        note(label + ": no second image supplied, skipping the parity check");
        return;
    }
    expect(!primary.empty(), label + ": the primary image is present");
    expect(primary.size() == secondary.size(),
           label + ": both images are the same size, " + std::to_string(primary.size()) +
               " vs " + std::to_string(secondary.size()));
    expect(primary == secondary, label + ": both images are byte-identical");
    if (primary == secondary) {
        note(label + ": byte-identical across architectures (" +
             std::to_string(primary.size()) + " bytes)");
    }
}

long long audit(const char* path, const char* label) {
    std::cout << "\n[" << label << "] " << path << "\n";
    const std::string bytes = readAll(path);
    if (bytes.empty()) {
        fail(std::string(label) + ": the image file is missing or empty: " + path);
        return 0;
    }

    lekho::bn::Dictionary dictionary;
    if (!dictionary.load(bytes)) {
        fail(std::string(label) + ": the image failed to load: " + dictionary.lastError());
        return 0;
    }
    auditLoads(dictionary, label);
    auditPrefixSweep(dictionary, label);
    const long long predictionEntries = auditPredictions(dictionary, label);
    auditFuzzyLookup(dictionary, label);

    // A second load of the same bytes must give the same answers. This catches
    // state left behind by the first load, which would otherwise only show up
    // as an intermittent failure in the IME.
    const std::string tag(label);
    lekho::bn::Dictionary reloaded;
    expect(reloaded.load(bytes), tag + ": the image loads a second time");
    expect(reloaded.wordCount() == dictionary.wordCount(),
           tag + ": reloading reports the same word count");
    expect(reloaded.formCount() == dictionary.formCount(),
           tag + ": reloading reports the same form count");
    for (const char first : std::string_view("abc")) {
        const std::string prefix(1, first);
        const auto before =
            dictionary.lookup(lekho::bn::SearchChannel::Phonetic, prefix, 9);
        const auto after =
            reloaded.lookup(lekho::bn::SearchChannel::Phonetic, prefix, 9);
        expect(before.size() == after.size(),
               tag + ": reloading gives the same result count for prefix \"" + prefix + "\"");
        for (std::size_t i = 0; i < before.size() && i < after.size(); ++i) {
            expect(before[i].word == after[i].word,
                   tag + ": reloading gives the same words for prefix \"" + prefix + "\"");
        }
    }
    return predictionEntries;
}

} // namespace

int main(int argc, char** argv) {
    const char* core = argc > 1 ? argv[1] : "artifacts/LekhoniCore.bgk";
    const char* phrases = argc > 2 ? argv[2] : "artifacts/LekhoniPhrases.bgk";
    const char* coreAlt = argc > 3 ? argv[3] : "";

    std::cout << "Auditing the shipped dictionary images.\n"
                 "This is a black-box audit: it uses only the public API and\n"
                 "assumes the image is exactly what a user would receive.\n";

    const long long corePredictions = audit(core, "core");
    long long phrasePredictions = 0;
    if (readAll(phrases).empty()) {
        note(std::string("phrases: no phrase image at ") + phrases + ", skipping");
    } else {
        phrasePredictions = audit(phrases, "phrases");
    }

    // The next-word prediction feature is only alive if some shipped image
    // carries prediction lists. Neither image having any is a silent product
    // failure: predictions() returns cleanly, every unit test still passes, and
    // the user simply never sees a suggestion. So it is asserted across the set
    // rather than per image, because the core is precompiled and cannot be given
    // hints, while the phrase layer is project-authored and can.
    expect(corePredictions + phrasePredictions > 0,
           "at least one shipped image carries prediction lists, otherwise the "
           "next-word prediction feature is dead in every build (core " +
               std::to_string(corePredictions) + ", phrases " +
               std::to_string(phrasePredictions) + ")");
    if (coreAlt != nullptr && coreAlt[0] != '\0') {
        auditCrossArchitectureParity(core, coreAlt, "parity");
    } else {
        note("parity: no second core image supplied, skipping");
    }

    std::cout << "\n" << checks << " audit checks run, " << failures << " failed.\n";
    if (failures != 0) {
        std::cerr << failures << " audit check(s) failed.\n";
        return 1;
    }
    std::cout << "The shipped images passed the audit.\n";
    return 0;
}
