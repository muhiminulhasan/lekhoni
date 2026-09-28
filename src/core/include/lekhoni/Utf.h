#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lekho::bn {

struct DecodedCodePoint {
    char32_t value;
    std::size_t byteLength;
};

// Strictly decodes one UTF-8 code point. Invalid bytes are rejected.
bool decodeUtf8(std::string_view input, std::size_t offset, DecodedCodePoint& result) noexcept;
bool isValidUtf8(std::string_view input) noexcept;
void appendUtf8(std::string& output, char32_t value);

bool isBanglaCodePoint(char32_t value) noexcept;
bool containsBangla(std::string_view text) noexcept;
// Converts the small set of Bengali precomposed/decomposed pairs that are
// visually equivalent in normal text. ZWJ and ZWNJ are deliberately preserved.
// An empty result means the input was not valid UTF-8.
std::string canonicalizeBangla(std::string_view text);
bool isAsciiLetter(char value) noexcept;
char asciiLower(char value) noexcept;
std::string asciiLower(std::string_view text);
void trimAsciiWhitespace(std::string_view input, std::string& output);

} // namespace lekho::bn
