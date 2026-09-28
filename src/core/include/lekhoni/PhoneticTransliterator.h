#pragma once

#include <string>
#include <string_view>

namespace lekho::bn {

// An original, longest-match phonetic converter. It has no external
// dictionary or Unicode dependency and operates entirely on UTF-8 bytes.
class PhoneticTransliterator final {
public:
    [[nodiscard]] std::string transliterate(std::string_view input) const;
};

} // namespace lekho::bn
