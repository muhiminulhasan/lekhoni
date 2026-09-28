#include "lekhoni/Dictionary.h"

#include "lekhoni/Utf.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lekho::bn {
namespace {

constexpr std::size_t kHeaderBytes = 96;
constexpr std::size_t kSectionEntryBytes = 40;
constexpr std::size_t kMetaBytes = 80;
constexpr std::size_t kFormBytes = 32;
constexpr std::size_t kWordBytes = 32;
constexpr std::size_t kMaximumFileBytes = 128U * 1024U * 1024U;
constexpr std::size_t kMaximumBlobBytes = 64U * 1024U * 1024U;
constexpr std::uint32_t kMaximumWords = 1'000'000;
constexpr std::uint32_t kMaximumForms = 2'000'000;
constexpr std::uint32_t kMaximumKeyBytes = 4096;
constexpr std::uint32_t kMaximumDisplayBytes = 1024U * 1024U;
constexpr std::uint16_t kRequiredFeatures = 0;

constexpr std::uint32_t fourCc(char a, char b, char c, char d) noexcept {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24U);
}

constexpr std::uint32_t kSectionMeta = fourCc('B', 'G', 'M', '1');
constexpr std::uint32_t kSectionForm = fourCc('F', 'O', 'R', 'M');
constexpr std::uint32_t kSectionWord = fourCc('W', 'O', 'R', 'D');
constexpr std::uint32_t kSectionBlob = fourCc('B', 'L', 'O', 'B');

void writeU16(std::string& output, std::size_t offset, std::uint16_t value) {
    output[offset] = static_cast<char>(value & 0xFFU);
    output[offset + 1] = static_cast<char>((value >> 8U) & 0xFFU);
}

void writeU32(std::string& output, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        output[offset + index] = static_cast<char>((value >> (index * 8U)) & 0xFFU);
    }
}

void writeU64(std::string& output, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        output[offset + index] = static_cast<char>((value >> (index * 8U)) & 0xFFU);
    }
}

std::uint16_t readU16(std::string_view bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset])) |
           static_cast<std::uint16_t>(
               static_cast<unsigned char>(bytes[offset + 1]) << 8U);
}

std::uint32_t readU32(std::string_view bytes, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

std::uint64_t readU64(std::string_view bytes, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

std::uint32_t crc32(std::string_view bytes,
                    std::size_t zeroFrom = 0,
                    std::size_t zeroLength = 0) noexcept {
    constexpr std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0; index < values.size(); ++index) {
            std::uint32_t value = index;
            for (std::uint32_t bit = 0; bit < 8; ++bit) {
                value = (value >> 1U) ^ ((value & 1U) != 0U ? 0xEDB88320U : 0U);
            }
            values[index] = value;
        }
        return values;
    }();

    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto byte = static_cast<unsigned char>(bytes[index]);
        const bool insideZeroRange = zeroLength != 0 && index >= zeroFrom &&
                                     index - zeroFrom < zeroLength;
        crc = table[(crc ^ (insideZeroRange ? 0U : byte)) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

std::uint64_t fnv1a64(std::string_view bytes, std::uint64_t seed) noexcept {
    std::uint64_t hash = seed;
    for (const char value : bytes) {
        hash ^= static_cast<unsigned char>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void appendU16(std::string& output, std::uint16_t value) {
    output.push_back(static_cast<char>(value & 0xFFU));
    output.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void appendU32(std::string& output, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        output.push_back(static_cast<char>((value >> (index * 8U)) & 0xFFU));
    }
}

void appendU64(std::string& output, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        output.push_back(static_cast<char>((value >> (index * 8U)) & 0xFFU));
    }
}

void padToEight(std::string& output) {
    while ((output.size() & 7U) != 0U) {
        output.push_back('\0');
    }
}

bool addRange(std::uint64_t offset, std::uint64_t length, std::uint64_t limit) noexcept {
    return offset <= limit && length <= limit - offset;
}

bool hasNul(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

// Matching order for phonetic keys: case-insensitive (folded), with raw
// bytes as the tiebreak so mixed-case keys keep a stable position.
std::string foldKey(std::string_view key) {
    return asciiLower(key);
}

bool keyBefore(std::string_view left, std::string_view right) {
    const std::string lf = foldKey(left);
    const std::string rf = foldKey(right);
    if (lf != rf) {
        return lf < rf;
    }
    return left < right;
}

bool hasEdgeAsciiWhitespace(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    const auto isSpace = [](char item) {
        return item == ' ' || item == '\t' || item == '\r' || item == '\n';
    };
    return isSpace(value.front()) || isSpace(value.back());
}

bool isPhoneticKey(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    for (const char item : value) {
        if (!isAsciiLetter(item) && item != '\'' && item != '-') {
            return false;
        }
    }
    return true;
}

// Bounded optimal-string-alignment distance. The transposition case covers
// common phonetic typos such as "bangaldesh" -> "bangladesh" without pulling
// in a third-party approximate-matching library.
//
// The rows live in fixed stack storage rather than three std::vector<int>s.
// This runs on the keystroke path, once per candidate form sharing the query's
// first letter, which is up to ~19k calls per keystroke in the shipped corpus;
// heap-allocating the three rows each time cost roughly 70 million allocations
// across a full evaluation run. The bound is safe because lookupFuzzyPhonetic
// rejects anything longer than 64 bytes and pre-filters on length difference,
// so neither operand can exceed kEditDistanceColumns.
constexpr std::size_t kEditDistanceColumns = 72;
using EditRow = std::array<int, kEditDistanceColumns>;

int editDistance(std::string_view left, std::string_view right, int maximum) noexcept {
    if (maximum < 0) {
        return maximum + 1;
    }
    if (left.size() > right.size()) {
        std::swap(left, right);
    }
    if (right.size() >= kEditDistanceColumns) {
        return maximum + 1;
    }

    const int rows = static_cast<int>(left.size());
    const int columns = static_cast<int>(right.size());
    EditRow previousPrevious{};
    EditRow previous{};
    EditRow current{};

    for (int column = 0; column <= columns; ++column) {
        previous[static_cast<std::size_t>(column)] = column;
    }
    if (rows == 0) {
        return columns <= maximum ? columns : maximum + 1;
    }

    for (int row = 1; row <= rows; ++row) {
        current[0] = row;
        int rowMinimum = current[0];
        for (int column = 1; column <= columns; ++column) {
            const int substitution = previous[static_cast<std::size_t>(column) - 1] +
                                     (left[static_cast<std::size_t>(row) - 1] ==
                                              right[static_cast<std::size_t>(column) - 1]
                                          ? 0
                                          : 1);
            const int deletion = previous[static_cast<std::size_t>(column)] + 1;
            const int insertion = current[static_cast<std::size_t>(column) - 1] + 1;
            int value = (std::min)(substitution, (std::min)(deletion, insertion));
            if (row > 1 && column > 1 &&
                left[static_cast<std::size_t>(row) - 1] ==
                    right[static_cast<std::size_t>(column) - 2] &&
                left[static_cast<std::size_t>(row) - 2] ==
                    right[static_cast<std::size_t>(column) - 1]) {
                value = (std::min)(value,
                                   previousPrevious[static_cast<std::size_t>(column) - 2] + 1);
            }
            current[static_cast<std::size_t>(column)] = value;
            rowMinimum = (std::min)(rowMinimum, value);
        }
        if (rowMinimum > maximum) {
            return maximum + 1;
        }
        previousPrevious.swap(previous);
        previous.swap(current);
    }

    const int result = previous[static_cast<std::size_t>(columns)];
    return result <= maximum ? result : maximum + 1;
}

bool isKnownSection(std::uint32_t id) noexcept {
    return id == kSectionMeta || id == kSectionForm || id == kSectionWord || id == kSectionBlob;
}

std::size_t toSize(std::uint64_t value, std::size_t maximum) noexcept {
    if (value > maximum || value > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return (std::numeric_limits<std::size_t>::max)();
    }
    return static_cast<std::size_t>(value);
}

} // namespace

bool DictionaryBuilder::add(std::string_view phonetic,
                             std::string_view bangla,
                             std::uint32_t score,
                             std::string_view hint) {
    if (phonetic.empty() || bangla.empty() || hasNul(phonetic) || hasNul(bangla) ||
        hasNul(hint) || hasEdgeAsciiWhitespace(phonetic) || hasEdgeAsciiWhitespace(bangla) ||
        hasEdgeAsciiWhitespace(hint) || !isValidUtf8(phonetic) || !isValidUtf8(bangla) ||
        !isValidUtf8(hint) || !isPhoneticKey(phonetic) ||
        phonetic.size() > kMaximumKeyBytes || bangla.size() > kMaximumDisplayBytes) {
        return false;
    }

    const std::string canonicalBangla = canonicalizeBangla(bangla);
    const std::string canonicalHint = containsBangla(hint) ? canonicalizeBangla(hint)
                                                             : std::string(hint);
    if (canonicalBangla.empty() || (containsBangla(hint) && canonicalHint.empty()) ||
        !containsBangla(canonicalBangla)) {
        return false;
    }

    const std::string normalizedPhonetic(phonetic);
    const auto existing = wordIds_.find(canonicalBangla);
    const bool isNewWord = existing == wordIds_.end();
    const std::size_t resultingForms = forms_.size() + words_.size() + 1 + (isNewWord ? 1U : 0U);
    if (resultingForms > kMaximumForms || words_.size() >= kMaximumWords) {
        return false;
    }

    std::uint32_t wordId = 0;
    if (isNewWord) {
        wordId = static_cast<std::uint32_t>(words_.size());
        words_.push_back({canonicalBangla, canonicalHint});
        wordIds_.emplace(canonicalBangla, wordId);
    } else {
        wordId = existing->second;
        if (words_[wordId].hint.empty() && !canonicalHint.empty()) {
            words_[wordId].hint = canonicalHint;
        }
    }

    forms_.push_back({normalizedPhonetic, wordId, score, SearchChannel::Phonetic});
    return true;
}

std::size_t DictionaryBuilder::wordCount() const noexcept {
    return words_.size();
}

std::size_t DictionaryBuilder::formCount() const noexcept {
    return forms_.size() + words_.size();
}

std::string DictionaryBuilder::build() const {
    if (words_.empty() || words_.size() > kMaximumWords ||
        formCount() > kMaximumForms) {
        return {};
    }

    std::vector<std::uint32_t> order(words_.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = static_cast<std::uint32_t>(index);
    }
    std::sort(order.begin(), order.end(), [this](std::uint32_t left, std::uint32_t right) {
        if (words_[left].display != words_[right].display) {
            return words_[left].display < words_[right].display;
        }
        return words_[left].hint < words_[right].hint;
    });

    std::vector<std::uint32_t> remap(words_.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        remap[order[index]] = static_cast<std::uint32_t>(index);
    }

    struct FinalForm {
        SearchChannel channel;
        std::string key;
        std::uint32_t wordId;
        std::uint32_t score;
    };

    std::vector<std::uint32_t> wordScores(words_.size(), 0);
    for (const auto& form : forms_) {
        wordScores[form.wordId] = (std::max)(
            wordScores[form.wordId], form.score);
    }

    std::vector<FinalForm> finalForms;
    finalForms.reserve(formCount());
    for (std::size_t index = 0; index < words_.size(); ++index) {
        const Word& word = words_[index];
        finalForms.push_back({SearchChannel::Bangla, word.display,
                              remap[index], wordScores[index]});
    }
    for (const auto& form : forms_) {
        finalForms.push_back({form.channel, form.key, remap[form.wordId], form.score});
    }

    std::sort(finalForms.begin(), finalForms.end(), [](const FinalForm& left, const FinalForm& right) {
        if (left.channel != right.channel) {
            return left.channel < right.channel;
        }
        if (keyBefore(left.key, right.key)) {
            return true;
        }
        if (keyBefore(right.key, left.key)) {
            return false;
        }
        if (left.wordId != right.wordId) {
            return left.wordId < right.wordId;
        }
        return left.score > right.score;
    });
    finalForms.erase(std::unique(finalForms.begin(), finalForms.end(),
                                 [](const FinalForm& left, const FinalForm& right) {
                                     return left.channel == right.channel &&
                                            left.key == right.key &&
                                            left.wordId == right.wordId;
                                 }),
                     finalForms.end());

    if (finalForms.size() > kMaximumForms) {
        return {};
    }

    std::string blob;
    std::unordered_map<std::string, std::uint32_t> interned;
    const auto intern = [&blob, &interned](std::string_view value) -> std::uint32_t {
        const std::string copy(value);
        const auto found = interned.find(copy);
        if (found != interned.end()) {
            return found->second;
        }
        if (value.size() > kMaximumBlobBytes ||
            blob.size() > static_cast<std::size_t>(kMaximumBlobBytes) - value.size()) {
            return static_cast<std::uint32_t>(kMaximumBlobBytes);
        }
        const auto offset = static_cast<std::uint32_t>(blob.size());
        blob.append(value);
        interned.emplace(copy, offset);
        return offset;
    };

    std::string formSection(finalForms.size() * kFormBytes, '\0');
    std::uint32_t maximumKeyLength = 0;
    for (std::size_t index = 0; index < finalForms.size(); ++index) {
        const auto& form = finalForms[index];
        const std::uint32_t keyOffset = intern(form.key);
        if (keyOffset == kMaximumBlobBytes || form.key.size() > kMaximumKeyBytes ||
            keyOffset > (std::numeric_limits<std::uint32_t>::max)() -
                            static_cast<std::uint32_t>(form.key.size())) {
            return {};
        }
        const std::size_t offset = index * kFormBytes;
        writeU32(formSection, offset + 0, form.wordId);
        writeU32(formSection, offset + 4, keyOffset);
        writeU32(formSection, offset + 8, static_cast<std::uint32_t>(form.key.size()));
        writeU16(formSection, offset + 12, 0);
        formSection[offset + 14] = static_cast<char>(form.channel);
        formSection[offset + 15] = 1;
        writeU32(formSection, offset + 16, form.score);
        maximumKeyLength = (std::max)(
            maximumKeyLength, static_cast<std::uint32_t>(form.key.size()));
    }

    std::string wordSection(words_.size() * kWordBytes, '\0');
    std::uint32_t maximumDisplayLength = 0;
    for (std::size_t newId = 0; newId < order.size(); ++newId) {
        const Word& word = words_[order[newId]];
        const std::uint32_t displayOffset = intern(word.display);
        const std::uint32_t hintOffset = word.hint.empty() ? 0U : intern(word.hint);
        if (displayOffset == kMaximumBlobBytes ||
            (word.hint.empty() ? false : hintOffset == kMaximumBlobBytes)) {
            return {};
        }
        const std::size_t offset = newId * kWordBytes;
        writeU32(wordSection, offset + 0, displayOffset);
        writeU32(wordSection, offset + 4, static_cast<std::uint32_t>(word.display.size()));
        writeU32(wordSection, offset + 8, hintOffset);
        writeU32(wordSection, offset + 12, static_cast<std::uint32_t>(word.hint.size()));
        writeU32(wordSection, offset + 16, 0);
        maximumDisplayLength = (std::max)(
            maximumDisplayLength, static_cast<std::uint32_t>(word.display.size()));
    }

    if (blob.size() > kMaximumBlobBytes) {
        return {};
    }

    struct SectionData {
        std::uint32_t id;
        std::string bytes;
        std::uint16_t elementSize;
        std::uint64_t records;
    };

    std::string meta(kMetaBytes, '\0');
    writeU32(meta, 0, kSectionMeta);
    writeU16(meta, 4, 1);
    writeU16(meta, 6, static_cast<std::uint16_t>(kMetaBytes));
    writeU64(meta, 16, static_cast<std::uint64_t>(words_.size()));
    writeU64(meta, 24, static_cast<std::uint64_t>(finalForms.size()));
    writeU64(meta, 32, static_cast<std::uint64_t>(blob.size()));
    writeU32(meta, 40, maximumKeyLength);
    writeU32(meta, 44, maximumDisplayLength);
    writeU32(meta, 48, static_cast<std::uint32_t>(SearchChannel::Phonetic));
    writeU32(meta, 52, static_cast<std::uint32_t>(SearchChannel::Bangla));
    writeU64(meta, 56, fnv1a64("lekhoni-phonetic-v1", 1469598103934665603ULL));
    writeU64(meta, 64, fnv1a64("lekhoni-utf8-v2", 1099511628211ULL));

    std::array<SectionData, 4> sections{{
        {kSectionMeta, std::move(meta), static_cast<std::uint16_t>(kMetaBytes), 1},
        {kSectionForm, std::move(formSection), static_cast<std::uint16_t>(kFormBytes), finalForms.size()},
        {kSectionWord, std::move(wordSection), static_cast<std::uint16_t>(kWordBytes), words_.size()},
        {kSectionBlob, std::move(blob), 0, 0},
    }};

    const std::size_t tableOffset = kHeaderBytes;
    const std::size_t tableBytes = sections.size() * kSectionEntryBytes;
    std::array<std::uint64_t, 4> sectionOffsets{};
    std::size_t cursor = tableOffset + tableBytes;
    for (std::size_t index = 0; index < sections.size(); ++index) {
        cursor = (cursor + 7U) & ~static_cast<std::size_t>(7U);
        sectionOffsets[index] = cursor;
        cursor += sections[index].bytes.size();
    }
    const std::size_t fileBytes = cursor;
    if (fileBytes > kMaximumFileBytes) {
        return {};
    }

    std::string output(fileBytes, '\0');
    std::memcpy(output.data(), "BGDX", 4);
    writeU16(output, 4, 0xFEFF);
    writeU16(output, 6, 1);
    writeU16(output, 8, 0);
    writeU16(output, 10, static_cast<std::uint16_t>(kHeaderBytes));
    writeU32(output, 12, 0);
    writeU64(output, 16, fileBytes);

    const std::string identitySource = std::to_string(words_.size()) + "|" +
                                      std::to_string(finalForms.size()) + "|" +
                                      std::to_string(crc32(sections[3].bytes));
    const std::uint64_t idLow = fnv1a64(identitySource, 1469598103934665603ULL);
    const std::uint64_t idHigh = fnv1a64(identitySource, 7809847782465536322ULL);
    for (std::size_t index = 0; index < 8; ++index) {
        output[0x18 + index] = static_cast<char>((idLow >> (index * 8U)) & 0xFFU);
        output[0x20 + index] = static_cast<char>((idHigh >> (index * 8U)) & 0xFFU);
    }
    writeU64(output, 0x28, tableOffset);
    writeU64(output, 0x30, tableBytes);
    writeU32(output, 0x38, static_cast<std::uint32_t>(sections.size()));
    writeU32(output, 0x3C, kRequiredFeatures);
    writeU32(output, 0x40, 0);
    writeU32(output, 0x44, 0);

    for (std::size_t index = 0; index < sections.size(); ++index) {
        const std::size_t entry = tableOffset + index * kSectionEntryBytes;
        writeU32(output, entry + 0, sections[index].id);
        writeU16(output, entry + 4, 1);
        writeU16(output, entry + 6, sections[index].elementSize);
        writeU64(output, entry + 8, sectionOffsets[index]);
        writeU64(output, entry + 16, sections[index].bytes.size());
        writeU64(output, entry + 24, sections[index].records);
        writeU32(output, entry + 32, crc32(sections[index].bytes));
        writeU32(output, entry + 36, 0);
        std::memcpy(output.data() + toSize(sectionOffsets[index], output.size()),
                    sections[index].bytes.data(), sections[index].bytes.size());
    }

    writeU32(output, 0x44, crc32(std::string_view(output).substr(tableOffset, tableBytes)));
    writeU32(output, 0x40, crc32(std::string_view(output), 0x40, 4));
    return output;
}

void Dictionary::clear() noexcept {
    bytes_.clear();
    meta_ = {};
    form_ = {};
    word_ = {};
    blob_ = {};
    wordCount_ = 0;
    formCount_ = 0;
    blobBytes_ = 0;
    scoreFloor_ = 0;
    loaded_ = false;
}

bool Dictionary::load(std::string bytes) {
    clear();
    lastError_.clear();
    try {
        bytes_ = std::move(bytes);
        if (!validate(lastError_)) {
            std::string empty;
            bytes_.swap(empty);
            clear();
            return false;
        }
        loaded_ = true;
        return true;
    } catch (const std::exception& error) {
        lastError_ = error.what();
        std::string empty;
        bytes_.swap(empty);
        clear();
        return false;
    } catch (...) {
        lastError_ = "unknown dictionary error";
        std::string empty;
        bytes_.swap(empty);
        clear();
        return false;
    }
}

bool Dictionary::readForm(std::size_t index, FormRecord& form) const {
    if (!loaded_ || index >= formCount_) {
        return false;
    }
    const std::size_t base = toSize(form_.offset, bytes_.size()) + index * kFormBytes;
    if (base > bytes_.size() || kFormBytes > bytes_.size() - base) {
        return false;
    }
    form.wordId = readU32(bytes_, base + 0);
    form.keyOffset = readU32(bytes_, base + 4);
    form.keyLength = readU32(bytes_, base + 8);
    form.channel = static_cast<std::uint8_t>(bytes_[base + 14]);
    form.score = readU32(bytes_, base + 16);
    return true;
}

bool Dictionary::readWord(std::uint32_t wordId, WordRecord& word) const {
    if (!loaded_ || wordId >= wordCount_) {
        return false;
    }
    const std::size_t base = toSize(word_.offset, bytes_.size()) +
                             static_cast<std::size_t>(wordId) * kWordBytes;
    if (base > bytes_.size() || kWordBytes > bytes_.size() - base) {
        return false;
    }
    word.displayOffset = readU32(bytes_, base + 0);
    word.displayLength = readU32(bytes_, base + 4);
    word.hintOffset = readU32(bytes_, base + 8);
    word.hintLength = readU32(bytes_, base + 12);
    return true;
}

std::string_view Dictionary::blobString(std::uint32_t offset,
                                        std::uint32_t length) const noexcept {
    if (!loaded_ || static_cast<std::uint64_t>(offset) + length > blobBytes_) {
        return {};
    }
    const std::size_t base = toSize(blob_.offset, bytes_.size()) + offset;
    if (base > bytes_.size() || length > bytes_.size() - base) {
        return {};
    }
    return std::string_view(bytes_).substr(base, length);
}

bool Dictionary::validate(std::string& error) {
    if (bytes_.size() < kHeaderBytes || bytes_.size() > kMaximumFileBytes) {
        error = "invalid dictionary file size";
        return false;
    }
    if (std::memcmp(bytes_.data(), "BGDX", 4) != 0 || readU16(bytes_, 4) != 0xFEFF ||
        readU16(bytes_, 6) != 1 || readU16(bytes_, 8) != 0 ||
        readU16(bytes_, 10) != kHeaderBytes || readU32(bytes_, 12) != 0 ||
        readU32(bytes_, 0x3C) != kRequiredFeatures) {
        error = "unsupported dictionary header";
        return false;
    }
    if (readU64(bytes_, 16) != bytes_.size()) {
        error = "dictionary size mismatch";
        return false;
    }
    if (readU32(bytes_, 0x40) != crc32(bytes_, 0x40, 4)) {
        error = "dictionary header checksum mismatch";
        return false;
    }

    const std::uint64_t tableOffset64 = readU64(bytes_, 0x28);
    const std::uint64_t tableLength64 = readU64(bytes_, 0x30);
    const std::uint32_t sectionCount = readU32(bytes_, 0x38);
    if (sectionCount == 0 || sectionCount > 16 || tableOffset64 != kHeaderBytes ||
        tableLength64 != static_cast<std::uint64_t>(sectionCount) * kSectionEntryBytes) {
        error = "invalid dictionary section table";
        return false;
    }
    const std::size_t tableOffset = toSize(tableOffset64, bytes_.size());
    const std::size_t tableLength = toSize(tableLength64, bytes_.size());
    if (tableOffset > bytes_.size() || tableLength > bytes_.size() - tableOffset ||
        readU32(bytes_, 0x44) != crc32(std::string_view(bytes_).substr(tableOffset, tableLength))) {
        error = "invalid section table checksum";
        return false;
    }

    struct Range {
        std::uint64_t offset;
        std::uint64_t bytes;
    };
    std::vector<Range> ranges;
    ranges.reserve(sectionCount);
    bool hasMeta = false;
    bool hasForm = false;
    bool hasWord = false;
    bool hasBlob = false;
    const std::uint64_t tableEnd = tableOffset64 + tableLength64;

    for (std::uint32_t index = 0; index < sectionCount; ++index) {
        const std::size_t entry = tableOffset + static_cast<std::size_t>(index) * kSectionEntryBytes;
        Dictionary::Section section;
        section.id = readU32(bytes_, entry + 0);
        section.version = readU16(bytes_, entry + 4);
        section.elementSize = readU16(bytes_, entry + 6);
        section.offset = readU64(bytes_, entry + 8);
        section.bytes = readU64(bytes_, entry + 16);
        section.records = readU64(bytes_, entry + 24);
        if (!isKnownSection(section.id) || section.version != 1 || (section.offset & 7U) != 0 ||
            !addRange(section.offset, section.bytes, bytes_.size()) ||
            section.offset < tableEnd || section.bytes > kMaximumFileBytes) {
            error = "invalid or unknown dictionary section";
            return false;
        }
        if (crc32(std::string_view(bytes_).substr(toSize(section.offset, bytes_.size()),
                                                  toSize(section.bytes, bytes_.size()))) !=
            readU32(bytes_, entry + 32)) {
            error = "dictionary section checksum mismatch";
            return false;
        }
        for (const auto& existing : ranges) {
            if (section.offset < existing.offset + existing.bytes &&
                existing.offset < section.offset + section.bytes) {
                error = "overlapping dictionary sections";
                return false;
            }
        }
        ranges.push_back({section.offset, section.bytes});

        switch (section.id) {
        case kSectionMeta: hasMeta = true; meta_ = section; break;
        case kSectionForm: hasForm = true; form_ = section; break;
        case kSectionWord: hasWord = true; word_ = section; break;
        case kSectionBlob: hasBlob = true; blob_ = section; break;
        default: break;
        }
    }

    if (!hasMeta || !hasForm || !hasWord || !hasBlob ||
        meta_.bytes != kMetaBytes || meta_.elementSize != kMetaBytes || meta_.records != 1 ||
        form_.elementSize != kFormBytes || word_.elementSize != kWordBytes ||
        blob_.elementSize != 0 || blob_.records != 0 || blob_.bytes > kMaximumBlobBytes ||
        form_.records > kMaximumForms || word_.records > kMaximumWords) {
        error = "missing or invalid required dictionary section";
        return false;
    }
    if (form_.records > (std::numeric_limits<std::uint64_t>::max)() / kFormBytes ||
        word_.records > (std::numeric_limits<std::uint64_t>::max)() / kWordBytes ||
        form_.records * kFormBytes != form_.bytes ||
        word_.records * kWordBytes != word_.bytes) {
        error = "dictionary record table size mismatch";
        return false;
    }

    wordCount_ = toSize(word_.records, kMaximumWords);
    formCount_ = toSize(form_.records, kMaximumForms);
    blobBytes_ = blob_.bytes;

    const std::size_t metaOffset = toSize(meta_.offset, bytes_.size());
    if (readU32(bytes_, metaOffset) != kSectionMeta || readU16(bytes_, metaOffset + 4) != 1 ||
        readU16(bytes_, metaOffset + 6) != kMetaBytes ||
        readU64(bytes_, metaOffset + 16) != word_.records ||
        readU64(bytes_, metaOffset + 24) != form_.records ||
        readU64(bytes_, metaOffset + 32) != blob_.bytes ||
        readU32(bytes_, metaOffset + 40) > kMaximumKeyBytes ||
        readU32(bytes_, metaOffset + 44) > kMaximumDisplayBytes ||
        readU32(bytes_, metaOffset + 48) != static_cast<std::uint32_t>(SearchChannel::Phonetic) ||
        readU32(bytes_, metaOffset + 52) != static_cast<std::uint32_t>(SearchChannel::Bangla) ||
        readU64(bytes_, metaOffset + 56) != fnv1a64("lekhoni-phonetic-v1", 1469598103934665603ULL) ||
        readU64(bytes_, metaOffset + 64) != fnv1a64("lekhoni-utf8-v2", 1099511628211ULL)) {
        error = "dictionary metadata mismatch";
        return false;
    }

    const std::string identitySource = std::to_string(word_.records) + "|" +
                                      std::to_string(form_.records) + "|" +
                                      std::to_string(crc32(std::string_view(bytes_).substr(
                                          toSize(blob_.offset, bytes_.size()),
                                          toSize(blob_.bytes, bytes_.size()))));
    if (readU64(bytes_, 0x18) != fnv1a64(identitySource, 1469598103934665603ULL) ||
        readU64(bytes_, 0x20) != fnv1a64(identitySource, 7809847782465536322ULL)) {
        error = "dictionary identity mismatch";
        return false;
    }

    loaded_ = true; // readForm/readWord/blobString require this during validation.
    std::string_view previousKey;
    std::uint8_t previousChannel = 0;
    std::uint32_t previousWordId = 0;
    // Phonetic-channel scores are sampled here because the sweep has to read
    // every form record anyway, so the median costs one extra vector and no
    // extra pass over the image. The Bangla channel is excluded: those records
    // carry a word's best phonetic score rather than an independent weight, so
    // including them would skew the distribution toward the high end.
    std::vector<std::uint32_t> phoneticScores;
    try {
        phoneticScores.reserve(formCount_);
    } catch (...) {
        // A pathologically large image simply gets no score floor, which makes
        // the engine fall back to "no correction" rather than "correct
        // everything". Never fatal: typing must not depend on this.
    }
    for (std::size_t index = 0; index < formCount_; ++index) {
        FormRecord form;
        if (!readForm(index, form) || form.wordId >= wordCount_ ||
            (form.channel != static_cast<std::uint8_t>(SearchChannel::Phonetic) &&
             form.channel != static_cast<std::uint8_t>(SearchChannel::Bangla))) {
            error = "invalid dictionary form record";
            loaded_ = false;
            return false;
        }
        const std::string_view key = blobString(form.keyOffset, form.keyLength);
        if (key.empty() || key.size() > kMaximumKeyBytes || hasNul(key) || !isValidUtf8(key) ||
            (form.channel == static_cast<std::uint8_t>(SearchChannel::Phonetic) &&
             !isPhoneticKey(key))) {
            error = "invalid dictionary search key";
            loaded_ = false;
            return false;
        }
        if (index != 0) {
            const bool ordered = form.channel > previousChannel ||
                                 (form.channel == previousChannel && keyBefore(previousKey, key)) ||
                                 (form.channel == previousChannel && !keyBefore(previousKey, key) &&
                                  !keyBefore(key, previousKey) && form.wordId >= previousWordId);
            if (!ordered) {
                error = "dictionary forms are not sorted";
                loaded_ = false;
                return false;
            }
        }
        if (form.channel == static_cast<std::uint8_t>(SearchChannel::Phonetic)) {
            try {
                phoneticScores.push_back(form.score);
            } catch (...) {
                phoneticScores.clear();
                phoneticScores.shrink_to_fit();
            }
        }
        previousChannel = form.channel;
        previousKey = key;
        previousWordId = form.wordId;
    }

    if (!phoneticScores.empty()) {
        // Median, not mean: the curated band sits far above the corpus plateau,
        // so a mean would sit well above the words that actually make up the
        // dictionary and would exclude most of them from typo correction.
        const std::size_t middle = phoneticScores.size() / 2;
        std::nth_element(phoneticScores.begin(),
                         phoneticScores.begin() + static_cast<std::ptrdiff_t>(middle),
                         phoneticScores.end());
        scoreFloor_ = phoneticScores[middle];
    }

    for (std::size_t index = 0; index < wordCount_; ++index) {
        WordRecord word;
        if (!readWord(static_cast<std::uint32_t>(index), word)) {
            error = "invalid dictionary word record";
            loaded_ = false;
            return false;
        }
        const std::string_view display = blobString(word.displayOffset, word.displayLength);
        const std::string_view hint = blobString(word.hintOffset, word.hintLength);
        if (display.empty() || display.size() > kMaximumDisplayBytes || hasNul(display) ||
            !isValidUtf8(display) || !containsBangla(display) ||
            canonicalizeBangla(display) != display ||
            (word.hintLength != 0 &&
             (hasNul(hint) || !isValidUtf8(hint) ||
              (containsBangla(hint) && canonicalizeBangla(hint) != hint)))) {
            error = "invalid dictionary display word";
            loaded_ = false;
            return false;
        }
    }

    loaded_ = false;
    return true;
}

std::vector<Suggestion> Dictionary::lookup(SearchChannel channel,
                                            std::string_view prefix,
                                            std::size_t limit) const {
    if (!loaded_ || prefix.empty() || limit == 0 || limit > 64 || hasNul(prefix) ||
        !isValidUtf8(prefix) || prefix.size() > kMaximumKeyBytes) {
        return {};
    }

    // Phonetic lookup is case-sensitive: gUR, guR and gur are different
    // spellings of different words. The engine performs the folded fallback.
    const std::string normalized = channel == SearchChannel::Phonetic
                                       ? std::string(prefix)
                                       : canonicalizeBangla(prefix);
    // Storage is fold-ordered, so the scan walks the folded prefix range;
    // acceptance is the raw prefix, keeping guR and gur distinct.
    const std::string foldProbe = channel == SearchChannel::Phonetic
                                      ? asciiLower(prefix)
                                      : normalized;
    const auto keyAcceptable = [&](std::string_view key) {
        if (channel != SearchChannel::Phonetic) {
            return true;
        }
        return key.compare(0, prefix.size(), prefix) == 0;
    };
    if (normalized.empty()) {
        return {};
    }
    const auto channelByte = static_cast<std::uint8_t>(channel);
    const auto lessThanPrefix = [this, channelByte, &normalized, &foldProbe](std::size_t index) {
        FormRecord form;
        if (!readForm(index, form)) {
            return false;
        }
        if (form.channel != channelByte) {
            return form.channel < channelByte;
        }
        return foldKey(blobString(form.keyOffset, form.keyLength)) < std::string_view(foldProbe);
    };

    std::size_t low = 0;
    std::size_t high = formCount_;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (lessThanPrefix(middle)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    std::vector<Suggestion> results;
    const auto better = [](const Suggestion& left, const Suggestion& right) {
        if (left.exact != right.exact) {
            return left.exact;
        }
        if (left.matchedKeyLength != right.matchedKeyLength) {
            return left.matchedKeyLength < right.matchedKeyLength;
        }
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.wordId < right.wordId;
    };

    constexpr std::size_t kMaximumScannedForms = 65536;
    std::size_t scanned = 0;
    for (std::size_t index = low; index < formCount_ && scanned < kMaximumScannedForms;
         ++index, ++scanned) {
        FormRecord form;
        if (!readForm(index, form)) {
            break;
        }
        if (form.channel != channelByte) {
            break;
        }
        const std::string_view key = blobString(form.keyOffset, form.keyLength);
        if (foldKey(key).compare(0, foldProbe.size(), foldProbe) != 0) {
            break;
        }
        if (!keyAcceptable(key)) {
            continue;
        }

        WordRecord word;
        if (!readWord(form.wordId, word)) {
            continue;
        }
        Suggestion candidate;
        candidate.wordId = form.wordId;
        candidate.word = std::string(blobString(word.displayOffset, word.displayLength));
        candidate.hint = std::string(blobString(word.hintOffset, word.hintLength));
        candidate.score = form.score;
        candidate.matchedKeyLength = static_cast<std::uint32_t>(key.size());
        candidate.exact = key == normalized;

        const auto existing = std::find_if(results.begin(), results.end(), [&](const auto& item) {
            return item.wordId == candidate.wordId;
        });
        if (existing != results.end()) {
            if (better(candidate, *existing)) {
                *existing = std::move(candidate);
                std::sort(results.begin(), results.end(), better);
            }
            continue;
        }

        if (results.size() < limit) {
            results.push_back(std::move(candidate));
            std::sort(results.begin(), results.end(), better);
        } else if (better(candidate, results.back())) {
            results.back() = std::move(candidate);
            std::sort(results.begin(), results.end(), better);
        }
    }
    return results;
}

std::vector<Suggestion> Dictionary::lookupFuzzyPhonetic(std::string_view value,
                                                         std::size_t limit) const {
    if (!loaded_ || value.empty() || limit == 0 || limit > 64 || value.size() < 4 ||
        value.size() > 64 || hasNul(value) || !isValidUtf8(value) ||
        !isPhoneticKey(value)) {
        return {};
    }

    const std::string normalized = asciiLower(value);
    const int maximumDistance = normalized.size() >= 6 ? 2 : 1;
    const auto channelByte = static_cast<std::uint8_t>(SearchChannel::Phonetic);
    const std::string firstKey(1, normalized.front());
    const auto lessThanFirst = [this, channelByte, &firstKey](std::size_t index) {
        FormRecord form;
        if (!readForm(index, form)) {
            return false;
        }
        if (form.channel != channelByte) {
            return form.channel < channelByte;
        }
        return foldKey(blobString(form.keyOffset, form.keyLength)) < firstKey;
    };

    std::size_t low = 0;
    std::size_t high = formCount_;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (lessThanFirst(middle)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    std::vector<Suggestion> results;
    const auto better = [](const Suggestion& left, const Suggestion& right) {
        if (left.editDistance != right.editDistance) {
            return left.editDistance < right.editDistance;
        }
        if (left.score != right.score) {
            return left.score > right.score;
        }
        if (left.matchedKeyLength != right.matchedKeyLength) {
            return left.matchedKeyLength < right.matchedKeyLength;
        }
        return left.wordId < right.wordId;
    };

    constexpr std::size_t kMaximumScannedForms = 65536;
    std::size_t scanned = 0;
    for (std::size_t index = low; index < formCount_ && scanned < kMaximumScannedForms;
         ++index, ++scanned) {
        FormRecord form;
        if (!readForm(index, form)) {
            break;
        }
        if (form.channel != channelByte) {
            break;
        }
        const std::string foldedKey = foldKey(blobString(form.keyOffset, form.keyLength));
        if (foldedKey.empty() || foldedKey.front() != normalized.front()) {
            break;
        }
        const std::size_t lengthDifference = foldedKey.size() > normalized.size()
                                                 ? foldedKey.size() - normalized.size()
                                                 : normalized.size() - foldedKey.size();
        if (lengthDifference > static_cast<std::size_t>(maximumDistance)) {
            continue;
        }
        const int distance = editDistance(normalized, foldedKey, maximumDistance);
        if (distance > maximumDistance) {
            continue;
        }

        WordRecord word;
        if (!readWord(form.wordId, word)) {
            continue;
        }
        Suggestion candidate;
        candidate.wordId = form.wordId;
        candidate.word = std::string(blobString(word.displayOffset, word.displayLength));
        candidate.hint = std::string(blobString(word.hintOffset, word.hintLength));
        candidate.score = form.score;
        candidate.matchedKeyLength = static_cast<std::uint32_t>(foldedKey.size());
        candidate.editDistance = static_cast<std::uint8_t>(distance);
        candidate.exact = false;

        const auto existing = std::find_if(results.begin(), results.end(), [&](const auto& item) {
            return item.wordId == candidate.wordId;
        });
        if (existing != results.end()) {
            if (better(candidate, *existing)) {
                *existing = std::move(candidate);
                std::sort(results.begin(), results.end(), better);
            }
            continue;
        }
        if (results.size() < limit) {
            results.push_back(std::move(candidate));
            std::sort(results.begin(), results.end(), better);
        } else if (better(candidate, results.back())) {
            results.back() = std::move(candidate);
            std::sort(results.begin(), results.end(), better);
        }
    }
    return results;
}

bool Dictionary::isKnownWord(std::string_view word) const {
    if (!loaded_ || word.empty() || hasNul(word) || !isValidUtf8(word)) {
        return false;
    }
    const std::string canonical = canonicalizeBangla(word);
    if (canonical.empty()) {
        return false;
    }
    // Bangla-channel keys are the canonical display words, so an exact
    // prefix lookup either finds the word itself or nothing.
    const auto results = lookup(SearchChannel::Bangla, canonical, 1);
    return !results.empty() && results.front().exact &&
           results.front().word == canonical;
}

std::vector<std::string> Dictionary::predictions(std::string_view word,
                                                  std::size_t limit) const {
    limit = (std::min)(limit, static_cast<std::size_t>(16));
    if (!loaded_ || word.empty() || limit == 0 || hasNul(word) || !isValidUtf8(word)) {
        return {};
    }
    const std::string target = canonicalizeBangla(word);
    if (target.empty()) {
        return {};
    }

    // Forms are sorted by (channel, key), so the Bangla section is contiguous
    // and sorted. Seeking the target directly is required here: a linear scan
    // from the start would exhaust its budget inside the much larger Phonetic
    // section and never reach any Bangla form.
    const auto channelByte = static_cast<std::uint8_t>(SearchChannel::Bangla);
    const auto lessThanTarget = [this, channelByte, &target](std::size_t index) {
        FormRecord form;
        if (!readForm(index, form)) {
            return false;
        }
        if (form.channel != channelByte) {
            return form.channel < channelByte;
        }
        return blobString(form.keyOffset, form.keyLength) < std::string_view(target);
    };

    std::size_t low = 0;
    std::size_t high = formCount_;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (lessThanTarget(middle)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    for (std::size_t index = low; index < formCount_; ++index) {
        FormRecord form;
        if (!readForm(index, form) || form.channel != channelByte) {
            break;
        }
        if (blobString(form.keyOffset, form.keyLength) != std::string_view(target)) {
            break;
        }
        WordRecord record;
        if (!readWord(form.wordId, record)) {
            continue;
        }
        const std::string_view display = blobString(record.displayOffset, record.displayLength);
        if (display != target) {
            continue;
        }

        const std::string_view hint = blobString(record.hintOffset, record.hintLength);
        std::vector<std::string> result;
        std::unordered_set<std::string> seen;
        std::size_t start = 0;
        while (start <= hint.size() && result.size() < limit) {
            const std::size_t separator = hint.find('|', start);
            const std::size_t end = separator == std::string_view::npos ? hint.size() : separator;
            const std::string candidate = canonicalizeBangla(hint.substr(start, end - start));
            if (!candidate.empty() && containsBangla(candidate) && seen.insert(candidate).second) {
                result.push_back(candidate);
            }
            if (separator == std::string_view::npos) {
                break;
            }
            start = separator + 1;
        }
        return result;
    }
    return {};
}

} // namespace lekho::bn
