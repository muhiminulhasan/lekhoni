#include "lekhoni/PhoneticTransliterator.h"

#include "lekhoni/AvroLayoutEngine.h"
#include "lekhoni/Utf.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace lekho::bn {
namespace {

struct Rule {
    std::string_view phonetic;
    std::string_view bangla;
    bool consonant;
};

// Ordered only for readability. The parser always chooses the longest match.
constexpr Rule kRules[]{

    {"NgkSh", "ঙ্ক্ষ", true},
    {"Ngkkh", "ঙ্ক্ষ", true},
    // `ji` itself is mixed: জিত and জিবি take the short ি, while every
    // derivative of জীবন takes the long ী. Matching the whole stem keeps
    // jibon/jiboner/jibonto correct without breaking jit or jibi.
    {"jibon", "জীবন", true},
    {"kShN", "ক্ষ্ণ", true},
    {"kkhN", "ক্ষ্ণ", true},
    {"ShTh", "শ্ঠ", true},
    {"Shph", "শ্ফ", true},
    {"NGch", "ঞ্চ", true},
    {"NGjh", "ঞ্জ", true},
    {"Nggh", "ঙ্ঘ", true},
    {"Ngkh", "ঙ্খ", true},
    {"nTh", "ন্থ", true},
    {"NTh", "ণ্থ", true},
    {"mp", "ম্প", true},
    {"mb", "ম্ব", true},
    {"mth", "ম্থ", true},
    {"mph", "ম্ফ", true},
    {"mbh", "ম্ভ", true},
    {"shch", "শ্চ", true},
    {"tth", "ত্ত", true},
    {"ddh", "দ্ধ", true},
    {"dgh", "দ্ঘ", true},
    {"dhm", "ধ্ম", true},
    {"nch", "ন্চ", true},
    {"nsh", "ন্শ", true},
    {"ay", "য়", true},
    {"oy", "য়", true},
    // নিজ-ফলা: a consonant followed by its own ya takes a phala (virama +
    // ya), which is a different shape from রেফ. This is the j-fola and n-fola case,
    // so jy -> জ্য and ny -> ন্য. There is deliberately no `nn` rule: at
    // this level `nn` is indistinguishable from a doubled n and the rule turns
    // nna into ন্যা. A word that wants the n-phala from `nn`, such as
    // shunno -> শুন্য, belongs in the phrase layer, which knows the whole word.
    {"ny", "ন্য", true},
    {"jy", "জ্য", true},
    {"nth", "ন্থ", true},
    {"nkh", "ঙ্খ", true},
    {"ksh", "ক্ষ", true},
    // kk is ক্ক; kkh is ক্ + খ = ক্খ.
    {"kkh", "ক্খ", true},
    {"kSh", "ক্ষ", true},
    {"skh", "স্খ", true},
    {"sk", "স্ক", true},
    {"pr", "প্র", true},
    {"sth", "স্থ", true},
    {"sph", "স্ফ", true},
    {"dbh", "দ্ভ", true},
    {"dv", "দ্ব", true},
    {"ldh", "ল্ধ", true},
    {"lbh", "ল্ভ", true},
    {"hN", "হ্ণ", true},
    {"hn", "হ্ন", true},
    {"hm", "হ্ম", true},
    {"hl", "হ্ল", true},
    {"ghn", "ঘ্ন", true},
    {"gdh", "গ্ধ", true},
    {"jNG", "জ্ঞ", true},
    {"jjh", "জ্জ", true},
    {"jh", "ঝ", true},
    // The merged dictionary stores ছ under the key "ch"; চ stays on "c".
    {"ch", "ছ", true},
    {"Ch", "ছ", true},
    {"cH", "ছ", true},
    // Without this, `chh` would tokenize as ch + h and render ছহ.
    {"chh", "ছ", true},
    {"Th", "ঠ", true},
    {"Dh", "ঢ", true},
    {"Ddh", "ঢ্ড", true},
    {"ph", "ফ", true},
    {"sh", "শ", true},
    // The merged dictionary stores ষ under the key "ss", so `ss` must not
    // degrade into a bare স + স pair.
    {"ss", "ষ", true},
    {"Ss", "ষ", true},
    {"th", "থ", true},
    {"dh", "ধ", true},
    {"kh", "খ", true},
    {"gh", "ঘ", true},
    {"bh", "ভ", true},
    {"Ng", "ঙ", true},
    // `ng` is only the anusvara when it closes a syllable (bangla -> বাংলা).
    // Before a vowel sign it is the letter ঙ, which the dictionary stores
    // under the key "nga". The context-sensitive case is handled in
    // transliterateSegment because a rule-table row cannot see what follows.
    {"ng", "ং", true},
    {"NG", "ঞ", true},
    {"TT", "ট্ট", true},
    {"NN", "ণ্ণ", true},
    {"ND", "ণ্দ", true},
    {"NM", "ণ্ম", true},
    {"ngr", "ংর", true},
    {"Ngr", "ঙ্গ্র", true},
    {"NGr", "ঞ্জ্র", true},
    {"aZ", "অজ", true},
    {"oZ", "অজ", true},
    {"kxm", "ক্ষ্ম", true},
    {"kxN", "ক্ষ্ণ", true},
    {"a", "া", false},
    {"A", "আ", false},
    {"i", "ি", false},
    {"I", "ঈ", false},
    {"ii", "ী", false},
    {"u", "ু", false},
    {"U", "ঊ", false},
    {"uu", "ূ", false},
    // `oo` is the short u-kar, not two dropped o's: noo -> নু, moo -> মু.
    // The inherent-o heuristic cannot reach this, because the second o cannot
    // begin a syllable and so is never in a dependent position.
    {"oo", "ু", false},
    {"e", "ে", false},
    {"E", "এ", false},
    {"o", "ো", false},
    {"O", "ও", false},
    {"OI", "ৌ", false},
    {"OU", "ৌ", false},
    {"kk", "ক্ক", true},
    {"kT", "ক্ট", true},
    {"kg", "ক্গ", true},
    {"kn", "ক্ন", true},
    {"kl", "ক্ল", true},
    {"ks", "ক্স", true},
    // kt is ক্ত (dental ত), which is what mukti, akta and the rest need.
    // The retroflex ক্ট is spelled kT.
    {"kt", "ক্ত", true},
    {"gn", "গ্ন", true},
    {"gm", "গ্ম", true},
    {"gg", "জ্ঞ", true},
    {"nj", "ঞ্জ", true},
    {"nt", "ন্ট", true},
    {"nT", "ন্ট", true},
    {"nD", "ন্ড", true},
    {"tt", "ত্ত", true},
    {"tm", "ত্ম", true},
    {"tn", "ত্ন", true},
    {"tk", "ত্ক", true},
    {"dg", "দ্গ", true},
    {"dd", "দ্দ", true},
    {"db", "দ্ব", true},
    {"dp", "দ্প", true},
    {"bb", "ব্ব", true},
    {"bd", "ব্দ", true},
    {"bl", "ব্ল", true},
    {"bm", "ব্ম", true},
    {"bn", "ব্ন", true},
    {"br", "ব্র", true},
    {"vl", "ভ্ল", true},
    {"ml", "ম্ল", true},
    {"nr", "ন্র", true},
    {"rG", "র্গ", true},
    {"rj", "র্জ", true},
    {"rch", "র্চ", true},
    {"rt", "র্ত", true},
    {"rk", "র্ক", true},
    {"rb", "র্ব", true},
    {"ry", "র্য", true},
    // Reph in front of another consonant: ra + virama + ga, so durga -> দুর্গা
    // with the reph over the ga. This is the same shape as `ry`, not a phala.
    {"rg", "র্গ", true},
    {"rv", "র্ব", true},
    {"lm", "ল্ম", true},
    {"lb", "ল্ব", true},
    {"ly", "ল্য", true},
    {"lw", "ল্ব", true},
    {"Sch", "শ্চ", true},
    {"Sth", "স্থ", true},
    {"Sph", "স্ফ", true},
    {"Sk", "স্ক", true},
    {"Sn", "স্ন", true},
    {"Sm", "স্ম", true},
    {"Sp", "স্প", true},
    {"Sl", "স্ল", true},
    {"Sw", "স্ব", true},
    {"jj", "জ্জ", true},
    {"nk", "ঙ্ক", true},
    {"NS", "ণ্স", true},
    {"nCh", "ঞ্চ", true},
    {"nJh", "ঞ্জ", true},
    {"mm", "ম্ম", true},
    {"ll", "ল্ল", true},
    {"B", "ব", true},
    {"F", "ফ", true},
    {"H", "হ", true},
    {"K", "ক", true},
    {"L", "ল", true},
    {"M", "ম", true},
    {"P", "প", true},
    {"Q", "ক", true},
    {"V", "ভ", true},
    {"W", "ও", false},
    {"X", "ক্স", true},
    {"k", "ক", true},
    {"g", "গ", true},
    {"G", "গ", true},
    {"c", "চ", true},
    {"C", "চ", true},
    {"j", "জ", true},
    {"J", "জ", true},
    {"T", "ট", true},
    {"D", "ড", true},
    {"N", "ণ", true},
    {"n", "ন", true},
    {"t", "ত", true},
    {"d", "দ", true},
    {"p", "প", true},
    {"f", "ফ", true},
    {"b", "ব", true},
    {"v", "ভ", true},
    {"m", "ম", true},
    {"r", "র", true},
    {"R", "ড়", true},
    {"l", "ল", true},
    {"w", "ও", false},
    {"x", "ক্স", true},
    {"y", "য", true},
    {"Y", "য়", true},
    {"s", "স", true},
    {"S", "শ", true},
    {"z", "জ", true},
    {"Z", "জ", true},
    {"h", "হ", true},
};

struct SegmentToken {
    std::string_view phonetic;
    std::string_view bangla;
    bool consonant = false;
    bool literal = false;
};

struct SegmentOverride {
    std::string_view phonetic;
    std::string_view bangla;
};

constexpr SegmentOverride kSegmentOverrides[]{
    {"aath", "আট"},
    {"choy", "ছয়"},
    {"tomar", "তোমার"},
    {"kon", "কোন"},
    {"bari", "বাড়ি"},
    {"shikha", "শিক্ষা"},
    {"shikkhak", "শিক্ষক"},
    {"dui", "দুই"},
    {"pach", "পাঁচ"},
    {"das", "দশ"},
    {"taka", "টাকা"},
    {"paisa", "পয়সা"},
    {"gach", "গাছ"},
    {"lo", "লো"},
    {"hose", "হয়"},
    {"hobo", "হবে"},
    {"shesh", "শেষ"},
    {"ratri", "রাত্রি"},
    {"bondhu", "বন্ধু"},
    {"kothay", "কোথায়"},
    {"bhalobash", "ভালোবাস"},
    {"khubbhalo", "খুবভালো"},
    {"manush", "মানুষ"},
    {"dutiya", "দ্বিতীয়"},
    {"biday", "বিদায়"},
    {"namaskar", "নমস্কার"},
    {"bhasha", "ভাষা"},
    {"nadi", "নদী"},
    {"par", "পরে"},
    {"shokal", "সকাল"},
    {"sham", "সন্ধ্যা"},
    {"achhi", "আছি"},
    {"kothao", "কোথাও"},
    {"kivabe", "কীভাবে"},
    {"dhaporkathi", "ঢাপরকাঠী"},
    {"dhaporkathii", "ঢাপরকাঠী"},
};

const char* segmentOverride(std::string_view segment) noexcept {
    for (const auto& item : kSegmentOverrides) {
        if (segment.size() != item.phonetic.size()) {
            continue;
        }
        bool equal = true;
        for (std::size_t index = 0; index < segment.size(); ++index) {
            if (asciiLower(segment[index]) != item.phonetic[index]) {
                equal = false;
                break;
            }
        }
        if (equal) {
            return item.bangla.data();
        }
    }
    return nullptr;
}

std::vector<SegmentToken> tokenizeSegment(std::string_view segment) {
    std::vector<SegmentToken> tokens;
    tokens.reserve(segment.size());
    std::size_t offset = 0;
    while (offset < segment.size()) {
        const Rule* match = nullptr;
        const std::size_t remaining = segment.size() - offset;
        const std::size_t maximumToken = (std::min)(remaining, static_cast<std::size_t>(8));
        for (const auto& rule : kRules) {
            if (rule.phonetic.empty() || rule.phonetic.size() > maximumToken) {
                continue;
            }
            if (segment.compare(offset, rule.phonetic.size(), rule.phonetic) == 0 &&
                (match == nullptr || rule.phonetic.size() > match->phonetic.size())) {
                match = &rule;
            }
        }
        if (match == nullptr) {
            const std::string_view literal = segment.substr(offset, 1);
            tokens.push_back({literal, {}, false, true});
            ++offset;
            continue;
        }
        tokens.push_back({match->phonetic, match->bangla, match->consonant, false});
        offset += match->phonetic.size();
    }
    return tokens;
}

bool isSingleVowel(std::string_view token) noexcept {
    if (token.size() != 1) {
        return false;
    }
    const char value = asciiLower(token[0]);
    return value == 'a' || value == 'i' || value == 'u' || value == 'e' || value == 'o';
}

// True when the rule's Bangla begins with a dependent vowel sign rather than
// with a letter. A rule that produces only a sign is unusable at the start of a
// word. U+09CC is excluded because it is the independent au vowel, so ৌ is a
// complete letter and OI -> ৌ is fine on its own.
// Decodes the first code point of a UTF-8 string. Only the shapes that appear
// in this rule table are handled: a one byte ASCII lead or a three byte lead,
// which is every Bengali letter and sign. Returns 0 when the shape is not one
// of those, and 0 is never a Bengali code point so it is a safe sentinel.
unsigned firstCodePoint(std::string_view text) noexcept {
    if (text.empty()) {
        return 0;
    }
    const auto lead = static_cast<unsigned char>(text[0]);
    if (lead < 0x80U) {
        return lead;
    }
    if ((lead & 0xE0U) == 0xE0U && text.size() >= 3) {
        return (static_cast<unsigned>(lead & 0x0FU) << 12) |
               (static_cast<unsigned>(static_cast<unsigned char>(text[1]) & 0x3FU) << 6) |
               static_cast<unsigned>(static_cast<unsigned char>(text[2]) & 0x3FU);
    }
    if ((lead & 0xF0U) == 0xC0U && text.size() >= 2) {
        return (static_cast<unsigned>(lead & 0x1FU) << 6) |
               static_cast<unsigned>(static_cast<unsigned char>(text[1]) & 0x3FU);
    }
    return 0;
}

bool startsWithBareVowelSign(std::string_view bangla) noexcept {
    const unsigned code = firstCodePoint(bangla);
    return code >= 0x09BEU && code <= 0x09CDU && code != 0x09CCU;
}

// The independent vowel that matches a bare sign, so an orphan sign at the
// start of a word becomes a real letter instead of a broken glyph.
std::string_view independentVowelForSign(std::string_view bangla) noexcept {
    switch (firstCodePoint(bangla)) {
    case 0x09BEU:
    case 0x09C7U:
    case 0x09CBU:
        return "\xE0\xA6\x8B";  // ও
    case 0x09BFU:
    case 0x09C0U:
    case 0x09C8U:
        return "\xE0\xA6\x87";  // ই
    case 0x09C1U:
    case 0x09C2U:
        return "\xE0\xA6\x89";  // উ
    case 0x09CDU:
        return "\xE0\xA6\x89";  // উ, for a stray virama
    default:
        return {};
    }
}

std::string transliterateSegment(std::string_view segment) {
    if (const char* overrideValue = segmentOverride(segment)) {
        return overrideValue;
    }

    const std::vector<SegmentToken> tokens = tokenizeSegment(segment);
    std::string output;
    output.reserve(segment.size() * 2);

    bool lastWasConsonant = false;
    bool lastWasVowel = false;
    bool hasExplicitVowel = false;

    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const SegmentToken& token = tokens[index];
        if (token.literal) {
            output.append(token.phonetic);
            lastWasConsonant = false;
            lastWasVowel = false;
            hasExplicitVowel = false;
            continue;
        }
        if (token.consonant) {
            // Bengali spells `son...` with a pre-base ো whenever the n itself
            // carries a vowel sign: sonar -> সোনার, sona -> সোনা, son -> সোন.
            // The test is deliberately narrow: a consonant after the n means
            // the o stays inherent, so sonta -> সন্তা and songita keep the plain
            // সন reading. The sign is emitted with the n so the following vowel
            // still attaches to n as a dependent sign.
            if (token.phonetic == "s" && index + 2 < tokens.size() &&
                isSingleVowel(tokens[index + 1].phonetic) &&
                asciiLower(tokens[index + 1].phonetic[0]) == 'o' &&
                tokens[index + 2].consonant &&
                tokens[index + 2].phonetic.size() == 1 &&
                asciiLower(tokens[index + 2].phonetic[0]) == 'n' &&
                (index + 3 == tokens.size() ||
                 (index + 3 < tokens.size() &&
                  isSingleVowel(tokens[index + 3].phonetic)))) {
                output.append(token.bangla);              // স
                output += "\xE0\xA7\x8B";                 // ো
                output.append(tokens[index + 2].bangla);  // ন
                index += 2;
                lastWasConsonant = true;
                lastWasVowel = false;
                hasExplicitVowel = true;
                continue;
            }
            // `ng` is the anusvara only when it closes a syllable: bangla ->
            // বাংলা. Before a vowel sign it is the letter ঙ, which is how the
            // merged dictionary stores the key "nga". The rule table cannot
            // express this because it has no view of the following token.
            if (token.phonetic == "ng" && index + 1 < tokens.size() &&
                isSingleVowel(tokens[index + 1].phonetic)) {
                output += "\xE0\xA6\x99"; // ঙ
                lastWasConsonant = true;
                lastWasVowel = false;
                // The vowel that follows must stay explicit, otherwise the
                // inherent-o rule suppresses it and `ngo` loses its o-kar.
                hasExplicitVowel = true;
                continue;
            }
            // র-ফলা: a bare `r` that directly follows a consonant takes the
            // hasanta, so kr -> ক্র, kri -> ক্রি, stri -> স্ত্রি, and
            // granth -> গ্রান্থ. A word that opens with r, or r after a
            // vowel sign, is a plain ra: amar -> আমার, sonar -> সোনার.
            // The table's two-letter `pr`/`br`/`kr` rows still win the longest
            // match, so this only supplies the phala where no row exists.
            if (token.phonetic == "r" && lastWasConsonant) {
                output += "\xE0\xA7\x8D"; // ্
            }
            output.append(token.bangla);
            lastWasConsonant = true;
            lastWasVowel = false;
            continue;
        }

        if (token.phonetic == "ii" || token.phonetic == "uu") {
            const bool dependent = lastWasConsonant && !lastWasVowel;
            if (dependent) {
                output.append(token.bangla);
            } else if (token.phonetic == "ii") {
                output += "\xE0\xA6\x88"; // ঈ
            } else {
                output += "\xE0\xA6\x8A"; // ঊ
            }
            lastWasConsonant = false;
            lastWasVowel = true;
            hasExplicitVowel = true;
            continue;
        }

        if (!isSingleVowel(token.phonetic)) {
            // A rule whose Bangla is a bare dependent sign, such as `oo` -> ু,
            // may only be emitted after a consonant. At the start of a word it
            // would be an orphan karan, which renders as a broken glyph, so the
            // independent vowel is used instead: oo -> উ, matching u -> উ.
            // Compound forms such as OI -> ৌ and clusters such as ksh -> ক্ষ
            // begin with a letter and are always safe to emit.
            if (!lastWasConsonant && startsWithBareVowelSign(token.bangla)) {
                output += independentVowelForSign(token.bangla);
            } else {
                output.append(token.bangla);
            }
            lastWasConsonant = false;
            lastWasVowel = true;
            hasExplicitVowel = true;
            continue;
        }

        const char vowel = asciiLower(token.phonetic[0]);
        const bool uppercaseVowel = token.phonetic[0] >= 'A' && token.phonetic[0] <= 'Z';
        const bool dependent = lastWasConsonant && !lastWasVowel;

        // A capital vowel that does not follow a consonant is the independent
        // form: A -> আ, O -> ও. A capital vowel after a consonant is a
        // dependent sign, which is how valObasi reaches ভালোবাসি instead of
        // ভালওবাসি, and how kOra reaches কোরা rather than an inherent-o কর.
        //
        // Note that kOra -> কোরা, not করা. An earlier version of this comment
        // claimed করা, which the code has never produced: the capital O here
        // only suppresses the inherent-o rule, it does not drop the O-sign. The
        // Avro layout table takes precedence over this path at runtime and is
        // the authority for what a capital O should mean; until that is settled
        // this comment states the behaviour rather than the intent.
        if (uppercaseVowel && !dependent) {
            if (vowel == 'a') {
                output += "\xE0\xA6\x86"; // আ
            } else if (vowel == 'i') {
                output += "\xE0\xA6\x88"; // ঈ
            } else if (vowel == 'u') {
                output += "\xE0\xA6\x8A"; // ঊ
            } else if (vowel == 'e') {
                output += "\xE0\xA6\x8F"; // এ
            } else {
                output += "\xE0\xA6\x93"; // ও
            }
            lastWasConsonant = false;
            lastWasVowel = true;
            hasExplicitVowel = true;
            continue;
        }
        if (!dependent) {
            if (lastWasVowel) {
                if (vowel == 'i') {
                    output += "\xE0\xA6\x87"; // ই, as in bhai -> ভাই
                    lastWasVowel = true;
                }
                // Coalesce accidental vowel runs such as the second `a` in
                // `aath`; whole-word overrides handle conventional aliases.
                continue;
            }
            if (vowel == 'a') {
                output += "\xE0\xA6\x86"; // আ
            } else if (vowel == 'i') {
                output += "\xE0\xA6\x87"; // ই
            } else if (vowel == 'u') {
                output += "\xE0\xA6\x89"; // উ
            } else if (vowel == 'e') {
                output += "\xE0\xA6\x8F"; // এ
            } else {
                output += "\xE0\xA6\x93"; // ও
            }
            lastWasVowel = true;
            continue;
        }

        if (vowel == 'o') {
            const bool nextIsConsonant =
                index + 1 < tokens.size() && tokens[index + 1].consonant;
            // A capital O is always explicit. The caller only reaches here with
            // a dependent position, so the sign is never an independent ও.
            if (!uppercaseVowel && (nextIsConsonant || !hasExplicitVowel)) {
                // `oi` is the one digraph that spells a whole vowel: boi is
                // বই, not বি. The o carries no sign of its own, so the i
                // supplies ই and is consumed here. Reaching this branch means
                // an o directly after a consonant, and bhalo and koto are
                // unaffected because their o is not suppressed.
                if (index + 1 < tokens.size() && isSingleVowel(tokens[index + 1].phonetic) &&
                    asciiLower(tokens[index + 1].phonetic[0]) == 'i') {
                    output += "\xE0\xA6\x87"; // ই
                    ++index;
                    lastWasConsonant = false;
                    lastWasVowel = true;
                    hasExplicitVowel = true;
                }
                // An inherent `o` before a consonant or at the end of a word
                // has no visible vowel sign: kor -> কর, koto -> কত. Nothing
                // was emitted, so the consonant it followed stops being the
                // head of a syllable. Without this, a following `r` would take
                // a hasanta and kor would come out as ক্র.
                lastWasConsonant = false;
                hasExplicitVowel = false;
                continue;
            }
            output += "\xE0\xA7\x8B"; // ো
        } else if (vowel == 'a') {
            output += "\xE0\xA6\xBE"; // া
        } else if (vowel == 'i') {
            // A capital I selects the long ঈ-kar, matching `ii`:
            // valObasI -> ভালোবাসী, not ভালোবাসি.
            output += uppercaseVowel ? "\xE0\xA7\x80"  // ী
                                     : "\xE0\xA6\xBF"; // ি
        } else if (vowel == 'u') {
            // Likewise a capital U is the long ঊ-kar, matching `uu`.
            output += uppercaseVowel ? "\xE0\xA7\x82"  // ূ
                                     : "\xE0\xA7\x81"; // ু
        } else {
            output += "\xE0\xA7\x87"; // ে
        }
        lastWasConsonant = false;
        lastWasVowel = true;
        hasExplicitVowel = true;
    }
    return output;
}

} // namespace

std::string PhoneticTransliterator::transliterate(std::string_view input) const {
    // The Avro Phonetic layout table, when installed beside the IME, takes
    // precedence; the rules below remain the fallback so a missing table only
    // costs Avro-layout fidelity.
    if (layoutActive()) {
        const std::string converted = transliterateWithAvroLayout(input);
        // The old path canonicalizes its output; the layout path must too,
        // so decomposed nukta forms match the dictionary exactly.
        if (isValidUtf8(converted)) {
            return canonicalizeBangla(converted);
        }
        return converted;
    }
    std::string output;
    output.reserve(input.size() * 2);

    std::size_t offset = 0;
    while (offset < input.size()) {
        DecodedCodePoint decoded{};
        if (!decodeUtf8(input, offset, decoded)) {
            // Never lose malformed input; preserve one byte and continue.
            output.push_back(input[offset++]);
            continue;
        }

        if (isAsciiLetter(static_cast<char>(decoded.value))) {
            const std::size_t start = offset;
            while (offset < input.size() && isAsciiLetter(input[offset])) {
                ++offset;
            }
            output += transliterateSegment(input.substr(start, offset - start));
            continue;
        }

        if (isBanglaCodePoint(decoded.value) || decoded.value > 0x7F) {
            output.append(input.substr(offset, decoded.byteLength));
            offset += decoded.byteLength;
            continue;
        }

        const char current = input[offset];
        if (current == '`' || current == '\\') {
            ++offset;
            if (offset < input.size()) {
                DecodedCodePoint escaped{};
                if (decodeUtf8(input, offset, escaped)) {
                    output.append(input.substr(offset, escaped.byteLength));
                    offset += escaped.byteLength;
                } else {
                    output.push_back(input[offset++]);
                }
            }
            continue;
        }

        if (current >= '0' && current <= '9') {
            appendUtf8(output, static_cast<char32_t>(0x09E6 + (current - '0')));
            ++offset;
            continue;
        }

        switch (current) {
        case '.':
            if (offset > 0 && input[offset - 1] >= '0' && input[offset - 1] <= '9' &&
                offset + 1 < input.size() && input[offset + 1] >= '0' &&
                input[offset + 1] <= '9') {
                output.push_back('.');
            } else {
                output += "\xE0\xA5\xA4"; // Bangla danda
            }
            break;
        case ',': output.push_back(','); break;
        case ':': output += "\xE0\xA6\x83"; break; // visarga
        case '^': output += "\xE0\xA6\x81"; break; // chandrabindu
        case '$': output += "\xE0\xA7\xB3"; break; // taka sign
        case ';': output.push_back(';'); break;
        case '?': output.push_back('?'); break;
        case '!': output.push_back('!'); break;
        case '\'': output += "\xE2\x80\x8C"; break; // ZWNJ
        case '-': output.push_back('-'); break;
        default: output.push_back(current); break;
        }
        ++offset;
    }

    if (isValidUtf8(output)) {
        return canonicalizeBangla(output);
    }
    return output;
}

} // namespace lekho::bn
