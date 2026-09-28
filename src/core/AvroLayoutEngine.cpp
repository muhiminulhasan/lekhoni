#include "lekhoni/AvroLayoutEngine.h"

#include "lekhoni/Utf.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace lekho::bn {

namespace {

constexpr std::uint32_t kMagic = 0x544C4742U;  // 'BGLT'
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kMaximumInputCodePoints = 65536;

std::uint16_t readU16(const std::string& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset])) |
           static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset + 1]) << 8U);
}

std::uint32_t readU32(const std::string& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (i * 8U);
    }
    return value;
}

std::uint64_t readU64(const std::string& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (i * 8U);
    }
    return value;
}

std::uint64_t fnv1a64(std::string_view bytes) {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (const char value : bytes) {
        hash ^= static_cast<unsigned char>(value);
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

bool inSet(const std::u32string& set, char32_t value) {
    return set.find(value) != std::u32string::npos;
}

bool takeString(const std::string& bytes, std::size_t& offset, std::u32string& out) {
    if (offset + 2 > bytes.size()) {
        return false;
    }
    const std::uint16_t length = readU16(bytes, offset);
    offset += 2;
    if (offset + length > bytes.size()) {
        return false;
    }
    const std::string raw = bytes.substr(offset, length);
    offset += length;
    std::size_t position = 0;
    while (position < raw.size()) {
        DecodedCodePoint decoded;
        if (!decodeUtf8(raw, position, decoded)) {
            return false;
        }
        out.push_back(decoded.value);
        position += decoded.byteLength;
    }
    return true;
}

} // namespace

// Table layout documentation lives in tools/compile_avro_layout.py.
struct AvroLayoutTable {
    enum class Scope : std::uint8_t { Vowel = 0, Consonant = 1, Punctuation = 2, Number = 3, Exact = 4 };
    enum class MatchType : std::uint8_t { Prefix = 0, Suffix = 1 };

    struct Match {
        MatchType type = MatchType::Prefix;
        bool negated = false;
        Scope scope = Scope::Vowel;
        char32_t value = 0;
    };

    struct Rule {
        std::u32string replace;
        std::vector<Match> matches;
    };

    struct Pattern {
        std::u32string find;
        std::u32string replace;
        std::vector<Rule> rules;
    };

    std::u32string caseSet;      // lowercase letters matched case-sensitively
    std::u32string vowelSet;     // aeiou
    std::u32string consonantSet; // bcdfghjklmnpqrstvwxyz
    std::u32string numberSet;    // 1234567890
    std::vector<Pattern> patterns;
};

namespace {

using Table = AvroLayoutTable;
using Match = Table::Match;
using Rule = Table::Rule;
using Pattern = Table::Pattern;
using Scope = Table::Scope;
using MatchType = Table::MatchType;

std::shared_ptr<const Table> parseTable(const std::string& bytes) {
    if (bytes.size() < 20 || readU32(bytes, 0) != kMagic || readU16(bytes, 4) != kVersion) {
        return {};
    }
    const std::uint32_t count = readU32(bytes, 8);
    if (count == 0 || count > 20000) {
        return {};
    }
    const std::uint64_t storedHash = readU64(bytes, bytes.size() - 8);
    if (storedHash != fnv1a64(std::string_view(bytes).substr(0, bytes.size() - 8))) {
        return {};
    }

    auto table = std::make_shared<Table>();
    std::size_t offset = 12;
    for (auto* set : {&table->caseSet, &table->vowelSet, &table->consonantSet, &table->numberSet}) {
        if (offset >= bytes.size()) {
            return {};
        }
        const std::uint8_t length = static_cast<unsigned char>(bytes[offset]);
        ++offset;
        if (offset + length > bytes.size()) {
            return {};
        }
        const std::string raw = bytes.substr(offset, length);
        offset += length;
        std::size_t position = 0;
        while (position < raw.size()) {
            DecodedCodePoint decoded;
            if (!decodeUtf8(raw, position, decoded)) {
                return {};
            }
            set->push_back(decoded.value);
            position += decoded.byteLength;
        }
    }

    for (std::uint32_t index = 0; index < count; ++index) {
        Pattern pattern;
        if (!takeString(bytes, offset, pattern.find) ||
            !takeString(bytes, offset, pattern.replace)) {
            return {};
        }
        if (offset + 2 > bytes.size()) {
            return {};
        }
        const std::uint16_t ruleCount = readU16(bytes, offset);
        offset += 2;
        if (ruleCount > 64) {
            return {};
        }
        for (std::uint16_t rule = 0; rule < ruleCount; ++rule) {
            Rule parsed;
            if (!takeString(bytes, offset, parsed.replace)) {
                return {};
            }
            if (offset + 2 > bytes.size()) {
                return {};
            }
            const std::uint16_t matchCount = readU16(bytes, offset);
            offset += 2;
            if (matchCount > 16) {
                return {};
            }
            for (std::uint16_t match = 0; match < matchCount; ++match) {
                if (offset + 4 > bytes.size()) {
                    return {};
                }
                Match parsedMatch;
                parsedMatch.type = static_cast<MatchType>(bytes[offset]);
                parsedMatch.negated = bytes[offset + 1] != 0;
                parsedMatch.scope = static_cast<Scope>(bytes[offset + 2]);
                const std::uint8_t valueLength = static_cast<unsigned char>(bytes[offset + 3]);
                offset += 4;
                if (parsedMatch.scope == Scope::Exact && valueLength == 0) {
                    return {};
                }
                if (offset + valueLength > bytes.size()) {
                    return {};
                }
                if (valueLength != 0) {
                    const std::string raw = bytes.substr(offset, valueLength);
                    offset += valueLength;
                    DecodedCodePoint decoded;
                    std::size_t position = 0;
                    if (!decodeUtf8(raw, position, decoded) || decoded.byteLength != raw.size()) {
                        return {};
                    }
                    parsedMatch.value = decoded.value;
                }
                parsed.matches.push_back(parsedMatch);
            }
            pattern.rules.push_back(std::move(parsed));
        }
        table->patterns.push_back(std::move(pattern));
    }
    if (offset != bytes.size() - 8) {
        return {};
    }
    return table;
}

// Classifies one input code point for rule scopes.
bool scopeHolds(const Table& table, const Match& match, char32_t context) {
    bool holds = false;
    if (match.scope == Scope::Exact) {
        holds = context == match.value;
    } else {
        const char32_t lower =
            (context >= U'A' && context <= U'Z') ? context + 0x20U : context;
        switch (match.scope) {
        case Scope::Vowel: holds = inSet(table.vowelSet, lower); break;
        case Scope::Consonant: holds = inSet(table.consonantSet, lower); break;
        case Scope::Number: holds = inSet(table.numberSet, context); break;
        case Scope::Punctuation:
            holds = !inSet(table.vowelSet, lower) && !inSet(table.consonantSet, lower) &&
                    !inSet(table.numberSet, context);
            break;
        case Scope::Exact: break;
        }
    }
    return match.negated ? !holds : holds;
}

bool ruleHolds(const Table& table, const Rule& rule, const std::vector<char32_t>& input,
               std::size_t position, std::size_t length) {
    for (const Match& match : rule.matches) {
        char32_t context = 0;
        if (match.type == MatchType::Prefix) {
            // An empty prefix (word start) behaves like a punctuation
            // character; the synthetic code point below classifies as one.
            context = position > 0 ? input[position - 1] : U'\x01';
        } else {
            const std::size_t after = position + length;
            // An empty suffix (word end) behaves the same way.
            context = after < input.size() ? input[after] : U'\x01';
        }
        if (!scopeHolds(table, match, context)) {
            return false;
        }
    }
    return true;
}

bool matchesFind(const Table& table, const Pattern& pattern,
                 const std::vector<char32_t>& input, std::size_t position) {
    if (position + pattern.find.size() > input.size()) {
        return false;
    }
    for (std::size_t i = 0; i < pattern.find.size(); ++i) {
        const char32_t expected = pattern.find[i];
        const char32_t actual = input[position + i];
        if (expected == actual) {
            continue;
        }
        const bool caseInsensitive =
            expected >= U'a' && expected <= U'z' && !inSet(table.caseSet, expected);
        if (caseInsensitive && (actual | 0x20U) == expected) {
            continue;
        }
        return false;
    }
    return true;
}

void appendUtf32AsUtf8(std::string& out, const std::u32string& text) {
    for (const char32_t value : text) {
        appendUtf8(out, value);
    }
}

} // namespace

bool AvroLayoutEngine::load(std::string bytes) {
    auto parsed = parseTable(bytes);
    if (!parsed) {
        return false;
    }
    table_ = std::move(parsed);
    return true;
}

std::string AvroLayoutEngine::transliterate(std::string_view input) const {
    if (!table_) {
        return {};
    }
    const Table& table = *table_;

    // Decode the input once; bounded so a hostile document cannot stall the
    // keystroke path.
    std::vector<char32_t> characters;
    characters.reserve((std::min)(input.size(), kMaximumInputCodePoints));
    std::size_t offset = 0;
    while (offset < input.size() && characters.size() < kMaximumInputCodePoints) {
        DecodedCodePoint decoded;
        if (!decodeUtf8(input, offset, decoded)) {
            // Preserve malformed input by carrying it through untouched; the
            // pattern walk cannot match it anyway.
            characters.push_back(static_cast<char32_t>(0x110000U + (offset & 0xFFU)));
            ++offset;
            continue;
        }
        characters.push_back(decoded.value);
        offset += decoded.byteLength;
    }

    std::string output;
    output.reserve(input.size() * 2);
    std::size_t position = 0;
    bool escapeNext = false;
    while (position < characters.size()) {
        const char32_t current = characters[position];

        // Lekhoni keeps the backslash literal escape; the backtick belongs to
        // the layout itself (cluster breaker and sign lock).
        if (escapeNext) {
            appendUtf8(output, current);
            escapeNext = false;
            ++position;
            continue;
        }
        if (current == U'\\') {
            escapeNext = true;
            ++position;
            continue;
        }

        const Pattern* matched = nullptr;
        for (const Pattern& pattern : table.patterns) {
            if (matchesFind(table, pattern, characters, position)) {
                matched = &pattern;
                break;
            }
        }

        if (matched != nullptr) {
            const std::u32string* replacement = &matched->replace;
            for (const Rule& rule : matched->rules) {
                if (ruleHolds(table, rule, characters, position, matched->find.size())) {
                    replacement = &rule.replace;
                    break;
                }
            }
            appendUtf32AsUtf8(output, *replacement);
            position += matched->find.size();
            continue;
        }

        // No layout pattern: keep Lekhoni's local behaviour for digits and
        // the punctuation the layout does not model, and pass everything
        // else (including Bangla input) through untouched. Sentinels above
        // U+10FFFF from malformed input are emitted as their original bytes.
        if (current > 0x10FFFFU) {
            output.push_back(static_cast<char>(current & 0xFFU));
        } else if (current >= U'0' && current <= U'9') {
            appendUtf8(output, static_cast<char32_t>(0x09E6 + (current - U'0')));
        } else if (current == U'$') {
            output += "\xE0\xA7\xB3";  // taka sign
        } else if (current == U'\'') {
            output += "\xE2\x80\x8C";  // ZWNJ
        } else {
            appendUtf8(output, current);
        }
        ++position;
    }
    return output;
}

namespace {
std::mutex g_layoutMutex;
std::shared_ptr<const AvroLayoutEngine> g_layout;
} // namespace

void setSharedAvroLayout(std::string bytes) {
    auto engine = std::make_shared<AvroLayoutEngine>();
    if (engine->load(std::move(bytes))) {
        std::lock_guard<std::mutex> guard(g_layoutMutex);
        g_layout = std::move(engine);
    }
}

bool layoutActive() noexcept {
    std::lock_guard<std::mutex> guard(g_layoutMutex);
    return g_layout != nullptr;
}

std::string transliterateWithAvroLayout(std::string_view input) {
    std::shared_ptr<const AvroLayoutEngine> engine;
    {
        std::lock_guard<std::mutex> guard(g_layoutMutex);
        engine = g_layout;
    }
    if (!engine) {
        return {};
    }
    return engine->transliterate(input);
}

} // namespace lekho::bn
