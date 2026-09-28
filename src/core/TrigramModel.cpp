#include "lekhoni/TrigramModel.h"

#include "lekhoni/Utf.h"

#include <algorithm>
#include <limits>

namespace lekho::bn {
namespace {

constexpr std::uint32_t kMagic = 0x47544742U;  // 'BGTG'
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kMaximumContexts = 400000;
constexpr std::size_t kMaximumEntryBytes = 32U * 1024U * 1024U;
constexpr std::size_t kMaximumWordBytes = 1024;

std::uint16_t readU16(const std::string& b, std::size_t o) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(b[o])) |
           static_cast<std::uint16_t>(static_cast<unsigned char>(b[o + 1]) << 8U);
}

std::uint32_t readU32(const std::string& b, std::size_t o) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(static_cast<unsigned char>(b[o + i])) << (i * 8U);
    }
    return v;
}

std::uint64_t readU64(const std::string& b, std::size_t o) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(b[o + i])) << (i * 8U);
    }
    return v;
}

std::uint64_t fnv1a64(std::string_view bytes) {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (const char value : bytes) {
        hash ^= static_cast<unsigned char>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Context hashing: FNV over w1, a NUL separator, then w2. An empty w1
// collapses to the bigram key of w2, which is exactly the fallback semantic
// the lookup needs.
std::uint64_t contextHash(std::string_view w1, std::string_view w2) {
    std::string key;
    key.reserve(w1.size() + w2.size() + 1);
    key.append(w1);
    key.push_back('\0');
    key.append(w2);
    return fnv1a64(key);
}

} // namespace

bool TrigramModel::load(std::string bytes) {
    loaded_ = false;
    bigrams_.clear();
    trigrams_.clear();
    entries_.clear();

    const auto fail = [&] {
        bigrams_.clear();
        trigrams_.clear();
        entries_.clear();
        loaded_ = false;
        return false;
    };

    if (bytes.size() < 24 || readU32(bytes, 0) != kMagic || readU16(bytes, 4) != kVersion) {
        return fail();
    }
    const std::uint32_t bigramCount = readU32(bytes, 8);
    const std::uint32_t trigramCount = readU32(bytes, 12);
    const std::uint32_t entryBytes = readU32(bytes, 16);
    if (bigramCount > kMaximumContexts || trigramCount > kMaximumContexts ||
        entryBytes > kMaximumEntryBytes) {
        return fail();
    }

    const std::uint64_t storedHash = readU64(bytes, bytes.size() - 8);
    if (storedHash != fnv1a64(std::string_view(bytes).substr(0, bytes.size() - 8))) {
        return fail();
    }

    std::size_t offset = 20;
    const auto readContexts = [&](std::uint32_t count, std::vector<Context>& region) {
        region.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (offset + 13 > bytes.size()) {
                return false;
            }
            Context context;
            context.hash = readU64(bytes, offset);
            context.offset = readU32(bytes, offset + 8);
            context.count = static_cast<unsigned char>(bytes[offset + 12]);
            offset += 13;
            if (context.count == 0) {
                return false;
            }
            if (i != 0 && !(region[i - 1].hash < context.hash)) {
                return false;  // must be strictly sorted for binary search
            }
            region[i] = context;
        }
        return true;
    };

    if (!readContexts(bigramCount, bigrams_) || !readContexts(trigramCount, trigrams_)) {
        return fail();
    }

    if (entryBytes != 0) {
        if (offset + entryBytes > bytes.size() - 8) {
            return fail();
        }
        try {
            entries_ = bytes.substr(offset, entryBytes);
        } catch (...) {
            return fail();
        }
        offset += entryBytes;
    }

    // Every context offset/count must land inside the entries region, and
    // every (len, word) pair inside it must be well-formed.
    const auto validateRegion = [&](const std::vector<Context>& region) {
        for (const Context& context : region) {
            std::size_t position = context.offset;
            for (std::uint8_t i = 0; i < context.count; ++i) {
                if (position + 2 > entries_.size()) {
                    return false;
                }
                const std::uint16_t length = readU16(entries_, position);
                position += 2;
                if (length == 0 || length > kMaximumWordBytes ||
                    position + length > entries_.size()) {
                    return false;
                }
                position += length;
            }
        }
        return true;
    };
    if (!validateRegion(bigrams_) || !validateRegion(trigrams_)) {
        return fail();
    }
    if (offset != bytes.size() - 8) {
        return fail();
    }

    loaded_ = true;
    return true;
}

std::vector<std::string> TrigramModel::lookupRegion(const std::vector<Context>& region,
                                                    std::uint64_t hash,
                                                    std::size_t limit) const {
    std::vector<std::string> result;
    const auto found = std::lower_bound(
        region.begin(), region.end(), hash,
        [](const Context& context, std::uint64_t probe) { return context.hash < probe; });
    if (found == region.end() || found->hash != hash) {
        return result;
    }
    std::size_t position = found->offset;
    for (std::uint8_t i = 0; i < found->count && result.size() < limit; ++i) {
        const std::uint16_t length = readU16(entries_, position);
        position += 2;
        result.emplace_back(entries_, position, length);
        position += length;
    }
    return result;
}

std::vector<std::string> TrigramModel::next(std::string_view w1, std::string_view w2,
                                            std::size_t limit) const {
    std::vector<std::string> result;
    if (!loaded_ || w2.empty() || limit == 0) {
        return result;
    }
    if (!w1.empty()) {
        result = lookupRegion(trigrams_, contextHash(w1, w2), limit);
        if (!result.empty()) {
            return result;
        }
    }
    return lookupRegion(bigrams_, contextHash({}, w2), limit);
}

} // namespace lekho::bn
