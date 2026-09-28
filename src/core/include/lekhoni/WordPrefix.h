#pragma once

#include <string>
#include <string_view>

namespace lekho::bn {

enum class WordKind {
    Empty,
    Phonetic,
    Bangla,
};

struct WordPrefix {
    WordKind kind = WordKind::Empty;
    std::string utf8;
};

// Returns the final token suitable for prefix lookup. The scan is bounded to
// prevent malformed documents from causing unbounded work.
WordPrefix findWordPrefix(std::string_view text, std::size_t maximumBytes = 256) noexcept;

} // namespace lekho::bn
