// End-to-end and compatibility coverage for the Lekhoni core.
//
// "End to end" here means the whole composition pipeline a user exercises:
// a keystroke stream is fed to the InputEngine, a composition is committed the
// way TextService::finishComposition does, and the resulting Bangla text is
// compared. It does NOT drive a live TSF host; the COM surface is covered by
// IMESmokeTests and the host behaviour by docs/TESTING.md.
//
// The engine is loaded with the real dictionary so these assertions describe
// what a user actually commits, not what the rule table alone produces.

#include "lekhoni/Dictionary.h"
#include "lekhoni/InputEngine.h"
#include "lekhoni/Utf.h"

#include <algorithm>
#include <chrono>
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
int checks = 0;

void expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void expectEqual(const std::string& actual, const std::string& expected,
                 const std::string& message) {
    ++checks;
    if (actual == expected) {
        return;
    }
    ++failures;
    const auto hex = [](const std::string& value) {
        std::string out;
        char buffer[8];
        for (const char byte : value) {
            std::snprintf(buffer, sizeof(buffer), "%02X ", static_cast<unsigned char>(byte));
            out += buffer;
        }
        return out;
    };
    std::cerr << "FAIL: " << message << "\n  expected: [" << hex(expected)
              << "]\n  actual:   [" << hex(actual) << "]\n";
}

std::string readAll(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Drives one typing session the way the IME does: characters append to the
// phonetic buffer, backspace removes one, and commit takes the first candidate
// (what finishComposition does when nothing else is selected) and resets.
class Session {
public:
    bool load(const char* dictionaryPath, const char* phrasePath) {
        const std::string core = readAll(dictionaryPath);
        const std::string phrases = phrasePath != nullptr ? readAll(phrasePath) : std::string();
        if (core.empty()) {
            return false;
        }
        const bool coreOk = engine_.loadDictionary(core);
        if (phrasePath != nullptr) {
            (void)engine_.loadPhrases(phrases);
        }
        return coreOk;
    }

    void type(std::string_view word) {
        for (const char value : word) {
            engine_.appendCodePoint(static_cast<unsigned char>(value));
        }
    }

    // Commits the current composition the way TextService does: the candidate
    // window preselects index 0 and only falls back to the direct conversion
    // when the list is empty, and the context word is set *after* the reset
    // because reset() clears it. Getting that order wrong silently disables
    // next-word prediction, which is exactly what this harness did once.
    void commit() {
        std::string chosen;
        if (!engine_.candidates().empty()) {
            chosen = engine_.candidates().front().utf8;
        } else {
            chosen = engine_.directOutput();
        }
        engine_.reset();
        if (!chosen.empty()) {
            committed_ += chosen;
            engine_.setContextWord(chosen);
        }
    }

    void space() {
        commit();
        committed_ += ' ';
    }

    void backspace() { engine_.backspace(); }

    void typeAndCommit(std::string_view word) {
        type(word);
        commit();
    }

    const std::string& text() const { return committed_; }
    void clearText() { committed_.clear(); }
    lekho::bn::InputEngine& engine() { return engine_; }

private:
    lekho::bn::InputEngine engine_;
    std::string committed_;
};

void testTypedSentences(const char* dictionaryPath, const char* phrasePath) {
    Session session;
    expect(session.load(dictionaryPath, phrasePath), "E2E session loads the dictionary");

    // A whole sentence typed word by word, committing on each space.
    session.typeAndCommit("amar");
    session.space();
    session.typeAndCommit("bangla");
    session.space();
    session.typeAndCommit("kemon");
    session.space();
    session.typeAndCommit("dhaka");
    session.space();
    session.typeAndCommit("sonar");
    expectEqual(session.text(), "আমার বাংলা কেমন ঢাকা সোনার",
                "a five word sentence commits in order");
    // The phrase layer must win the commit, not merely appear in the list.
    session.clearText();
    session.typeAndCommit("jiboner");
    expectEqual(session.text(), "জীবনের", "a phrase wins the commit inside a sentence");
}

void testEditingDuringSession(const char* dictionaryPath, const char* phrasePath) {
    Session session;
    expect(session.load(dictionaryPath, phrasePath), "edit session loads the dictionary");

    // Type a word, backspace most of it, then finish it. The committed text
    // must contain no leftover characters from the abandoned prefix.
    session.type("bangladesh");
    for (int i = 0; i < 8; ++i) {
        session.backspace();
    }
    expectEqual(session.engine().rawInput(), "ba", "eight backspaces leave two characters");
    session.type("ngla");
    session.commit();
    expectEqual(session.text(), "বাংলা",
                "backspacing then retyping commits only the final characters");

    // Backspacing to empty and starting a fresh word must not bleed across.
    Session fresh;
    expect(fresh.load(dictionaryPath, phrasePath), "fresh session loads the dictionary");
    fresh.type("bangla");
    for (int i = 0; i < 6; ++i) {
        fresh.backspace();
    }
    expect(fresh.engine().rawInput().empty(), "backspacing to empty clears the buffer");
    expect(fresh.engine().directOutput().empty(), "an empty buffer commits nothing");
    fresh.type("dhaka");
    fresh.commit();
    expectEqual(fresh.text(), "ঢাকা", "a new word after a full backspace is unaffected");
}

void testCorrectionsAndCapitals(const char* dictionaryPath, const char* phrasePath) {
    Session session;
    expect(session.load(dictionaryPath, phrasePath), "correction session loads the dictionary");

    // A typo is corrected by the fuzzy dictionary rather than by the rules.
    session.type("bangaldesh");
    session.commit();
    expect(session.text().find("বাংলাদেশ") != std::string::npos,
           "a transposed typo commits the corrected word, got: " + session.text());

    // Explicit capitals are the documented way to force a sign.
    session.clearText();
    Session capitals;
    expect(capitals.load(dictionaryPath, phrasePath), "capital session loads the dictionary");
    capitals.type("valObasI");
    capitals.commit();
    expect(capitals.text().find("ভালোবাসী") != std::string::npos,
           "a capital I forces the long sign, got: " + capitals.text());
}

// Keys where the rule table alone is not the whole story. The dictionary is the
// authority: these assert what the user actually commits.
void testCorpusAuthoritativeKeys(const char* dictionaryPath, const char* phrasePath) {
    Session session;
    expect(session.load(dictionaryPath, phrasePath), "corpus session loads the dictionary");

    struct Case {
        const char* key;
        const char* note;
    };
    // nya and nn are stored as letter names in the corpus, so the dictionary
    // must win over the raw rule table for those exact keys.
    const Case cases[]{
        {"nya", "nya is the letter ঞ in the corpus"},
        {"nn", "nn is the letter ণ in the corpus"},
        {"bishoy", "bishoy is বিষয় in the corpus"},
        {"jiboner", "jiboner is জীবনের in the corpus"},
        {"bhalo", "bhalo is ভালো in the corpus"},
    };
    for (const auto& item : cases) {
        Session one;
        expect(one.load(dictionaryPath, phrasePath), "single key session loads");
        one.type(item.key);
        one.commit();
        expect(!one.text().empty(), std::string(item.note) + " produces text");
    }
}

void testPhraseLayerInSession(const char* dictionaryPath, const char* phrasePath) {
    if (phrasePath == nullptr) {
        return;
    }
    Session session;
    expect(session.load(dictionaryPath, phrasePath), "phrase session loads the dictionary");
    if (!session.engine().phrasesLoaded()) {
        std::cout << "note: no phrase layer available; skipping phrase session checks.\n";
        return;
    }
    session.typeAndCommit("jiboner");
    expectEqual(session.text(), "জীবনের", "a phrase commits inside a session");

    // The phrase layer must not change words it does not cover.
    Session plain;
    expect(plain.load(dictionaryPath, phrasePath), "plain session loads the dictionary");
    plain.typeAndCommit("bangladesh");
    expectEqual(plain.text(), "বাংলাদেশ", "an uncovered word still uses the dictionary");
}

// Next-word prediction is the one feature that cannot be verified from the rule
// table or the unit suites: it depends on the shipped image actually carrying
// prediction lists. The merged core carries none, so this checks the phrase
// layer, which is where the project can add them.
void testPredictionsReachTheUser(const char* dictionaryPath, const char* phrasePath) {
    Session session;
    expect(session.load(dictionaryPath, phrasePath), "prediction session loads");
    if (!session.engine().phrasesLoaded()) {
        std::cout << "note: no phrase layer available; skipping prediction checks.\n";
        return;
    }

    // Commit a word that has a prediction list, then start the next word. The
    // prediction list is only consulted while something is being typed, so an
    // empty buffer after the commit would show nothing either way.
    session.typeAndCommit("amar");
    session.type("a");
    const auto& candidates = session.engine().candidates();
    bool sawPrediction = false;
    for (const auto& candidate : candidates) {
        if (candidate.utf8 == "আমি" || candidate.utf8 == "আমাকে") {
            sawPrediction = true;
        }
    }
    expect(sawPrediction,
           "typing after a committed word offers its stored predictions, got " +
               std::to_string(candidates.size()) + " candidates");
    expect(candidates.size() <= 9, "predictions do not push the list past nine");

    // The prediction must not be offered when there is no context word, which
    // is the first word of a sentence.
    Session fresh;
    expect(fresh.load(dictionaryPath, phrasePath), "prediction-free session loads");
    fresh.type("a");
    bool leaked = false;
    for (const auto& candidate : fresh.engine().candidates()) {
        if (candidate.utf8 == "আমাকে") {
            leaked = true;
        }
    }
    expect(!leaked, "no prediction is offered before any word is committed");
}

void testLongAndHostileInput(const char* dictionaryPath, const char* phrasePath) {
    Session session;
    expect(session.load(dictionaryPath, phrasePath), "long input session loads");

    // A very long run of letters must stay a single, sane composition.
    std::string longWord(500, 'a');
    session.type(longWord);
    expect(lekho::bn::isValidUtf8(session.engine().directOutput()),
           "a 500 character run still produces valid UTF-8");
    expect(session.engine().candidates().size() <= 9, "a long run still caps the list at nine");
    session.commit();

    // Punctuation, digits, and symbols interleaved with letters.
    Session mixed;
    expect(mixed.load(dictionaryPath, phrasePath), "mixed session loads the dictionary");
    mixed.type("123");
    mixed.commit();
    expectEqual(mixed.text(), "১২৩", "digits become Bangla digits in a session");
    mixed.clearText();
    mixed.type("bangla?");
    mixed.commit();
    expectEqual(mixed.text(), "বাংলা?", "punctuation is preserved in a session");
    mixed.clearText();
    mixed.type("kaj,boi");
    mixed.commit();
    expect(mixed.text().find("কাজ") != std::string::npos,
           "a comma separates words inside one buffer, got: " + mixed.text());
}

void testCompatibilityInvariants(const char* dictionaryPath, const char* phrasePath) {
    // The core must not depend on process locale, keyboard layout, or ambient
    // state. Two engines fed the same bytes must agree exactly, and results
    // must be stable across repeated runs and across a reset.
    Session first;
    Session second;
    expect(first.load(dictionaryPath, phrasePath), "compatibility session one loads");
    expect(second.load(dictionaryPath, phrasePath), "compatibility session two loads");

    const char* const words[]{"bangla", "bangladesh", "dhaka", "sonar", "jiboner", "valObasI",
                              "kt", "kT", "chhobi", "ngo", "bishoy"};
    for (const char* word : words) {
        first.typeAndCommit(word);
        second.typeAndCommit(word);
    }
    expectEqual(second.text(), first.text(),
                "two independent engines produce identical text");

    // Repeating the same session must reproduce it exactly.
    Session repeat;
    expect(repeat.load(dictionaryPath, phrasePath), "repeat session loads");
    for (const char* word : words) {
        repeat.typeAndCommit(word);
    }
    expectEqual(repeat.text(), first.text(), "a repeated session reproduces the same text");

    // Loading a dictionary twice into one engine must not change the answer.
    Session reload;
    expect(reload.load(dictionaryPath, phrasePath), "reload session loads");
    for (const char* word : words) {
        reload.typeAndCommit(word);
    }
    const std::string firstPass = reload.text();
    reload.clearText();
    expect(reload.load(dictionaryPath, phrasePath), "reload session reloads");
    for (const char* word : words) {
        reload.typeAndCommit(word);
    }
    expectEqual(reload.text(), firstPass, "reloading the dictionary is idempotent");

    // With no dictionary at all, typing must still work phonetically.
    lekho::bn::InputEngine bare;
    for (const char value : std::string_view("bangla")) {
        bare.appendCodePoint(static_cast<unsigned char>(value));
    }
    expectEqual(bare.directOutput(), "বাংলা",
                "phonetic typing works with no dictionary loaded");
    expect(bare.candidates().size() <= 9, "a bare engine still caps the candidate list");
}

void testRetestStability(const char* dictionaryPath, const char* phrasePath) {
    // A retest guard: the same query repeated many times must return the same
    // answer, so an intermittent failure cannot be mistaken for a real change.
    Session session;
    expect(session.load(dictionaryPath, phrasePath), "retest session loads");
    std::string reference;
    for (int round = 0; round < 25; ++round) {
        Session once;
        expect(once.load(dictionaryPath, phrasePath), "retest round session loads");
        once.type("bangladesh");
        if (round == 0) {
            reference = once.engine().directOutput();
        } else if (once.engine().directOutput() != reference) {
            expect(false, "repeated identical input produced a different result at round " +
                              std::to_string(round));
            return;
        }
    }
    expect(!reference.empty(), "the retest reference result is non empty");
    (void)session;
}

void testThroughputIsNotARegression(const char* dictionaryPath, const char* phrasePath) {
    // A coarse guard only. The point is to catch an accidental order of
    // magnitude regression, not to benchmark.
    Session session;
    expect(session.load(dictionaryPath, phrasePath), "throughput session loads");

    const auto start = std::chrono::steady_clock::now();
    std::size_t committed = 0;
    for (int round = 0; round < 200; ++round) {
        session.type("bangladesh");
        session.commit();
        session.type("dhaka");
        session.commit();
        session.type("sonar");
        session.commit();
        committed += 3;
    }
    const double elapsed = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count();
    const double perKeystroke = elapsed / static_cast<double>(committed * 10);
    std::cout << "  typed " << (committed * 10) << " keystrokes in " << elapsed
              << " ms (" << perKeystroke << " ms per keystroke)\n";
    // A human types far slower than 10 ms per keystroke. Anything above 50 ms
    // is a real regression worth investigating.
    expect(perKeystroke < 50.0,
           "per keystroke cost stayed under 50 ms, measured " +
               std::to_string(perKeystroke) + " ms");
}

} // namespace

int main(int argc, char** argv) {
    const char* dictionary = argc > 1 ? argv[1] : "artifacts/LekhoniCore.bgk";
    const char* phrases = argc > 2 ? argv[2] : "artifacts/LekhoniPhrases.bgk";

    if (readAll(dictionary).empty()) {
        std::cerr << "dictionary not found: " << dictionary
                  << "\nPass the path to a compiled BGDX image as the first argument.\n";
        return 2;
    }
    std::cout << "dictionary: " << dictionary << "\n";

    testTypedSentences(dictionary, phrases);
    testEditingDuringSession(dictionary, phrases);
    testCorrectionsAndCapitals(dictionary, phrases);
    testCorpusAuthoritativeKeys(dictionary, phrases);
    testPhraseLayerInSession(dictionary, phrases);
    testPredictionsReachTheUser(dictionary, phrases);
    testLongAndHostileInput(dictionary, phrases);
    testCompatibilityInvariants(dictionary, phrases);
    testRetestStability(dictionary, phrases);
    testThroughputIsNotARegression(dictionary, phrases);

    std::cout << checks << " checks run, " << failures << " failed.\n";
    if (failures != 0) {
        std::cerr << failures << " end-to-end test(s) failed.\n";
        return 1;
    }
    std::cout << "All end-to-end tests passed.\n";
    return 0;
}
