#include "lekhoni/Dictionary.h"
#include "lekhoni/InputEngine.h"
#include "lekhoni/PhoneticTransliterator.h"
#include "lekhoni/Utf.h"
#include "lekhoni/WordPrefix.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expectEqual(std::string_view actual, std::string_view expected, std::string_view testName) {
    if (actual == expected) {
        return;
    }
    ++failures;
    const auto hex = [](std::string_view value) {
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const char byte : value) {
            output << std::setw(2) << static_cast<unsigned int>(static_cast<unsigned char>(byte)) << ' ';
        }
        return output.str();
    };
    std::cerr << "FAIL: " << testName
              << "\n  expected: " << expected << " [" << hex(expected) << "]"
              << "\n  actual:   " << actual << " [" << hex(actual) << "]\n";
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void testUtf8() {
    expect(lekho::bn::isValidUtf8("plain ascii"), "ASCII must be valid UTF-8");
    expect(lekho::bn::isValidUtf8("বাংলা"), "Bangla UTF-8 must be valid");
    expect(lekho::bn::isValidUtf8("emoji 😀"), "Four-byte UTF-8 must be valid");
    expect(!lekho::bn::isValidUtf8("\xC0\xAF"), "overlong encoding must be rejected");
    expect(!lekho::bn::isValidUtf8("\xED\xA0\x80"), "surrogate must be rejected");
    expect(lekho::bn::containsBangla("hello বাংলা"), "Bangla code point detection");
    expect(!lekho::bn::containsBangla("hello"), "ASCII is not Bangla");
}

void testWordPrefix() {
    const auto phonetic = lekho::bn::findWordPrefix("আমি ভালো আছি ban");
    expect(phonetic.kind == lekho::bn::WordKind::Phonetic, "Latin token kind");
    expectEqual(phonetic.utf8, "ban", "Latin token extraction");

    const auto bangla = lekho::bn::findWordPrefix("আমি ভালো আছি");
    expect(bangla.kind == lekho::bn::WordKind::Bangla, "Bangla token kind");
    expectEqual(bangla.utf8, "আছি", "Bangla token extraction");

    const auto bounded = lekho::bn::findWordPrefix(std::string(300, 'a'));
    expectEqual(bounded.utf8, std::string(256, 'a'), "prefix scan is bounded");
}

// Builds a unique lowercase-letter-only phonetic key for a small integer, so a
// large synthetic dictionary can be generated without tripping key validation.
std::string letterKey(int value) {
    std::string key;
    do {
        key.insert(key.begin(), static_cast<char>('a' + (value % 26)));
        value /= 26;
    } while (value > 0);
    return "z" + key;
}

void testDictionary() {
    lekho::bn::DictionaryBuilder builder;
    expect(builder.add("bangla", "বাংলা", 10000), "add bangla");
    expect(builder.add("bangladesh", "বাংলাদেশ", 10000), "add bangladesh");
    expect(builder.add("test", "পরীক্ষা", 10), "add test");
    expect(builder.add("testing", "পরীক্ষা", 100), "add testing alias");
    expect(!builder.add("bad key", "শব্দ", 1), "reject invalid phonetic key");
    expect(!builder.add("valid", "latin", 1), "reject non-Bangla display");

    const std::string image = builder.build();
    expect(!image.empty(), "dictionary image is generated");

    lekho::bn::Dictionary dictionary;
    expect(dictionary.load(image), "dictionary image validates: " + dictionary.lastError());
    expect(dictionary.wordCount() == 3, "dictionary word count");
    expect(dictionary.formCount() == 7, "dictionary form count");

    // Phonetic lookup is case-sensitive at the dictionary level (guR vs
    // gur); friendly-case input is handled by the engine's folded fallback.
    expect(dictionary.lookup(lekho::bn::SearchChannel::Phonetic, "BANGLA", 9).empty(),
           "phonetic lookup respects case");
    const auto phonetic = dictionary.lookup(lekho::bn::SearchChannel::Phonetic, "bangla", 9);
    expect(phonetic.size() == 2, "case-insensitive phonetic prefix lookup");
    if (phonetic.size() == 2) {
        expect(phonetic[0].word == "বাংলা", "shorter exact key ranks deterministically");
        expect(phonetic[1].word == "বাংলাদেশ", "longer prefix match");
    }

    const auto aliases = dictionary.lookup(lekho::bn::SearchChannel::Phonetic, "te", 9);
    expect(aliases.size() == 1 && aliases[0].score == 10 && aliases[0].matchedKeyLength == 4,
           "shorter form outranks a longer high-frequency alias");

    const auto bangla = dictionary.lookup(lekho::bn::SearchChannel::Bangla, "বাংলা", 1);
    expect(bangla.size() == 1, "Bangla prefix result limit");

    lekho::bn::DictionaryBuilder predictionBuilder;
    expect(predictionBuilder.add("ami", "আমি", 100, "আমরা|আমার"),
           "add prediction hints");
    lekho::bn::Dictionary predictionDictionary;
    expect(predictionDictionary.load(predictionBuilder.build()),
           "prediction dictionary validates");
    const auto predictions = predictionDictionary.predictions("আমি", 4);
    expect(predictions.size() == 2 && predictions[0] == "আমরা" && predictions[1] == "আমার",
           "prediction hints are returned in source order");

    // Regression: a tiny dictionary hides a scan that starts at form 0 and
    // gives up after a fixed budget, because the Phonetic channel is sorted
    // first. Pad the phonetic channel past that budget and the bug reappears.
    {
        lekho::bn::DictionaryBuilder padded;
        // Filler entries only need to be valid Bangla: the point is to grow the
        // phonetic channel past the old 65536-form scan budget. Every filler
        // key is unique, so each one contributes a form.
        for (int index = 0; index < 70000; ++index) {
            std::string word;
            for (int repeat = 0; repeat <= index % 7; ++repeat) {
                word += "\xE0\xA6\x95\xE0\xA6\xBE"; // কা
            }
            padded.add(letterKey(index), word, 1000);
        }
        expect(padded.formCount() > 65536,
               "filler dictionary exceeds the old scan budget");
        padded.add("ami", "আমি", 100, "আমরা|আমার");
        padded.add("bangla", "বাংলা", 100, "বাংলাদেশ|বাঙলা");

        lekho::bn::Dictionary large;
        expect(large.load(padded.build()), "large dictionary validates");
        const auto largePredictions = large.predictions("আমি", 4);
        expect(largePredictions.size() == 2 && largePredictions[0] == "আমরা" &&
                   largePredictions[1] == "আমার",
               "prediction hints survive a large phonetic channel");
        expect(!large.predictions("বাংলা", 4).empty(),
               "prediction hints work for a later Bangla form");
    }

    auto corrupt = image;
    corrupt[0] = 'X';
    lekho::bn::Dictionary invalidDictionary;
    expect(!invalidDictionary.load(corrupt), "bad magic is rejected");

    auto truncated = image;
    truncated.resize(64);
    expect(!invalidDictionary.load(truncated), "truncated dictionary is rejected");

    bool corruptionRejected = true;
    for (std::size_t offset = 17; offset < image.size(); offset += 19) {
        auto damaged = image;
        damaged[offset] = static_cast<char>(damaged[offset] ^ 0x5A);
        if (invalidDictionary.load(damaged)) {
            corruptionRejected = false;
            break;
        }
    }
    expect(corruptionRejected, "single-byte dictionary corruption is rejected");
}

void testInputEngine() {
    lekho::bn::DictionaryBuilder builder;
    builder.add("bangla", "বাংলা", 10000);
    builder.add("bangladesh", "বাংলাদেশ", 10000);
    builder.add("dhaka", "ঢাকা", 10000);
    builder.add("ghor", "ঘর", 9000);
    builder.add("amar", "আমার", 9000);
    builder.add("dhaporkathi", "ঢাপরকাঠী", 3000);

    lekho::bn::InputEngine engine;
    expect(engine.loadDictionary(builder.build()), "input engine dictionary load");
    for (const char value : std::string_view("bangla")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)), "append phonetic key");
    }
    expectEqual(engine.rawInput(), "bangla", "engine retains phonetic source");
    expectEqual(engine.directOutput(), "বাংলা", "engine direct conversion");
    expect(engine.candidates().size() == 2, "engine merges direct and dictionary candidates");
    if (engine.candidates().size() >= 2) {
        expectEqual(engine.candidates()[1].utf8, "বাংলাদেশ", "dictionary candidate follows direct");
    }

    expect(engine.backspace(), "engine backspace");
    expectEqual(engine.rawInput(), "bangl", "backspace removes one code point");
    expect(!engine.appendCodePoint(0xD800), "reject UTF-16 surrogate input");

    const auto bangla = engine.suggestBangla("বাংলা", 5);
    expect(bangla.size() == 2, "engine Bangla-prefix suggestions");

    engine.reset();
    for (const char value : std::string_view("dhaka")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)), "append correction key");
    }
    // The dictionary exact hit leads, and the as-typed layout conversion
    // stays selectable right behind it instead of being erased.
    expect(!engine.candidates().empty() && engine.candidates()[0].utf8 == "ঢাকা",
           "exact dictionary spelling wins");
    expect(engine.candidates().size() == 2 && engine.candidates()[1].utf8 == "ধাকা",
           "the layout conversion remains selectable");

    engine.reset();
    for (const char value : std::string_view("bangaldesh")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)), "append typo key");
    }
    const auto typoCandidates = engine.candidates();
    expect(!typoCandidates.empty() && typoCandidates.front().utf8 == "বাংলাদেশ",
           "one-edit phonetic typo correction");

    engine.reset();
    for (const char value : std::string_view("ghor")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)),
               "append inherent-o key");
    }
    expectEqual(engine.directOutput(), "ঘর",
                "dictionary canonical spelling is displayed while typing");

    engine.reset();
    for (const char value : std::string_view("amaR")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)),
               "append mixed-case retroflex key");
    }
    expectEqual(engine.directOutput(), "আমার",
                "dictionary case-folded key keeps the preferred spelling");

    engine.reset();
    for (const char value : std::string_view("DhaporkaTHi")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)),
               "append DhaporkaTHi key");
    }
    expectEqual(engine.directOutput(), "ঢাপরকাঠী",
                "dictionary canonicalizes the requested spelling");

    engine.reset();
    expectEqual(lekho::bn::canonicalizeBangla("\xE0\xA6\xA8\xE0\xA6\xAF\xE0\xA6\xBC"),
                "\xE0\xA6\xA8\xE0\xA7\x9F", "Bengali decomposed y normalization");
    expectEqual(lekho::bn::canonicalizeBangla("\xE0\xA6\xAC\xE0\xA6\xBE\xE0\xA6\xA1\xE0\xA6\xBC\xE0\xA6\xBF"),
                "\xE0\xA6\xAC\xE0\xA6\xBE\xE0\xA7\x9C\xE0\xA6\xBF",
                "Bengali decomposed r normalization");

    lekho::bn::DictionaryBuilder contextBuilder;
    contextBuilder.add("ami", "আমি", 100, "আমরা|আমার");
    lekho::bn::InputEngine predictionEngine;
    expect(predictionEngine.loadDictionary(contextBuilder.build()),
           "prediction engine dictionary load");
    predictionEngine.setContextWord("আমি");
    // Predictions only surface while they match what is being typed: the
    // hint আমরা appears for a compatible first letter and stays hidden for
    // an incompatible one.
    expect(predictionEngine.appendCodePoint('a'), "append compatible first letter");
    const auto predicted = predictionEngine.candidates();
    expect(std::any_of(predicted.begin(), predicted.end(), [](const auto& candidate) {
               return candidate.utf8 == "আমরা";
           }),
           "context prediction is added to the next word candidates");
    predictionEngine.reset();
    expect(predictionEngine.appendCodePoint('x'), "append incompatible letter");
    const auto hidden = predictionEngine.candidates();
    expect(std::none_of(hidden.begin(), hidden.end(), [](const auto& candidate) {
               return candidate.utf8 == "আমরা";
           }),
           "context prediction stays hidden for incompatible input");
}

void testBackspaceEditing() {
    lekho::bn::DictionaryBuilder builder;
    builder.add("bangla", "বাংলা", 10000);
    builder.add("bangladesh", "বাংলাদেশ", 10000);
    builder.add("bangladesh", "বাংলাদেশি", 6000);
    builder.add("sonar", "সোনার", 10000);

    lekho::bn::InputEngine engine;
    expect(engine.loadDictionary(builder.build()), "backspace dictionary load");

    engine.reset();
    for (const char value : std::string_view("bangladesh")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)),
               "append for backspace test");
    }
    expectEqual(engine.rawInput(), "bangladesh", "raw input before backspace");
    expectEqual(engine.directOutput(), "বাংলাদেশ", "canonical output before backspace");

    expect(engine.backspace(), "backspace accepted");
    expectEqual(engine.rawInput(), "banglades", "backspace drops exactly one code point");
    expectEqual(engine.directOutput(), "বাংলাদেস", "recomposed output after backspace");
    expect(engine.candidates().size() >= 2, "suggestions are recomputed after backspace");
    bool suggestionsOfferOriginal = false;
    for (const auto& candidate : engine.candidates()) {
        if (candidate.utf8 == "বাংলাদেশ" || candidate.utf8 == "বাংলাদেশি") {
            suggestionsOfferOriginal = true;
        }
    }
    expect(suggestionsOfferOriginal, "backspaced word keeps dictionary suggestions");

    // Rewriting the deleted character must not leave stale input behind.
    expect(engine.backspace(), "second backspace accepted");
    expectEqual(engine.rawInput(), "banglade", "second backspace state");
    expect(engine.appendCodePoint('s'), "retype the deleted character");
    expectEqual(engine.rawInput(), "banglades", "retyped first character");
    expect(engine.appendCodePoint('h'), "retype the last deleted character");
    expectEqual(engine.rawInput(), "bangladesh", "retyped raw input has no leftovers");
    expectEqual(engine.directOutput(), "বাংলাদেশ", "retyped direct output");
    for (int step = 0; step < 5; ++step) {
        expect(engine.backspace(), "repeated backspace accepted");
    }
    expectEqual(engine.rawInput(), "bangl", "backspacing down to a short prefix");
    expect(engine.appendCodePoint('a'), "type again after backspacing");
    expectEqual(engine.rawInput(), "bangla", "typing after backspace restarts cleanly");
    expectEqual(engine.directOutput(), "বাংলা", "typing after backspace converts correctly");

    engine.reset();
    for (const char value : std::string_view("sonar")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)), "append sonar key");
    }
    expectEqual(engine.directOutput(), "সোনার", "sonar direct conversion");
    engine.reset();
    expect(!engine.backspace(), "backspace on empty input is rejected");
    expectEqual(engine.rawInput(), "", "rejected backspace keeps state unchanged");
}

void testPhraseLayer() {
    // A phrase layer must beat both the core dictionary and the raw
    // transliteration, and must do so without touching the core dictionary.
    lekho::bn::DictionaryBuilder core;
    core.add("jiboner", "জিবনের", 100);
    core.add("dhaka", "ঢাকা", 100);

    lekho::bn::DictionaryBuilder phrase;
    phrase.add("jiboner", "জীবনের", 100);
    phrase.add("vuti", "বুতি", 100);

    lekho::bn::InputEngine engine;
    expect(engine.loadDictionary(core.build()), "phrase test core load");
    expect(!engine.phrasesLoaded(), "no phrase layer before it is loaded");

    const std::string phraseImage = phrase.build();
    expect(engine.loadPhrases(phraseImage), "phrase layer loads");
    expect(engine.phrasesLoaded(), "phrase layer is reported loaded");

    // A phrase that also exists in the core dictionary wins on the exact key.
    engine.reset();
    for (const char value : std::string_view("jiboner")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)), "append jiboner");
    }
    expectEqual(engine.directOutput(), "জীবনের", "phrase outranks the core dictionary");

    // A phrase the core dictionary does not know at all is committed directly.
    engine.reset();
    for (const char value : std::string_view("vuti")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)), "append vuti");
    }
    expectEqual(engine.directOutput(), "বুতি", "an unknown-to-core phrase is committed");

    // An exact core match still applies when no phrase covers the key.
    engine.reset();
    for (const char value : std::string_view("dhaka")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)), "append dhaka");
    }
    expectEqual(engine.directOutput(), "ঢাকা", "core canonical spelling is untouched");

    // A partial phrase shows up in the list without hijacking the direct text.
    engine.reset();
    for (const char value : std::string_view("vut")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)), "append vut");
    }
    expect(!engine.directOutput().empty(), "partial phrase still produces text");

    // Reloading an empty layer clears it and restores core-only behaviour.
    expect(engine.loadPhrases({}), "phrase layer can be cleared");
    expect(!engine.phrasesLoaded(), "cleared phrase layer is reported unloaded");
    engine.reset();
    for (const char value : std::string_view("vuti")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)), "re-append vuti");
    }
    expect(engine.directOutput() != "বুতি", "cleared phrase no longer overrides");

    // A corrupt phrase image must be rejected without disturbing the engine.
    expect(!engine.loadPhrases("not a dictionary image at all"),
           "a corrupt phrase image is rejected");
    expect(engine.dictionaryLoaded(), "the core dictionary survives a bad phrase image");

    // A deliberate capital must never be swallowed by a case-folded phrase key.
    {
        lekho::bn::DictionaryBuilder phraseOnly;
        phraseOnly.add("valobasi", "ভালোবাসি", 100);
        lekho::bn::InputEngine capitalEngine;
        expect(capitalEngine.loadDictionary(core.build()), "capital test core load");
        expect(capitalEngine.loadPhrases(phraseOnly.build()), "capital test phrase load");

        capitalEngine.reset();
        for (const char value : std::string_view("valobasi")) {
            expect(capitalEngine.appendCodePoint(static_cast<unsigned char>(value)),
                   "append valobasi");
        }
        expectEqual(capitalEngine.directOutput(), "ভালোবাসি",
                    "a lowercase key takes the phrase");

        capitalEngine.reset();
        for (const char value : std::string_view("valObasI")) {
            expect(capitalEngine.appendCodePoint(static_cast<unsigned char>(value)),
                   "append valObasI");
        }
        expectEqual(capitalEngine.directOutput(), "ভালোবাসী",
                    "an explicit capital bypasses the phrase layer");
    }
}

void testPhonetics() {
    const lekho::bn::PhoneticTransliterator converter;
    expectEqual(converter.transliterate("bangla"), "বাংলা", "bangla");
    expectEqual(converter.transliterate("bangladesh"), "বাংলাদেশ", "bangladesh");
    expectEqual(converter.transliterate("ami"), "আমি", "ami");
    expectEqual(converter.transliterate("kitab"), "কিতাব", "kitab");
    expectEqual(converter.transliterate("Dhaka"), "ঢাকা", "Dhaka");
    expectEqual(converter.transliterate("ghor"), "ঘর", "inherent o before consonant");
    expectEqual(converter.transliterate("kore"), "করে", "o before r uses inherent vowel");
    expectEqual(converter.transliterate("koto"), "কত", "inherent o at word end");
    expectEqual(converter.transliterate("kemon"), "কেমন", "inherent o before n at word end");
    expectEqual(converter.transliterate("mone"), "মনে", "inherent o before n with a vowel sign");
    expectEqual(converter.transliterate("shonibar"), "শনিবার",
                "inherent o before n keeps the sh spelling");
    expectEqual(converter.transliterate("dhon"), "ধন", "inherent o before n at word end");
    expectEqual(converter.transliterate("ng"), "ং", "word-final ng is the anusvara");
    expectEqual(converter.transliterate("nga"),
                "\xE0\xA6\x99\xE0\xA6\xBE", "ng before a vowel sign is the letter ঙ");
    expectEqual(converter.transliterate("ch"), "ছ", "ch is ছ");
    expectEqual(converter.transliterate("c"), "চ", "c is চ");
    expectEqual(converter.transliterate("chhobi"),
                "\xE0\xA6\x9B\xE0\xA6\xAC\xE0\xA6\xBF", "chh is ছ, not চহ");
    expectEqual(converter.transliterate("ss"), "\xE0\xA6\xB7", "ss is ষ, not a bare স pair");
    expectEqual(converter.transliterate("kkh"),
                "\xE0\xA6\x95\xE0\xA7\x8D\xE0\xA6\x96", "kkh is ক্খ");
    expectEqual(converter.transliterate("kk"),
                "\xE0\xA6\x95\xE0\xA7\x8D\xE0\xA6\x95", "kk stays ক্ক");
    // kt is ক্ত, the dental conjunct, so mukti and akta come out right.
    expectEqual(converter.transliterate("kt"),
                "\xE0\xA6\x95\xE0\xA7\x8D\xE0\xA6\xA4", "kt is ক্ত");
    expectEqual(converter.transliterate("kT"),
                "\xE0\xA6\x95\xE0\xA7\x8D\xE0\xA6\x9F", "kT is the retroflex ক্ট");
    expectEqual(converter.transliterate("mukti"),
                "\xE0\xA6\xAE\xE0\xA7\x81\xE0\xA6\x95\xE0\xA7\x8D\xE0\xA6\xA4\xE0\xA6\xBF",
                "mukti is মুক্তি");
    expectEqual(converter.transliterate("sh"), "শ", "sh is শ");
    expectEqual(converter.transliterate("kh"), "খ", "kh is খ");
    expectEqual(converter.transliterate("khela"),
                "\xE0\xA6\x96\xE0\xA7\x87\xE0\xA6\xB2\xE0\xA6\xBE", "khela spelling");
    expectEqual(converter.transliterate("bangla"), "বাংলা", "bangla keeps the anusvara ng");
    expectEqual(converter.transliterate("bangladesh"), "বাংলাদেশ",
                "bangladesh keeps the anusvara ng");
    expectEqual(converter.transliterate("valObasi"),
                "\xE0\xA6\xAD\xE0\xA6\xBE\xE0\xA6\xB2\xE0\xA7\x8B\xE0\xA6\xAC\xE0\xA6\xBE\xE0\xA6\xB8\xE0\xA6\xBF",
                "capital O after a consonant is the ো sign");
    expectEqual(converter.transliterate("valObasI"),
                "\xE0\xA6\xAD\xE0\xA6\xBE\xE0\xA6\xB2\xE0\xA7\x8B\xE0\xA6\xAC\xE0\xA6\xBE\xE0\xA6\xB8\xE0\xA7\x80",
                "capital I after a consonant is the long ী sign");
    expectEqual(converter.transliterate("majI"),
                "\xE0\xA6\xAE\xE0\xA6\xBE\xE0\xA6\x9C\xE0\xA7\x80",
                "majI spelling uses the long ী sign");
    expectEqual(converter.transliterate("valobasi"),
                "\xE0\xA6\xAD\xE0\xA6\xBE\xE0\xA6\xB2\xE0\xA6\xAC\xE0\xA6\xBE\xE0\xA6\xB8\xE0\xA6\xBF",
                "lowercase i after a consonant is the short ি sign");
    expectEqual(converter.transliterate("mU"), "\xE0\xA6\xAE\xE0\xA7\x82",
                "capital U after a consonant is the long ূ sign");
    expectEqual(converter.transliterate("mu"), "\xE0\xA6\xAE\xE0\xA7\x81",
                "lowercase u after a consonant is the short ু sign");
    expectEqual(converter.transliterate("thii"),
                "\xE0\xA6\xA5\xE0\xA7\x80", "doubled i is the long ী sign");
    // The jibon stem takes the long ii-sign in every derivative.
    expectEqual(converter.transliterate("jibon"),
                "\xE0\xA6\x9C\xE0\xA7\x80\xE0\xA6\xAC\xE0\xA6\xA8", "jibon uses the long ী sign");
    expectEqual(converter.transliterate("jiboner"),
                "\xE0\xA6\x9C\xE0\xA7\x80\xE0\xA6\xAC\xE0\xA6\xA8\xE0\xA7\x87\xE0\xA6\xB0",
                "jiboner is জীবনের");
    // Neighbouring ji- words keep the short sign and must not be affected.
    expectEqual(converter.transliterate("jit"),
                "\xE0\xA6\x9C\xE0\xA6\xBF\xE0\xA6\xA4", "jit is জিত");
    expectEqual(converter.transliterate("jibi"),
                "\xE0\xA6\x9C\xE0\xA6\xBF\xE0\xA6\xAC\xE0\xA6\xBF", "jibi is জিবি");
    expectEqual(converter.transliterate("jirayeki"),
                "\xE0\xA6\x9C\xE0\xA6\xBF\xE0\xA6\xB0\xE0\xA7\x9F\xE0\xA7\x87\xE0\xA6\x95\xE0\xA6\xBF",
                "jirayeki keeps the short sign");
    expectEqual(converter.transliterate("bA"),
                "\xE0\xA6\xAC\xE0\xA6\xBE", "capital A after a consonant is the া sign");
    expectEqual(converter.transliterate("pIti"),
                "\xE0\xA6\xAA\xE0\xA7\x80\xE0\xA6\xA4\xE0\xA6\xBF",
                "capital I after a consonant is the long ী sign");
    expectEqual(converter.transliterate("kEra"),
                "\xE0\xA6\x95\xE0\xA7\x87\xE0\xA6\xB0\xE0\xA6\xBE",
                "capital E after a consonant is the ে sign");
    expectEqual(converter.transliterate("jOl"),
                "\xE0\xA6\x9C\xE0\xA7\x8B\xE0\xA6\xB2", "jOl spelling");
    // A capital vowel that does not follow a consonant stays independent.
    expectEqual(converter.transliterate("A"), "আ", "standalone A is আ");
    expectEqual(converter.transliterate("O"), "ও", "standalone O is ও");
    expectEqual(converter.transliterate("I"), "ঈ", "standalone I is ঈ");
    expectEqual(converter.transliterate("Amar"), "আমার", "leading A is the independent vowel");
    expectEqual(converter.transliterate("Ora"), "ওরা", "leading O is the independent vowel");
    // The lowercase inherent-o rule is untouched.
    expectEqual(converter.transliterate("ghor"), "ঘর", "lowercase o stays inherent");
    expectEqual(converter.transliterate("koto"), "কত", "lowercase word-final o stays inherent");
    expectEqual(converter.transliterate("sonar"), "সোনার", "son prefix carries a pre-base ো");
    expectEqual(converter.transliterate("sona"), "সোনা", "sona spelling");
    expectEqual(converter.transliterate("son"), "সোন", "son prefix on its own");
    expectEqual(converter.transliterate("sonali"), "সোনালি", "sonali spelling");
    expectEqual(converter.transliterate("sonta"),
                "\xE0\xA6\xB8\xE0\xA6\xA8\xE0\xA7\x8D\xE0\xA6\x9F\xE0\xA6\xBE",
                "a consonant after n keeps the inherent o");
    expectEqual(converter.transliterate("noy"), "\xE0\xA6\xA8\xE0\xA7\x9F", "ya nukta spelling");
    expectEqual(converter.transliterate("bari"), "\xE0\xA6\xAC\xE0\xA6\xBE\xE0\xA7\x9C\xE0\xA6\xBF",
                "ri forms retroflex i");
    expectEqual(converter.transliterate("aath"), "আট", "whole-word aath alias");
    expectEqual(converter.transliterate("choy"), "\xE0\xA6\x9B\xE0\xA7\x9F",
                "whole-word choy alias");
    expectEqual(converter.transliterate("keu"), "কে", "coalesce trailing u");
    expectEqual(converter.transliterate("Th"), "ঠ", "capital Th is retroflex th");
    expectEqual(converter.transliterate("DhaporkaThii"), "ঢাপরকাঠী",
                "DhaporkaThii long-i spelling");
    expectEqual(converter.transliterate("uu"), "ঊ", "standalone long u");
    expectEqual(converter.transliterate("bhalo"), "ভালো", "bhalo");
    expectEqual(converter.transliterate("amar bangla khub bhalo"), "আমার বাংলা খুব ভালো", "common phrase");
    expectEqual(converter.transliterate("123"), "১২৩", "Bangla digits");
    expectEqual(converter.transliterate("12.5"), "১২.৫", "decimal point");
    expectEqual(converter.transliterate("."), "\xE0\xA5\xA4", "standalone period is the danda");
    expectEqual(converter.transliterate("1\\."), "১.",
                "escaped dot stays a literal doshomik");
    expectEqual(converter.transliterate("bangla?"), "বাংলা?", "punctuation");
    expectEqual(converter.transliterate("`e"), "e", "literal escape");
    expectEqual(converter.transliterate("বাংলা"), "বাংলা", "Bangla passthrough");
    expectEqual(converter.transliterate("Bangla"), "বাংলা", "mixed-case phonetic word");
    expectEqual(converter.transliterate("@"), "@", "literal symbol is preserved");

    for (int first = 0x20; first <= 0x7E; ++first) {
        for (int second = 0x20; second <= 0x7E; ++second) {
            std::string input(1, static_cast<char>(first));
            input.push_back(static_cast<char>(second));
            if (!lekho::bn::isValidUtf8(converter.transliterate(input))) {
                expect(false, "all ASCII pairs must produce valid UTF-8");
                break;
            }
        }
    }
}

} // namespace

void testSelectionMemory() {
    lekho::bn::DictionaryBuilder builder;
    expect(builder.add("amar", "আমার", 9000), "add amar");
    expect(builder.add("amra", "আমরা", 8000), "add amra");
    expect(builder.add("ami", "আমি", 9000), "add ami");
    lekho::bn::InputEngine engine;
    expect(engine.loadDictionary(builder.build()), "memory engine dictionary");
    engine.setUserDay(100);
    for (const char value : std::string_view("am")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)), "append am");
    }
    expect(engine.candidates().size() >= 2, "am offers choices");

    // The user picks another candidate: record it and verify the boost.
    engine.recordSelection("am", "আমরা", 100);
    engine.reset();
    for (const char value : std::string_view("am")) {
        engine.appendCodePoint(static_cast<unsigned char>(value));
    }
    expect(!engine.candidates().empty() && engine.candidates().front().utf8 == "আমরা",
           "remembered selection takes slot zero");

    // Case-folded keys share one memory entry.
    engine.reset();
    for (const char value : std::string_view("AM")) {
        engine.appendCodePoint(static_cast<unsigned char>(value));
    }
    expect(!engine.candidates().empty() && engine.candidates().front().utf8 == "আমরা",
           "memory is case insensitive");

    // The memory reflects the user's own choice, so it applies regardless of
    // typed case, unlike the phrase layer's case heuristic.
    engine.reset();
    for (const char value : std::string_view("aM")) {
        engine.appendCodePoint(static_cast<unsigned char>(value));
    }
    expect(!engine.candidates().empty() && engine.candidates().front().utf8 == "আমরা",
           "memory applies to mixed-case typing");

    // A second remembered word ranks by recency-weighted count: the more
    // recent pick takes slot zero, the older one sits right behind it.
    engine.recordSelection("am", "আমি", 200);
    engine.setUserDay(200);
    engine.reset();
    for (const char value : std::string_view("am")) {
        engine.appendCodePoint(static_cast<unsigned char>(value));
    }
    expect(!engine.candidates().empty() && engine.candidates().front().utf8 == "আমি",
           "the more recent pick leads");
    expect(engine.candidates().size() >= 2 && engine.candidates()[1].utf8 == "আমরা",
           "the older remembered word follows");

    // Re-picking the older word at a later day flips the order again: count
    // times recency, not last-write-wins.
    engine.recordSelection("am", "আমরা", 300);
    engine.setUserDay(300);
    engine.reset();
    for (const char value : std::string_view("am")) {
        engine.appendCodePoint(static_cast<unsigned char>(value));
    }
    expect(!engine.candidates().empty() && engine.candidates().front().utf8 == "আমরা",
           "a refreshed pick overtakes the leader");

    // Recency decay: after two half-lives a stale single pick falls behind a
    // fresh one.
    engine.setUserDay(300 + 2 * lekho::bn::SelectionMemory::kDecayHalfLifeDays);
    engine.reset();
    for (const char value : std::string_view("am")) {
        engine.appendCodePoint(static_cast<unsigned char>(value));
    }
    expect(!engine.candidates().empty() && engine.candidates().front().utf8 == "আমরা",
           "a counted pick outlives a decayed one");
    engine.setUserDay(100);

    // Persistence round trip. The saved image is BGS2.
    const std::string image = engine.userSelectionsBytes();
    expect(!image.empty(), "memory serializes");
    expect(image.size() >= 4 && image[0] == 'B' && image[1] == 'G' && image[2] == 'S' &&
               image[3] == '2',
           "memory saves in the BGS2 format");
    lekho::bn::InputEngine restored;
    expect(restored.loadDictionary(builder.build()), "restored dictionary");
    restored.loadUserSelections(image);
    expect(restored.userSelectionCount() == 2, "memory reloads");
    restored.setUserDay(300);
    for (const char value : std::string_view("am")) {
        restored.appendCodePoint(static_cast<unsigned char>(value));
    }
    expect(!restored.candidates().empty() && restored.candidates().front().utf8 == "আমরা",
           "restored memory still boosts");

    // BGS1 images (one word per key, from the previous release) import as
    // non-decaying count-1 entries.
    const unsigned char bgs1[]{
        0x42, 0x47, 0x53, 0x31, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0xD1, 0xB0, 0x5B, 0xC6, 0x00, 0x52, 0xAC, 0x9B, 0xA6, 0x6B, 0x5A, 0x90,
        0x51, 0xD6, 0xBB, 0xA4, 0x0C, 0x00, 0xE0, 0xA6, 0x86, 0xE0, 0xA6, 0xAE,
        0xE0, 0xA6, 0xB0, 0xE0, 0xA6, 0xBE, 0x05, 0x00, 0x00, 0x00, 0x4D, 0x00,
        0x00, 0x00, 0x3A, 0x8A, 0x73, 0xC5, 0x76, 0x99, 0x48, 0x6B};
    lekho::bn::SelectionMemory legacy;
    expect(legacy.load(std::string(reinterpret_cast<const char*>(bgs1), sizeof(bgs1))),
           "a BGS1 image imports");
    expect(legacy.size() == 1, "BGS1 import keeps the entry");
    expect(legacy.find("am", 100000).size() == 1 &&
               legacy.find("am", 100000).front() == "আমরা",
           "imported entries do not decay away");

    // A full key group evicts its weakest member for a newcomer.
    lekho::bn::SelectionMemory group;
    group.record("k", "এক", 10);
    group.record("k", "দুই", 10);
    group.record("k", "তিন", 10);
    group.record("k", "চার", 500);
    const auto survivors = group.find("k", 500);
    expect(survivors.size() == 3, "a key keeps at most three words");
    expect(survivors.front() == "চার", "the newcomer leads a full group");
    expect(std::find(survivors.begin(), survivors.end(), "এক") == survivors.end(),
           "the stalest member was evicted");

    // Corruption never blocks typing.
    std::string corrupt = image;
    corrupt[10] = static_cast<char>(corrupt[10] ^ 0x5A);
    lekho::bn::InputEngine damaged;
    damaged.loadDictionary(builder.build());
    damaged.loadUserSelections(corrupt);
    expect(damaged.userSelectionCount() == 0, "corrupt memory is rejected");
    for (const char value : std::string_view("am")) {
        damaged.appendCodePoint(static_cast<unsigned char>(value));
    }
    expect(!damaged.candidates().empty(), "typing continues without memory");
}

void testScoreFloor() {
    // A synthetic corpus with a known score distribution: nine words at 100,
    // three at 300, one at 50. The median floor is 100, independent of any
    // fixed constant, and the fuzzy slot-zero gate reads it.
    lekho::bn::DictionaryBuilder builder;
    struct Row {
        const char* key;
        const char* word;
    };
    const Row low[]{
        {"ka", "কা"}, {"ki", "কি"}, {"ku", "কু"}, {"ke", "কে"}, {"ko", "কো"},
        {"kha", "খা"}, {"khi", "খি"}, {"khu", "খু"}, {"khe", "খে"},
    };
    for (const Row& row : low) {
        expect(builder.add(row.key, row.word, 100), std::string("add ") + row.key);
    }
    expect(builder.add("ga", "গা", 300), "add ga");
    expect(builder.add("gi", "গী", 300), "add gi");
    expect(builder.add("koka", "কোকা", 300), "add koka");
    expect(builder.add("poka", "পোকা", 50), "add the below-floor neighbour");

    lekho::bn::Dictionary dictionary;
    expect(dictionary.load(builder.build()), "floor dictionary loads");
    expect(dictionary.scoreFloor() == 100, "the floor is the median score");

    lekho::bn::InputEngine engine;
    expect(engine.loadDictionary(builder.build()), "floor engine dictionary");

    // Out-of-vocabulary input one edit from a below-floor word keeps the raw
    // conversion in slot zero; the neighbour still appears further down.
    for (const char value : std::string_view("paka")) {
        engine.appendCodePoint(static_cast<unsigned char>(value));
    }
    expect(!engine.candidates().empty() && engine.candidates().front().utf8 == "পাকা",
           "a below-floor neighbour does not hijack slot zero");
    engine.reset();

    // One edit from an at-or-above-floor word takes slot zero: the classic
    // typo-correction case (bangaldesh -> bangladesh) must survive the gate.
    // The key is five characters because the fuzzy search needs four.
    for (const char value : std::string_view("kokas")) {
        engine.appendCodePoint(static_cast<unsigned char>(value));
    }
    expect(!engine.candidates().empty() && engine.candidates().front().utf8 == "কোকা",
           "an at-floor neighbour corrects slot zero");
}

void testTrigramModel() {
    // Hand-built BGTG1: bigram (empty,"আমি") -> [ভালোবাসি, ভালো];
    // trigram ("আমি","ভালো") -> [বাংলা].
    const unsigned char fixture[]{
        0x42, 0x47, 0x54, 0x47, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x39, 0x00, 0x00, 0x00, 0x0A, 0x9C, 0x81, 0xBE,
        0xB3, 0x1C, 0xDD, 0xAC, 0x00, 0x00, 0x00, 0x00, 0x02, 0x09, 0x3E, 0xF9,
        0x67, 0x78, 0xFF, 0x8F, 0x24, 0x28, 0x00, 0x00, 0x00, 0x01, 0x18, 0x00,
        0xE0, 0xA6, 0xAD, 0xE0, 0xA6, 0xBE, 0xE0, 0xA6, 0xB2, 0xE0, 0xA7, 0x8B,
        0xE0, 0xA6, 0xAC, 0xE0, 0xA6, 0xBE, 0xE0, 0xA6, 0xB8, 0xE0, 0xA6, 0xBF,
        0x0C, 0x00, 0xE0, 0xA6, 0xAD, 0xE0, 0xA6, 0xBE, 0xE0, 0xA6, 0xB2, 0xE0,
        0xA7, 0x8B, 0x0F, 0x00, 0xE0, 0xA6, 0xAC, 0xE0, 0xA6, 0xBE, 0xE0, 0xA6,
        0x82, 0xE0, 0xA6, 0xB2, 0xE0, 0xA6, 0xBE, 0x48, 0x81, 0x70, 0xDD, 0xF9,
        0xB3, 0x16, 0xEF};
    const std::string image(reinterpret_cast<const char*>(fixture), sizeof(fixture));

    lekho::bn::TrigramModel model;
    expect(model.load(image), "trigram fixture loads");
    expect(model.isLoaded(), "trigram model is active");

    const auto bigram = model.next("", "আমি", 5);
    expect(bigram.size() == 2 && bigram[0] == "ভালোবাসি" && bigram[1] == "ভালো",
           "bigram context returns frequency-ordered continuations");

    const auto trigram = model.next("আমি", "ভালো", 5);
    expect(trigram.size() == 1 && trigram[0] == "বাংলা",
           "trigram context outranks the bigram fallback");

    expect(model.next("অন্য", "আমি", 5).size() == 2,
           "an unknown trigram context falls back to the bigram tail");
    expect(model.next("", "অজানা", 5).empty(), "unknown bigram yields nothing");
    expect(model.next("", "", 5).empty(), "empty context yields nothing");

    std::string corrupt = image;
    corrupt[20] = static_cast<char>(corrupt[20] ^ 0x5A);
    lekho::bn::TrigramModel damaged;
    expect(!damaged.load(corrupt), "corrupt trigram table is rejected");
    expect(!damaged.isLoaded(), "a rejected table is inactive");

    // Engine integration: committed words build the context chain, and the
    // trigram continuation appears among the next word's candidates.
    lekho::bn::DictionaryBuilder builder;
    expect(builder.add("bhalobashi", "ভালোবাসি", 9000), "add bhalobashi");
    expect(builder.add("bangla", "বাংলা", 9000), "add bangla");
    lekho::bn::InputEngine engine;
    expect(engine.loadDictionary(builder.build()), "trigram engine dictionary");
    engine.loadTrigrams(image);
    expect(engine.trigramsLoaded(), "trigram engine loads the table");
    engine.setContextWord("আমি");
    for (const char value : std::string_view("bha")) {
        expect(engine.appendCodePoint(static_cast<unsigned char>(value)), "append bha");
    }
    bool sawContinuation = false;
    for (const auto& candidate : engine.candidates()) {
        if (candidate.utf8 == "ভালোবাসি") {
            sawContinuation = true;
        }
    }
    expect(sawContinuation, "the trigram continuation reaches the candidate list");
}

void testNuktaSpelling() {
    // gur/guR are different words in Bangla (গুর vs গুড়). The dictionary
    // keys keep the distinction: guR resolves to গুড় exactly, gur does not.
    lekho::bn::DictionaryBuilder builder;
    expect(builder.add("guR", "গুড়", 9000), "add guR");
    expect(builder.add("gur", "গুর", 8000), "add gur");
    lekho::bn::InputEngine engine;
    expect(engine.loadDictionary(builder.build()), "nukta engine dictionary");

    engine.reset();
    for (const char value : std::string_view("guR")) {
        engine.appendCodePoint(static_cast<unsigned char>(value));
    }
    expect(!engine.candidates().empty() && engine.candidates().front().utf8 == "গুড়",
           "guR commits the nukta word");

    engine.reset();
    for (const char value : std::string_view("gur")) {
        engine.appendCodePoint(static_cast<unsigned char>(value));
    }
    expect(engine.candidates().empty() || engine.candidates().front().utf8 != "গুড়",
           "gur does not commit the nukta word");
}

int main() {
    testUtf8();
    testWordPrefix();
    testDictionary();
    testInputEngine();
    testSelectionMemory();
    testScoreFloor();
    testTrigramModel();
    testNuktaSpelling();
    testBackspaceEditing();
    testPhraseLayer();
    testPhonetics();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }

    std::cout << "All core tests passed.\n";
    return 0;
}
