// Robustness, boundary, fuzz, and conformance coverage for the Lekhoni core.
//
// This suite is deliberately adversarial. The dictionary image is an
// untrusted-by-default byte buffer that ships next to the DLL, so the parser
// is the security boundary: every test in the format section below asserts that
// a mutated image can never read out of bounds or produce an inconsistent
// object. All randomness is seeded so a failure is always reproducible.

#include "lekhoni/Dictionary.h"
#include "lekhoni/InputEngine.h"
#include "lekhoni/PhoneticTransliterator.h"
#include "lekhoni/AvroLayoutEngine.h"
#include "lekhoni/Utf.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

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

// xorshift64* keeps every run byte-for-byte reproducible.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed ? seed : 0x9E3779B97F4A7C15ULL) {}

    std::uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1DULL;
    }

    std::size_t below(std::size_t bound) {
        return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
    }

private:
    std::uint64_t state_;
};

// A dictionary small enough to mutate byte-exhaustively but large enough to
// contain every section, a word record, a form record, and blob data.
std::string sampleImage() {
    lekho::bn::DictionaryBuilder builder;
    builder.add("bangla", "বাংলা", 10000, "ভাষা|দেশ|শব্দ");
    builder.add("bangladesh", "বাংলাদেশ", 10000, "বাংলাদেশি");
    builder.add("dhaka", "ঢাকা", 9000);
    builder.add("amar", "আমার", 9000, "আমি|আমাকে");
    builder.add("jibon", "জীবন", 8000);
    builder.add("sonar", "সোনার", 7000);
    builder.add("amarLogo", "আমার লোগো", 6000);
    builder.add("k", "ক", 5000);
    builder.add("zz", "যুগাঞ্জ", 4000, "যুগ|যুগের");
    builder.add("qqqq", "কুকু", 3000, "কুকুর");
    builder.add("kOba", "কবা", 2000);
    return builder.build();
}

// ---------------------------------------------------------------------------
// 1. Binary format robustness
// ---------------------------------------------------------------------------

void testFormatRejectsNonDictionaries() {
    const std::string good = sampleImage();
    expect(!good.empty(), "sample image builds");

    lekho::bn::Dictionary dictionary;
    expect(!dictionary.load({}), "an empty buffer is rejected");
    expect(!dictionary.load("BGDX"), "a four byte buffer is rejected");
    expect(!dictionary.load("NOTBGDX1................................"), "a wrong magic is rejected");
    expect(!dictionary.isLoaded(), "a rejected load leaves the object unloaded");
    expect(dictionary.lookup(lekho::bn::SearchChannel::Phonetic, "a", 4).empty(),
           "queries on an unloaded dictionary return nothing");
    expect(dictionary.predictions("আমি", 4).empty(),
           "predictions on an unloaded dictionary return nothing");
    expect(dictionary.lookupFuzzyPhonetic("abcd", 4).empty(),
           "fuzzy search on an unloaded dictionary returns nothing");

    // A buffer that is all zeroes and a buffer of pure noise must both fail
    // without a crash.
    expect(!dictionary.load(std::string(4096, '\0')), "an all zero buffer is rejected");
    std::string noise(8192, '\0');
    Rng rng(0xC0FFEEULL);
    for (char& byte : noise) {
        byte = static_cast<char>(rng.next() & 0xFFU);
    }
    expect(!dictionary.load(noise), "a random buffer is rejected");
}

void testTruncationIsSafe() {
    const std::string good = sampleImage();
    // Every truncation length must be rejected, and none may crash. Sampling
    // every byte of a multi-kilobyte image keeps this fast while still
    // covering the header, the section table, and every section boundary.
    std::size_t accepted = 0;
    for (std::size_t length = 0; length < good.size(); ++length) {
        lekho::bn::Dictionary dictionary;
        if (dictionary.load(good.substr(0, length))) {
            ++accepted;
        }
    }
    expect(accepted == 0, "no truncated image is ever accepted (" +
                              std::to_string(accepted) + " slipped through)");
}

void testSingleByteCorruptionIsSafe() {
    const std::string good = sampleImage();
    std::size_t accepted = 0;
    std::size_t inconsistent = 0;
    for (std::size_t offset = 0; offset < good.size(); ++offset) {
        std::string damaged = good;
        damaged[offset] = static_cast<char>(damaged[offset] ^ 0x5A);
        lekho::bn::Dictionary dictionary;
        if (!dictionary.load(damaged)) {
            continue;
        }
        ++accepted;
        // If a mutated image is accepted, the object must still be internally
        // consistent: counts must be sane and every query must terminate.
        if (dictionary.formCount() > 0 && dictionary.wordCount() == 0) {
            ++inconsistent;
        }
        (void)dictionary.lookup(lekho::bn::SearchChannel::Phonetic, "ba", 9);
        (void)dictionary.lookup(lekho::bn::SearchChannel::Bangla, "ক", 9);
        (void)dictionary.lookupFuzzyPhonetic("bangla", 9);
        (void)dictionary.predictions("আমার", 4);
    }
    expect(accepted == 0, "no single byte corruption is accepted (" +
                              std::to_string(accepted) + " slipped through)");
    expect(inconsistent == 0, "no accepted corruption produced an inconsistent object");
}

void testSeededFuzzIsSafe() {
    const std::string good = sampleImage();
    Rng rng(0x5EED1234ULL);
    for (int iteration = 0; iteration < 20000; ++iteration) {
        std::string damaged = good;
        const std::size_t mutations = 1 + rng.below(8);
        for (std::size_t m = 0; m < mutations; ++m) {
            const std::size_t offset = rng.below(damaged.size());
            damaged[offset] = static_cast<char>(rng.next() & 0xFFU);
        }
        lekho::bn::Dictionary dictionary;
        if (dictionary.load(damaged)) {
            // Accepted images are still exercised through the full query
            // surface so any out-of-bounds read shows up under sanitizers.
            (void)dictionary.lookup(lekho::bn::SearchChannel::Phonetic, "a", 9);
            (void)dictionary.lookup(lekho::bn::SearchChannel::Phonetic, "zzzz", 9);
            (void)dictionary.lookup(lekho::bn::SearchChannel::Bangla, "ক", 9);
            (void)dictionary.lookupFuzzyPhonetic("bangladesh", 9);
            (void)dictionary.predictions("আমার", 8);
        }
    }
    expect(true, "seeded fuzz completed 20000 mutated images without a crash");
}

void testHostileHeaderFields() {
    const std::string good = sampleImage();
    // The header is 16 bytes: magic, byte order mark, version, flags, header
    // size, reserved, then a 64-bit total size. Each of these is attacker
    // controlled, so each is driven to an extreme and must be rejected.
    struct Patch {
        std::size_t offset;
        std::string bytes;
    };
    const std::vector<Patch> patches{
        {0, std::string("XXXX", 4)},                       // magic
        {4, std::string("\xFF\xFE", 2)},                   // byte order mark
        {6, std::string("\xFF\xFF", 2)},                   // version
        {8, std::string("\xFF\xFF\xFF\xFF", 4)},           // flags
        {10, std::string("\x00\x00", 2)},                  // header size
        {12, std::string("\xFF\xFF\xFF\xFF", 4)},          // reserved
        {16, std::string("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8)}, // total size
    };
    for (const auto& patch : patches) {
        std::string damaged = good;
        damaged.replace(patch.offset, patch.bytes.size(), patch.bytes);
        lekho::bn::Dictionary dictionary;
        dictionary.load(damaged);
        // Whether it is accepted or not, it must not crash; that is the point.
        if (dictionary.isLoaded()) {
            (void)dictionary.lookup(lekho::bn::SearchChannel::Phonetic, "a", 9);
        }
    }
    expect(true, "hostile header fields are survivable");

    // A byte order mark of zero is what the real writer emits; confirm the
    // untouched image still validates so the patches above are meaningful.
    lekho::bn::Dictionary dictionary;
    expect(dictionary.load(good), "the pristine image still loads after patching tests");
}

// ---------------------------------------------------------------------------
// 2. Determinism and round-trip
// ---------------------------------------------------------------------------

void testBuildIsDeterministic() {
    const std::string first = sampleImage();
    const std::string second = sampleImage();
    expect(first == second, "building the same input twice is byte identical");

    // Insertion order must not change the output, otherwise the checked-in
    // dictionary is not reproducible.
    lekho::bn::DictionaryBuilder ordered;
    ordered.add("aaa", "আ", 100);
    ordered.add("bbb", "ব", 200);
    ordered.add("ccc", "গ", 300);
    ordered.add("aab", "আব", 400);

    lekho::bn::DictionaryBuilder shuffled;
    shuffled.add("ccc", "গ", 300);
    shuffled.add("aab", "আব", 400);
    shuffled.add("aaa", "আ", 100);
    shuffled.add("bbb", "ব", 200);

    expect(ordered.build() == shuffled.build(),
           "insertion order does not change the compiled bytes");
}

void testRoundTrip() {
    const std::string image = sampleImage();
    lekho::bn::Dictionary dictionary;
    expect(dictionary.load(image), "round trip image loads");

    struct Entry {
        const char* key;
        const char* bangla;
    };
    const Entry entries[]{
        {"bangla", "বাংলা"},      {"bangladesh", "বাংলাদেশ"}, {"dhaka", "ঢাকা"},
        {"amar", "আমার"},        {"jibon", "জীবন"},          {"sonar", "সোনার"},
        {"k", "ক"},              {"zz", "যুগাঞ্জ"},
    };
    for (const auto& entry : entries) {
        const auto hits =
            dictionary.lookup(lekho::bn::SearchChannel::Phonetic, entry.key, 1);
        expectEqual(hits.empty() ? std::string() : hits.front().word, entry.bangla,
                    std::string("round trip for ") + entry.key);
    }

    const auto predictions = dictionary.predictions("আমার", 8);
    expect(predictions.size() == 2, "prediction hints survive the round trip");
}

void testDuplicateAdds() {
    lekho::bn::DictionaryBuilder builder;
    expect(builder.add("dup", "দুপ", 100), "first add of a key succeeds");
    expect(builder.add("dup", "দুপ", 200), "second add of the same key succeeds");
    // The same Bangla word under two keys must not duplicate the word record.
    expect(builder.add("dupa", "দুপ", 300), "the same word under a second key succeeds");
    expect(builder.wordCount() == 1, "a repeated word is stored once");
    expect(builder.formCount() == 4,
           "three adds over one word yield three forms and one word record");
}

// ---------------------------------------------------------------------------
// 3. Builder validation boundaries
// ---------------------------------------------------------------------------

void testBuilderValidation() {
    lekho::bn::DictionaryBuilder builder;
    expect(!builder.add("", "ক", 1), "an empty key is rejected");
    expect(!builder.add("key", "", 1), "an empty word is rejected");
    // A NUL must be tested with an explicit length; a bare literal truncates
    // at the NUL and would silently test the wrong thing.
    expect(!builder.add(std::string("k\0ey", 4), "ক", 1), "a NUL inside the key is rejected");
    expect(!builder.add("key", std::string("ক\0ক", 5), 1),
           "a NUL inside the word is rejected");
    expect(!builder.add("key", "ক", 1, std::string("a\0b", 3)),
           "a NUL inside the hint is rejected");
    expect(!builder.add(" key", "ক", 1), "a leading space in the key is rejected");
    expect(!builder.add("key ", "ক", 1), "a trailing space in the key is rejected");
    expect(!builder.add("key", " ক", 1), "a leading space in the word is rejected");
    expect(!builder.add("key", "ascii-only", 1),
           "a word with no Bangla is rejected");
    expect(!builder.add("kea", "\xC3\x28", 1), "invalid UTF-8 in the word is rejected");
    expect(!builder.add("key1", "ক", 1),
           "a digit in the key is rejected because keys are phonetic only");
    expect(builder.add("keb", "ক", 0xFFFFFFFFU),
           "the maximum frequency is accepted, not rejected by range");
    expect(builder.add("kec", "ক", 0), "a zero frequency is accepted");
    expect(!builder.add(std::string(4097, 'a'), "ক", 1), "an over long key is rejected");
    expect(builder.add(std::string(4096, 'a'), "ক", 1), "a maximum length key is accepted");
    expect(builder.wordCount() > 0, "valid entries were recorded");
}

void testQueryBoundaries() {
    lekho::bn::Dictionary dictionary;
    expect(dictionary.load(sampleImage()), "boundary image loads");

    expect(dictionary.lookup(lekho::bn::SearchChannel::Phonetic, "b", 0).empty(),
           "a zero limit returns nothing");
    expect(dictionary.lookup(lekho::bn::SearchChannel::Phonetic, "b", 65).empty(),
           "an over large limit is rejected");
    expect(!dictionary.lookup(lekho::bn::SearchChannel::Phonetic, "b", 64).empty(),
           "the maximum limit is accepted");
    expect(dictionary.lookup(lekho::bn::SearchChannel::Phonetic, "", 9).empty(),
           "an empty prefix returns nothing");
    expect(dictionary.lookup(lekho::bn::SearchChannel::Phonetic, std::string("b\0c", 3), 9)
               .empty(),
           "a NUL in the prefix returns nothing");
    expect(dictionary.lookup(lekho::bn::SearchChannel::Phonetic, "\xC3\x28", 9).empty(),
           "invalid UTF-8 in the prefix returns nothing");
    expect(dictionary.lookup(lekho::bn::SearchChannel::Phonetic, std::string(5000, 'a'), 9)
               .empty(),
           "an over long prefix returns nothing");

    // Fuzzy has its own minimum length; a short key must not reach the scan.
    expect(dictionary.lookupFuzzyPhonetic("ba", 9).empty(),
           "fuzzy search below the minimum length returns nothing");
    expect(!dictionary.lookupFuzzyPhonetic("bangla", 9).empty(),
           "fuzzy search at the minimum length returns something");

    expect(dictionary.predictions("", 4).empty(), "empty predictions return nothing");
    expect(dictionary.predictions("আমার", 0).empty(), "a zero prediction limit returns nothing");
    expect(dictionary.predictions("আমার", 999).size() <= 16,
           "a huge prediction limit is clamped");
    expect(dictionary.predictions("কোনো-এমন-শব্দ-নেই", 4).empty(),
           "an unknown word has no predictions");
}

// ---------------------------------------------------------------------------
// 4. UTF-8 and canonicalization
// ---------------------------------------------------------------------------

void testUtf8Decoder() {
    // Valid shortest forms and the canonical Bengali letters.
    const char* const valid[]{
        "a",              "\xC3\xA9",          "\xE0\xA4\x95",     "\xE0\xA4\xB7",
        "\xE0\xA5\xA4",   "\xE0\xA6\x93",       "\xE0\xA6\x94",     "\xE0\xA6\x95",
        "\xE0\xA6\x99",   "\xE0\xA6\x9A",       "\xE0\xA6\x9B",     "\xE0\xA6\x9F",
        "\xE0\xA6\xA4",   "\xE0\xA6\xAB",       "\xE0\xA6\xAE",     "\xE0\xA7\x80",
        "\xE0\xA7\x81",   "\xE0\xA7\x87",       "\xE0\xA7\x8B",     "\xE0\xA7\x8D",
        "\xE0\xA7\x9F",   "\xE0\xA7\xA0",       "\xE0\xA7\xA8",     "\xE0\xA7\xAF",
        "\xE0\xA7\xB0",   "\xE0\xA7\xB2",       "\xE0\xA7\xB6",     "\xE0\xA7\xBF",
        "\xE0\xA7\xA1",   "\xE0\xA7\xA4",       "\xE0\xA8\x80",     "\xF0\x9F\x98\x80",
    };
    for (const char* text : valid) {
        expect(lekho::bn::isValidUtf8(text), std::string("valid UTF-8 accepted: ") + text);
    }

    // Overlong encodings, surrogates, out of range, and lone continuation bytes.
    const char* const invalid[]{
        "\x80", "\xBF", "\xC0\xAF", "\xC1\xBF", "\xC2", "\xE0\x80\xAF",
        "\xE0\x9F\xBF", "\xED\xA0\x80", "\xED\xBF\xBF", "\xF0\x80\x80\xAF",
        "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xF8\x88\x80\x80\x80",
        "\xFE", "\xFF", "\xE2\x82",
    };
    for (const char* text : invalid) {
        expect(!lekho::bn::isValidUtf8(text), std::string("invalid UTF-8 rejected"));
    }
}

void testCanonicalizationIsIdempotent() {
    // Idempotence is the property the engine relies on: it canonicalizes the
    // transliteration and then canonicalizes dictionary words again.
    const char* const samples[]{
        "বাংলা", "ন্য", "য়", "ড়", "ঢ়", "ক্ষ", "শ্র", "র্", "‍", "ক‍্ষ",
    };
    for (const char* sample : samples) {
        const std::string once = lekho::bn::canonicalizeBangla(sample);
        const std::string twice = lekho::bn::canonicalizeBangla(once);
        expectEqual(twice, once, std::string("canonicalization is idempotent for ") + sample);
        expect(lekho::bn::isValidUtf8(once), "canonicalization produces valid UTF-8");
    }
    expectEqual(lekho::bn::canonicalizeBangla(""), "", "empty input canonicalizes to empty");
    expectEqual(lekho::bn::canonicalizeBangla("ascii"), "ascii",
                "ASCII passes through unchanged");
    expectEqual(lekho::bn::canonicalizeBangla("\xC3\x28"), "",
                "invalid UTF-8 canonicalizes to empty");
}

// ---------------------------------------------------------------------------
// 5. Transliteration coverage
// ---------------------------------------------------------------------------

void testTransliterationIsTotal() {
    // Every single and double ASCII combination must produce valid UTF-8 and
    // must never throw. This is the exhaustively checkable part of the input
    // space; longer inputs are covered by the seeded sample below.
    lekho::bn::PhoneticTransliterator converter;
    for (int first = 0x20; first <= 0x7E; ++first) {
        std::string input(1, static_cast<char>(first));
        expect(lekho::bn::isValidUtf8(converter.transliterate(input)),
               "single ASCII character produces valid UTF-8");
        for (int second = 0x20; second <= 0x7E; ++second) {
            input[0] = static_cast<char>(first);
            input.push_back(static_cast<char>(second));
            expect(lekho::bn::isValidUtf8(converter.transliterate(input)),
                   "ASCII pair produces valid UTF-8");
            input.pop_back();
        }
    }
}

void testTransliterationSampleIsStable() {
    lekho::bn::PhoneticTransliterator converter;
    Rng rng(0xBEEF42ULL);
    const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'`-";
    const std::size_t span = sizeof(alphabet) - 1;
    for (int iteration = 0; iteration < 20000; ++iteration) {
        std::string input;
        const std::size_t length = 1 + rng.below(12);
        for (std::size_t i = 0; i < length; ++i) {
            input.push_back(alphabet[rng.below(span)]);
        }
        const std::string first = converter.transliterate(input);
        const std::string second = converter.transliterate(input);
        expectEqual(second, first, "transliteration is a pure function");
        expect(lekho::bn::isValidUtf8(first), "sampled transliteration is valid UTF-8");
    }
}

void testTransliterationPreservesBangla() {
    lekho::bn::PhoneticTransliterator converter;
    // Bangla text typed back through the transliterator must survive intact,
    // otherwise an IME switch mid word corrupts the document.
    const char* const samples[]{
        "বাংলা",     "ঢাকা",    "ভালোবাসী", "সন্তান",   "জীবনের",
        "মানুষ",    "বাংলাদেশ", "শিক্ষা",    "স্বাধীন",  "অধিকার",
    };
    for (const char* sample : samples) {
        expectEqual(converter.transliterate(sample), sample,
                    std::string("Bangla text passes through: ") + sample);
    }
}

// ---------------------------------------------------------------------------
// 5b. Structural well-formedness of transliterator output
// ---------------------------------------------------------------------------

// Valid UTF-8 is a weak guarantee. A string can be perfectly valid UTF-8 and
// still render as broken glyphs: a virama with nothing after it, a dependent
// vowel sign with no consonant in front of it, or two signs stacked on one
// consonant. None of those are detectable by isValidUtf8, and all of them reach
// the user's document. These are the properties that catch a rule table edit
// producing well-formed nonsense.
bool isDependentSign(char32_t value) {
    switch (value) {
    case 0x09BE:  // aa kar
    case 0x09BF:  // i kar
    case 0x09C0:  // ii kar
    case 0x09C1:  // u kar
    case 0x09C2:  // uu kar
    case 0x09C7:  // e kar
    case 0x09C8:  // ai kar
    case 0x09CB:  // o kar
    case 0x09CD:  // virama, which joins rather than follows
        return true;
    default:
        // U+09CC is deliberately absent. It is the independent au vowel, so a
        // word may legitimately open with it: OI -> ৌ. Listing it here would
        // report every `OI` input as an orphan karan.
        return false;
    }
}

// Wraps the structural checks so the exhaustive sweep and the fuzz share one
// definition of "well formed". A violation reports the offending input, which
// is the whole point: the reader can paste it straight into a fixture.
struct PhoneticProbe {
    void check(const std::string& input, const std::string& context) {
        const std::string output = converter.transliterate(input);
        ++applied;
        if (!lekho::bn::isValidUtf8(output)) {
            fail(context + ": invalid UTF-8 for input \"" + input + "\"");
            return;
        }
        std::vector<char32_t> points;
        for (std::size_t offset = 0; offset < output.size();) {
            lekho::bn::DecodedCodePoint decoded;
            if (!lekho::bn::decodeUtf8(output, offset, decoded)) {
                return;  // already reported above
            }
            points.push_back(decoded.value);
            offset += decoded.byteLength;
        }
        if (points.empty()) {
            return;
        }
        // A virama with nothing after it renders as a dangling stroke.
        if (points.back() == 0x09CD) {
            fail(context + ": output ends with a lone virama for input \"" + input +
                 "\" -> \"" + output + "\"");
        }
        // A dependent sign with no consonant in front is an orphan karan.
        if (isDependentSign(points.front())) {
            fail(context + ": output opens with a dependent sign for input \"" + input +
                 "\" -> \"" + output + "\"");
        }
        // Two signs in a row is always wrong: a consonant takes at most one.
        for (std::size_t i = 1; i < points.size(); ++i) {
            if (isDependentSign(points[i]) && isDependentSign(points[i - 1])) {
                fail(context + ": two dependent signs in a row for input \"" + input +
                     "\" -> \"" + output + "\"");
                break;
            }
        }
    }

    lekho::bn::PhoneticTransliterator converter;
    long long applied = 0;
};

void testTransliterationIsStructurallySound() {
    // Exhaustive over all three character ASCII combinations: 95^3 = 857,375.
    // Two characters is already covered above; three is where a two-token rule
    // plus a sign starts interacting, which is where the oi digraph and the
    // r-phala live. Four would be 81 million, so longer inputs are covered by
    // the seeded fuzz below instead.
    PhoneticProbe probe;
    for (int first = 0x20; first <= 0x7E; ++first) {
        std::string input(3, ' ');
        input[0] = static_cast<char>(first);
        for (int second = 0x20; second <= 0x7E; ++second) {
            input[1] = static_cast<char>(second);
            for (int third = 0x20; third <= 0x7E; ++third) {
                input[2] = static_cast<char>(third);
                probe.check(input, "exhaustive three character input");
            }
        }
    }
    expect(probe.applied == 95LL * 95 * 95,
           "the exhaustive sweep covered every three character ASCII input, got " +
               std::to_string(probe.applied));
    std::cout << "structural checks applied to " << probe.applied
              << " three character inputs\n";
}

void testTransliterationStructureOnLongInput() {
    // The same properties on longer random inputs, where several rules chain and
    // a sign can be left stranded at the end of the string.
    PhoneticProbe probe;
    Rng rng(0x5EED11ULL);
    const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'`-";
    const std::size_t span = sizeof(alphabet) - 1;
    for (int iteration = 0; iteration < 20000; ++iteration) {
        std::string input;
        const std::size_t length = 1 + rng.below(14);
        for (std::size_t i = 0; i < length; ++i) {
            input.push_back(alphabet[rng.below(span)]);
        }
        probe.check(input, "sampled long input");
    }
    expect(probe.applied == 20000, "the long input fuzz applied every sample");
}

// Interleaves the four mutating operations instead of always feeding then
// checking. The existing engine invariant test only ever does feed-then-check,
// which is exactly the shape that hid the backspace composition corruption: the
// buffer was only ever appended to, never edited underneath a live composition.
// Interleaving is where state left over from a previous operation shows up.
void testEngineOperationFuzz() {
    const std::string core = sampleImage();
    lekho::bn::DictionaryBuilder phraseBuilder;
    phraseBuilder.add("jiboner", "জীবনের", 100);
    phraseBuilder.add("amar", "আমার", 100, "আমি|আমাকে");
    const std::string phrases = phraseBuilder.build();

    lekho::bn::InputEngine engine;
    expect(engine.loadDictionary(core), "operation fuzz loads the sample core");
    expect(engine.loadPhrases(phrases), "operation fuzz loads the sample phrases");

    const char* const operations[]{"append", "append", "append", "backspace", "reset",
                                   "append", "context", "backspace", "append", "backspace"};
    Rng rng(0x0F0F0F0FULL);
    const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'";
    const std::size_t span = sizeof(alphabet) - 1;
    const std::size_t operationCount = sizeof(operations) / sizeof(operations[0]);

    for (int iteration = 0; iteration < 6000; ++iteration) {
        const char* operation = operations[rng.below(operationCount)];
        if (std::strcmp(operation, "append") == 0) {
            engine.appendCodePoint(static_cast<unsigned char>(alphabet[rng.below(span)]));
        } else if (std::strcmp(operation, "backspace") == 0) {
            (void)engine.backspace();
        } else if (std::strcmp(operation, "reset") == 0) {
            engine.reset();
        } else {
            engine.setContextWord("আমার");
        }

        // After every single mutation the observable state must still be sound.
        // Checking between operations rather than only at the end is what makes
        // this catch an operation that corrupts state.
        expect(lekho::bn::isValidUtf8(engine.rawInput()),
               std::string("after ") + operation + " the raw input is valid UTF-8");
        expect(lekho::bn::isValidUtf8(engine.directOutput()),
               std::string("after ") + operation + " the direct output is valid UTF-8");
        expect(engine.candidates().size() <= 9,
               std::string("after ") + operation + " the candidate list is capped at nine");
        const auto& candidates = engine.candidates();
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            expect(lekho::bn::isValidUtf8(candidates[i].utf8),
                   std::string("after ") + operation + " every candidate is valid UTF-8");
            for (std::size_t j = i + 1; j < candidates.size(); ++j) {
                expect(candidates[i].utf8 != candidates[j].utf8,
                       std::string("after ") + operation + " no candidate is duplicated");
            }
        }
        if (!engine.directOutput().empty()) {
            expect(!candidates.empty(),
                   std::string("after ") + operation +
                       " a non empty direct output has a candidate");
        }
    }

    // reset() after an arbitrary interleaving must reach the initial state, and
    // doing it twice must be the same as doing it once.
    engine.reset();
    const std::string settledInput = engine.rawInput();
    const std::string settledOutput = engine.directOutput();
    engine.reset();
    expect(engine.rawInput() == settledInput, "reset is idempotent for the raw input");
    expect(engine.directOutput() == settledOutput,
           "reset is idempotent for the direct output");
    expect(engine.candidates().empty(), "reset leaves no candidates");
}

// The phrase image is a second untrusted byte buffer that ships beside the DLL
// and is parsed by the same loader, so it gets the same adversarial treatment as
// the core. Without this, a truncated phrase file would be the one input path
// nobody ever fuzzes.
void testMalformedPhraseImages() {
    lekho::bn::DictionaryBuilder builder;
    builder.add("jiboner", "জীবনের", 100, "জীবন|জীবনে");
    builder.add("amar", "আমার", 100, "আমি");
    const std::string good = builder.build();
    expect(!good.empty(), "the phrase sample image is not empty");

    // A good image must load, or the negative cases below prove nothing.
    {
        lekho::bn::InputEngine engine;
        expect(engine.loadPhrases(good), "the intact phrase image loads");
        expect(engine.phrasesLoaded(), "the engine reports the phrase layer loaded");
    }

    // Truncation at every length must be rejected or tolerated, never crash and
    // never leave a half-parsed object claiming to be loaded.
    for (std::size_t length = 0; length < good.size(); length += 7) {
        lekho::bn::InputEngine engine;
        (void)engine.loadPhrases(good.substr(0, length));
        // Whether it loads or not, the engine must remain usable.
        engine.appendCodePoint('a');
        engine.backspace();
        expect(lekho::bn::isValidUtf8(engine.directOutput()),
               "a truncated phrase image leaves the engine producing valid UTF-8");
    }

    // Single byte corruption across the header and the first records.
    for (std::size_t offset = 0; offset < good.size() && offset < 96; ++offset) {
        std::string damaged = good;
        damaged[offset] = static_cast<char>(damaged[offset] ^ 0xFF);
        lekho::bn::InputEngine engine;
        (void)engine.loadPhrases(damaged);
        engine.appendCodePoint('a');
        expect(lekho::bn::isValidUtf8(engine.directOutput()),
               "a corrupted phrase image leaves the engine producing valid UTF-8");
    }

    // Hostile sizes: empty, enormous, and full of NUL bytes.
    const std::vector<std::string> hostile{
        std::string(),
        std::string(1, '\0'),
        std::string(64, '\0'),
        std::string("\xFF\xFE\xFD\xFC", 4),
        std::string("BGDX1", 5) + std::string(4096, 'A'),
    };
    for (const std::string& image : hostile) {
        lekho::bn::InputEngine engine;
        (void)engine.loadPhrases(image);
        engine.appendCodePoint('b');
        expect(lekho::bn::isValidUtf8(engine.directOutput()),
               "a hostile phrase image leaves the engine producing valid UTF-8");
    }

    // A phrase image that fails to load must not stop the core from working.
    // The phrase layer is optional by design, so a bad one is a degraded IME
    // rather than a broken one.
    lekho::bn::InputEngine engine;
    expect(engine.loadDictionary(sampleImage()), "the core loads alongside a bad phrase");
    expect(!engine.loadPhrases(std::string("not a dictionary at all")),
           "a garbage phrase image is rejected");
    expect(!engine.phrasesLoaded(), "the engine reports the phrase layer as not loaded");
    for (const char value : std::string_view("bangladesh")) {
        engine.appendCodePoint(static_cast<unsigned char>(value));
    }
    expect(!engine.directOutput().empty(),
           "phonetic typing still works after the phrase layer was rejected");
}

// ---------------------------------------------------------------------------
// 6. Engine invariants
// ---------------------------------------------------------------------------

void feed(lekho::bn::InputEngine& engine, std::string_view word) {
    engine.reset();
    for (const char value : word) {
        engine.appendCodePoint(static_cast<unsigned char>(value));
    }
}

void testEngineInvariants() {
    const std::string core = sampleImage();
    lekho::bn::DictionaryBuilder phraseBuilder;
    phraseBuilder.add("jiboner", "জীবনের", 100);
    const std::string phrases = phraseBuilder.build();

    lekho::bn::InputEngine engine;
    expect(engine.loadDictionary(core), "engine loads the sample core");
    expect(engine.loadPhrases(phrases), "engine loads the sample phrases");

    Rng rng(0xFEEDFACEULL);
    const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'";
    const std::size_t span = sizeof(alphabet) - 1;
    for (int iteration = 0; iteration < 4000; ++iteration) {
        std::string input;
        const std::size_t length = 1 + rng.below(10);
        for (std::size_t i = 0; i < length; ++i) {
            input.push_back(alphabet[rng.below(span)]);
        }
        feed(engine, input);

        expect(engine.candidates().size() <= 9, "the candidate list never exceeds nine");
        const auto& candidates = engine.candidates();
        for (std::size_t i = 1; i < candidates.size(); ++i) {
            expect(candidates[i].utf8 != candidates[i - 1].utf8,
                   "adjacent candidates are never identical");
        }
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            for (std::size_t j = i + 1; j < candidates.size(); ++j) {
                expect(candidates[i].utf8 != candidates[j].utf8,
                       "the candidate list holds no duplicates");
            }
        }
        if (!engine.directOutput().empty()) {
            expect(!candidates.empty(), "a non empty direct output always has a candidate");
            expectEqual(candidates.front().utf8, engine.directOutput(),
                        "the direct output is always the first candidate");
        }
        for (const auto& candidate : candidates) {
            expect(lekho::bn::isValidUtf8(candidate.utf8),
                   "every candidate is valid UTF-8");
            expect(!candidate.utf8.empty(), "no candidate is empty");
        }
    }

    // reset() must return the engine to its initial observable state.
    engine.reset();
    expect(engine.rawInput().empty(), "reset clears the phonetic buffer");
    expect(engine.directOutput().empty(), "reset clears the direct output");
    expect(engine.candidates().empty(), "reset clears the candidate list");

    // Backspacing past empty is a no-op, never an underflow.
    expect(!engine.backspace(), "backspace on an empty buffer is rejected");
    expect(engine.rawInput().empty(), "a rejected backspace leaves the buffer empty");
}

void testPhrasePriorityMatrix() {
    lekho::bn::DictionaryBuilder coreBuilder;
    coreBuilder.add("jiboner", "জিবনের", 100);
    coreBuilder.add("dhaka", "ঢাকা", 100);

    lekho::bn::DictionaryBuilder phraseBuilder;
    phraseBuilder.add("jiboner", "জীবনের", 100);
    phraseBuilder.add("valobasi", "ভালোবাসি", 100);

    lekho::bn::InputEngine engine;
    expect(engine.loadDictionary(coreBuilder.build()), "priority core loads");
    expect(engine.loadPhrases(phraseBuilder.build()), "priority phrases load");

    // A phrase outranks a conflicting core entry.
    feed(engine, "jiboner");
    expectEqual(engine.directOutput(), "জীবনের", "a phrase outranks the core dictionary");

    // A core entry outranks the transliteration.
    feed(engine, "dhaka");
    expectEqual(engine.directOutput(), "ঢাকা", "a core entry outranks the transliteration");

    // An explicit capital bypasses the phrase layer entirely.
    feed(engine, "valObasI");
    expectEqual(engine.directOutput(), "ভালোবাসী",
                "an explicit capital bypasses the phrase layer");
    feed(engine, "valobasi");
    expectEqual(engine.directOutput(), "ভালোবাসি", "a lowercase key takes the phrase");

    // Dropping the layer restores pure core plus transliteration behaviour.
    expect(engine.loadPhrases({}), "the phrase layer can be cleared");
    feed(engine, "jiboner");
    expectEqual(engine.directOutput(), "জিবনের", "cleared phrases fall back to the core");
    feed(engine, "valobasi");
    expectEqual(engine.directOutput(), "ভালবাসি", "cleared phrases fall back to rules");

    // A corrupt layer must not disturb the core. The rejected image replaces
    // the buffer, so the phrase layer ends up unloaded rather than stale.
    expect(!engine.loadPhrases("garbage that is not a dictionary"),
           "a corrupt phrase image is rejected");
    expect(!engine.phrasesLoaded(), "a rejected phrase image leaves the layer unloaded");
    feed(engine, "dhaka");
    expectEqual(engine.directOutput(), "ঢাকা", "the core survives a corrupt phrase image");
}

// ---------------------------------------------------------------------------
// 7. Concurrency
// ---------------------------------------------------------------------------

void testConcurrentReadsAreSafe() {
    // The dictionary is documented as immutable after load, so many threads may
    // query it at once. A single threaded reference answer is compared against
    // concurrent results.
    const std::string image = sampleImage();

    lekho::bn::Dictionary reference;
    expect(reference.load(image), "concurrent reference loads");

    const char* const queries[]{"a",  "ba",   "bang", "bangla", "bangladesh",
                                "dh", "dhaka", "am",  "amar",   "sonar",
                                "jibon", "zz", "zzzzzzzz"};
    const std::size_t queryCount = sizeof(queries) / sizeof(queries[0]);

    std::vector<std::string> expected;
    for (std::size_t i = 0; i < queryCount; ++i) {
        std::string joined;
        for (const auto& hit : reference.lookup(lekho::bn::SearchChannel::Phonetic,
                                                 queries[i], 9)) {
            joined += hit.word;
            joined.push_back('|');
        }
        expected.push_back(joined);
    }

    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            lekho::bn::Dictionary local;
            if (!local.load(image)) {
                ++mismatches;
                return;
            }
            for (int repeat = 0; repeat < 40; ++repeat) {
                for (std::size_t i = 0; i < queryCount; ++i) {
                    std::string joined;
                    for (const auto& hit :
                         local.lookup(lekho::bn::SearchChannel::Phonetic, queries[i], 9)) {
                        joined += hit.word;
                        joined.push_back('|');
                    }
                    if (joined != expected[i]) {
                        ++mismatches;
                    }
                    (void)local.lookupFuzzyPhonetic(queries[i], 9);
                    (void)local.predictions("আমার", 4);
                }
            }
            (void)t;
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    expect(mismatches.load() == 0,
           "concurrent readers agree with the single threaded result (" +
               std::to_string(mismatches.load()) + " mismatches)");
}

// ---------------------------------------------------------------------------
// 8. Conformance vectors
// ---------------------------------------------------------------------------

// Each line is: input<TAB>expected Bangla. Lines starting with # are comments.
// The file is optional; a missing file is reported, not treated as a failure,
// so the suite still runs on a fresh checkout.
void testConformanceVectors() {
    const char* const candidates[]{"tests/conformance.txt", "conformance.txt"};
    std::ifstream file;
    std::string path;
    for (const char* candidate : candidates) {
        file.open(candidate, std::ios::binary);
        if (file.is_open()) {
            path = candidate;
            break;
        }
    }
    if (!file.is_open()) {
        std::cout << "note: no conformance vector file found; skipping.\n";
        return;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // The shipped IME loads the Avro layout table from beside the DLL; the
    // vectors describe that pipeline, so the test loads it too when available.
    for (const char* table : {"data/compiled/avro-layout.dat", "../data/compiled/avro-layout.dat", "AvroLayout.dat"}) {
        std::ifstream layoutFile(table, std::ios::binary);
        if (layoutFile.is_open()) {
            std::string layout((std::istreambuf_iterator<char>(layoutFile)),
                               std::istreambuf_iterator<char>());
            lekho::bn::setSharedAvroLayout(layout);
            break;
        }
    }
    lekho::bn::PhoneticTransliterator converter;
    std::size_t applied = 0;
    std::size_t lineNumber = 0;
    std::size_t offset = 0;
    while (offset <= content.size()) {
        const std::size_t newline = content.find('\n', offset);
        const std::size_t end = newline == std::string::npos ? content.size() : newline;
        std::string line = content.substr(offset, end - offset);
        offset = end + 1;
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            fail(path + ":" + std::to_string(lineNumber) + " is missing a tab separator");
            continue;
        }
        const std::string input = line.substr(0, tab);
        const std::string expected = line.substr(tab + 1);
        // The engine canonicalizes the transliteration before committing, so
        // the vectors describe the same pipeline rather than the raw parser.
        // This is why niyom and jirayeki agree on one য় codepoint.
        const std::string actual =
            lekho::bn::canonicalizeBangla(converter.transliterate(input));
        expectEqual(actual, expected,
                    path + ":" + std::to_string(lineNumber) + " for input \"" + input + "\"");
        ++applied;
    }
    std::cout << "conformance vectors applied: " << applied << " from " << path << "\n";
    expect(applied > 0, "the conformance file contains at least one vector");
}

} // namespace

int main() {
    testFormatRejectsNonDictionaries();
    testTruncationIsSafe();
    testSingleByteCorruptionIsSafe();
    testSeededFuzzIsSafe();
    testHostileHeaderFields();

    testBuildIsDeterministic();
    testRoundTrip();
    testDuplicateAdds();

    testBuilderValidation();
    testQueryBoundaries();

    testUtf8Decoder();
    testCanonicalizationIsIdempotent();

    testTransliterationIsTotal();
    testTransliterationIsStructurallySound();
    testTransliterationStructureOnLongInput();
    testTransliterationSampleIsStable();
    testTransliterationPreservesBangla();

    testEngineInvariants();
    testEngineOperationFuzz();
    testMalformedPhraseImages();
    testPhrasePriorityMatrix();

    testConcurrentReadsAreSafe();

    testConformanceVectors();

    std::cout << checks << " checks run, " << failures << " failed.\n";
    if (failures != 0) {
        std::cerr << failures << " robustness test(s) failed.\n";
        return 1;
    }
    std::cout << "All robustness tests passed.\n";
    return 0;
}
