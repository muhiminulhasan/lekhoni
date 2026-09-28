#include "lekhoni/SelectionMemory.h"

#include "lekhoni/Utf.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace lekho::bn {

namespace {

constexpr std::uint32_t kMagicBGS1 = 0x31534742U;  // 'BGS1'
constexpr std::uint32_t kMagicBGS2 = 0x32534742U;  // 'BGS2'
constexpr std::size_t kMaximumWordBytes = 1024;
constexpr std::uint32_t kMaximumCount = 255;

// 128-bit key hash: two FNV-1a 64 passes with different seeds. The typed key
// itself never reaches the file.
std::pair<std::uint64_t, std::uint64_t> hashKey(std::string_view key) {
    std::uint64_t low = 1469598103934665603ULL;
    std::uint64_t high = 7809847782465536322ULL;
    for (const char value : key) {
        const auto byte = static_cast<unsigned char>(value);
        low ^= byte;
        low *= 1099511628211ULL;
        high ^= byte;
        high *= 1099511628211ULL;
        high ^= 0x9E3779B97F4A7C15ULL;
    }
    return {low, high};
}

} // namespace

// Table layout (little-endian):
//   u32 magic ('BGS2'), u16 version (=2), u16 reserved, u32 entry count,
//   per entry: u64 hashLow, u64 hashHigh, u16 wordLen, word bytes,
//              u8 count, u32 day,
//   u64 FNV-1a hash of everything before.
// The BGS1 layout differed only in reading the count as u32; the loader
// accepts both and imports BGS1 entries with day = 0 (no decay).

double SelectionMemory::decayedScore(const Entry& entry, std::uint32_t currentDay) {
    if (entry.day == 0 || currentDay <= entry.day) {
        return static_cast<double>(entry.count);
    }
    const std::uint64_t age = currentDay - entry.day;
    const double elapsed = age > 4096 ? 4096.0 : static_cast<double>(age);
    return static_cast<double>(entry.count) * std::pow(0.5, elapsed / 14.0);
}

void SelectionMemory::record(std::string_view key, std::string_view word,
                             std::uint32_t dayStamp) {
    if (key.empty() || word.empty() || word.size() > kMaximumWordBytes) {
        return;
    }
    try {
        const std::string normalized = asciiLower(key);
        const auto [hashLow, hashHigh] = hashKey(normalized);
        const std::string stored(word);

        const auto groupBegin = std::lower_bound(
            entries_.begin(), entries_.end(), std::make_pair(hashHigh, hashLow),
            [](const Entry& entry, const std::pair<std::uint64_t, std::uint64_t>& probe) {
                return std::make_pair(entry.hashHigh, entry.hashLow) < probe;
            });
        auto groupEnd = groupBegin;
        while (groupEnd != entries_.end() && groupEnd->hashHigh == hashHigh &&
               groupEnd->hashLow == hashLow) {
            ++groupEnd;
        }

        for (auto it = groupBegin; it != groupEnd; ++it) {
            if (it->word == stored) {
                if (it->count < kMaximumCount) {
                    ++it->count;
                }
                it->day = dayStamp == 0 ? 1 : dayStamp;
                return;
            }
        }

        if (entries_.size() >= kMaximumEntries &&
            (groupBegin == groupEnd || entries_.size() > kMaximumEntries)) {
            // Global cap: drop the weakest entry anywhere. A brand-new entry
            // scores 1.0 (count 1, current day), so only evict when something
            // is strictly weaker.
            auto weakest = entries_.begin();
            double weakestScore = decayedScore(*weakest, dayStamp);
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                const double score = decayedScore(*it, dayStamp);
                if (score < weakestScore) {
                    weakestScore = score;
                    weakest = it;
                }
            }
            if (weakestScore >= 1.0) {
                return;
            }
            entries_.erase(weakest);
        }

        Entry entry;
        entry.hashLow = hashLow;
        entry.hashHigh = hashHigh;
        entry.word = stored;
        entry.count = 1;
        entry.day = dayStamp == 0 ? 1 : dayStamp;

        if (static_cast<std::size_t>(groupEnd - groupBegin) >= kWordsPerKey) {
            // Group full: replace its weakest member when the newcomer is
            // fresher; otherwise the new pick is dropped.
            auto weakest = groupBegin;
            double weakestScore = decayedScore(*weakest, dayStamp);
            for (auto it = groupBegin; it != groupEnd; ++it) {
                const double score = decayedScore(*it, dayStamp);
                if (score < weakestScore) {
                    weakestScore = score;
                    weakest = it;
                }
            }
            if (weakestScore >= 1.0) {
                return;
            }
            *weakest = std::move(entry);
            return;
        }

        entries_.insert(groupEnd, std::move(entry));
    } catch (...) {
        // Remembering is a convenience; losing a record never blocks typing.
        entries_.clear();
    }
}

std::vector<std::string> SelectionMemory::find(std::string_view key,
                                               std::uint32_t currentDay) const {
    std::vector<std::string> result;
    if (key.empty() || entries_.empty()) {
        return result;
    }
    const auto [hashLow, hashHigh] = hashKey(asciiLower(key));
    const auto groupBegin = std::lower_bound(
        entries_.begin(), entries_.end(), std::make_pair(hashHigh, hashLow),
        [](const Entry& entry, const std::pair<std::uint64_t, std::uint64_t>& probe) {
            return std::make_pair(entry.hashHigh, entry.hashLow) < probe;
        });
    auto groupEnd = groupBegin;
    while (groupEnd != entries_.end() && groupEnd->hashHigh == hashHigh &&
           groupEnd->hashLow == hashLow) {
        ++groupEnd;
    }
    if (groupBegin == groupEnd) {
        return result;
    }

    std::vector<const Entry*> group;
    group.reserve(static_cast<std::size_t>(groupEnd - groupBegin));
    for (auto it = groupBegin; it != groupEnd; ++it) {
        group.push_back(&*it);
    }
    std::stable_sort(group.begin(), group.end(),
                     [currentDay](const Entry* left, const Entry* right) {
                         return decayedScore(*left, currentDay) >
                                decayedScore(*right, currentDay);
                     });
    result.reserve(group.size());
    for (const Entry* entry : group) {
        result.push_back(entry->word);
    }
    return result;
}

std::string SelectionMemory::save() const {
    try {
        std::string out;
        out.reserve(16 + entries_.size() * 48);
        const std::uint32_t magic = kMagicBGS2;
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>((magic >> (i * 8U)) & 0xFFU));
        }
        const std::uint16_t version = 2;
        out.push_back(static_cast<char>(version & 0xFFU));
        out.push_back(static_cast<char>((version >> 8U) & 0xFFU));
        out.push_back('\0');
        out.push_back('\0');
        const std::uint32_t count = static_cast<std::uint32_t>(entries_.size());
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>((count >> (i * 8U)) & 0xFFU));
        }
        for (const Entry& entry : entries_) {
            for (int i = 0; i < 8; ++i) {
                out.push_back(static_cast<char>((entry.hashLow >> (i * 8U)) & 0xFFU));
            }
            for (int i = 0; i < 8; ++i) {
                out.push_back(static_cast<char>((entry.hashHigh >> (i * 8U)) & 0xFFU));
            }
            const std::uint16_t wordLength = static_cast<std::uint16_t>(entry.word.size());
            out.push_back(static_cast<char>(wordLength & 0xFFU));
            out.push_back(static_cast<char>((wordLength >> 8U) & 0xFFU));
            out.append(entry.word);
            out.push_back(static_cast<char>(entry.count & 0xFFU));
            for (int i = 0; i < 4; ++i) {
                out.push_back(static_cast<char>((entry.day >> (i * 8U)) & 0xFFU));
            }
        }
        std::uint64_t hash = 0xCBF29CE484222325ULL;
        for (const char value : out) {
            hash ^= static_cast<unsigned char>(value);
            hash *= 1099511628211ULL;
        }
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<char>((hash >> (i * 8U)) & 0xFFU));
        }
        return out;
    } catch (...) {
        return {};
    }
}

namespace {

// Shared entry reader for BGS1 (countSize 4) and BGS2 (countSize 1).
bool readEntry(const std::string& bytes, std::size_t& offset, std::size_t bodyEnd,
               std::size_t countSize,
               std::uint64_t& hashLow, std::uint64_t& hashHigh, std::string& word,
               std::uint32_t& count, std::uint32_t& day) {
    if (offset + 18 > bodyEnd) {
        return false;
    }
    hashLow = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        hashLow |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + i]))
                   << (i * 8U);
    }
    offset += 8;
    hashHigh = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        hashHigh |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + i]))
                    << (i * 8U);
    }
    offset += 8;
    const std::uint16_t wordLength = static_cast<std::uint16_t>(
        static_cast<unsigned char>(bytes[offset]) |
        (static_cast<unsigned char>(bytes[offset + 1]) << 8U));
    offset += 2;
    if (wordLength == 0 || wordLength > kMaximumWordBytes ||
        offset + wordLength + countSize + 4 > bodyEnd) {
        return false;
    }
    word = bytes.substr(offset, wordLength);
    offset += wordLength;
    count = 0;
    for (std::size_t i = 0; i < countSize; ++i) {
        count |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i]))
                 << (i * 8U);
    }
    offset += countSize;
    day = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        day |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i]))
               << (i * 8U);
    }
    offset += 4;
    return true;
}

} // namespace

bool SelectionMemory::load(std::string bytes) {
    entries_.clear();
    const auto fail = [&] {
        entries_.clear();
        return false;
    };
    if (bytes.size() < 20) {
        return fail();
    }
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    for (int i = 0; i < 4; ++i) {
        magic |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i]))
                 << (i * 8U);
    }
    offset += 4;
    const bool legacy = magic == kMagicBGS1;
    if (!legacy && magic != kMagicBGS2) {
        return fail();
    }
    const std::uint16_t version = static_cast<std::uint16_t>(
        static_cast<unsigned char>(bytes[offset]) |
        (static_cast<unsigned char>(bytes[offset + 1]) << 8U));
    if (version != (legacy ? 1 : 2)) {
        return fail();
    }
    offset += 4;  // version + reserved
    std::uint32_t count = 0;
    for (int i = 0; i < 4; ++i) {
        count |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i]))
                 << (i * 8U);
    }
    offset += 4;
    if (count > kMaximumEntries) {
        return fail();
    }
    const std::size_t bodyEnd = bytes.size() - 8;
    const std::size_t countSize = legacy ? 4 : 1;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint64_t hashLow = 0;
        std::uint64_t hashHigh = 0;
        std::string word;
        std::uint32_t wordCount = 0;
        std::uint32_t day = 0;
        if (!readEntry(bytes, offset, bodyEnd, countSize, hashLow,
                       hashHigh, word, wordCount, day)) {
            return fail();
        }
        if (!isValidUtf8(word)) {
            return fail();
        }
        Entry entry;
        entry.hashLow = hashLow;
        entry.hashHigh = hashHigh;
        entry.word = std::move(word);
        entry.count = legacy ? 1 : (wordCount > kMaximumCount ? kMaximumCount : wordCount);
        // Imported BGS1 entries carry day 0: they rank by count alone until
        // the user picks the word again.
        entry.day = legacy ? 0 : day;
        entries_.push_back(std::move(entry));
    }
    if (offset != bodyEnd) {
        return fail();
    }
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (std::size_t i = 0; i < bodyEnd; ++i) {
        hash ^= static_cast<unsigned char>(bytes[i]);
        hash *= 1099511628211ULL;
    }
    std::uint64_t stored = 0;
    for (int i = 0; i < 8; ++i) {
        stored |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[bodyEnd + i]))
                  << (i * 8U);
    }
    if (stored != hash) {
        return fail();
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        return std::make_pair(a.hashHigh, a.hashLow) < std::make_pair(b.hashHigh, b.hashLow);
    });
    return true;
}

} // namespace lekho::bn
