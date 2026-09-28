#include "lekhoni/Utf.h"

#include <algorithm>

namespace lekho::bn {

bool decodeUtf8(std::string_view input, std::size_t offset, DecodedCodePoint& result) noexcept {
    if (offset >= input.size()) {
        return false;
    }

    const auto first = static_cast<unsigned char>(input[offset]);
    if (first <= 0x7F) {
        result = {static_cast<char32_t>(first), 1};
        return true;
    }

    if (first >= 0xC2 && first <= 0xDF) {
        if (offset + 1 >= input.size()) {
            return false;
        }
        const auto second = static_cast<unsigned char>(input[offset + 1]);
        if ((second & 0xC0) != 0x80) {
            return false;
        }
        result = {static_cast<char32_t>(((first & 0x1F) << 6) | (second & 0x3F)), 2};
        return true;
    }

    if (first >= 0xE0 && first <= 0xEF) {
        if (offset + 2 >= input.size()) {
            return false;
        }
        const auto second = static_cast<unsigned char>(input[offset + 1]);
        const auto third = static_cast<unsigned char>(input[offset + 2]);
        if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80) {
            return false;
        }
        if (first == 0xE0 && second < 0xA0) {
            return false; // overlong
        }
        if (first == 0xED && second >= 0xA0) {
            return false; // surrogate
        }
        result = {
            static_cast<char32_t>(((first & 0x0F) << 12) |
                                  ((second & 0x3F) << 6) |
                                  (third & 0x3F)),
            3};
        return true;
    }

    if (first >= 0xF0 && first <= 0xF4) {
        if (offset + 3 >= input.size()) {
            return false;
        }
        const auto second = static_cast<unsigned char>(input[offset + 1]);
        const auto third = static_cast<unsigned char>(input[offset + 2]);
        const auto fourth = static_cast<unsigned char>(input[offset + 3]);
        if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80 ||
            (fourth & 0xC0) != 0x80) {
            return false;
        }
        if (first == 0xF0 && second < 0x90) {
            return false; // overlong
        }
        if (first == 0xF4 && second >= 0x90) {
            return false; // > U+10FFFF
        }
        result = {
            static_cast<char32_t>(((first & 0x07) << 18) |
                                  ((second & 0x3F) << 12) |
                                  ((third & 0x3F) << 6) |
                                  (fourth & 0x3F)),
            4};
        return true;
    }

    return false;
}

bool isValidUtf8(std::string_view input) noexcept {
    std::size_t offset = 0;
    while (offset < input.size()) {
        DecodedCodePoint decoded{};
        if (!decodeUtf8(input, offset, decoded)) {
            return false;
        }
        offset += decoded.byteLength;
    }
    return true;
}

void appendUtf8(std::string& output, char32_t value) {
    if (value <= 0x7F) {
        output.push_back(static_cast<char>(value));
    } else if (value <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (value >> 6)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    } else if (value <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (value >> 12)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (value >> 18)));
        output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    }
}

bool isBanglaCodePoint(char32_t value) noexcept {
    return (value >= 0x0980 && value <= 0x09FF) ||
           (value >= 0xA8E0 && value <= 0xA8FF);
}

bool containsBangla(std::string_view text) noexcept {
    std::size_t offset = 0;
    while (offset < text.size()) {
        DecodedCodePoint decoded{};
        if (!decodeUtf8(text, offset, decoded)) {
            return false;
        }
        if (isBanglaCodePoint(decoded.value)) {
            return true;
        }
        offset += decoded.byteLength;
    }
    return false;
}

std::string canonicalizeBangla(std::string_view text) {
    std::string output;
    output.reserve(text.size());

    std::size_t offset = 0;
    char32_t previous = 0;
    std::size_t previousOffset = 0;
    while (offset < text.size()) {
        DecodedCodePoint decoded{};
        if (!decodeUtf8(text, offset, decoded)) {
            return {};
        }

        char32_t replacement = decoded.value;
        if (previous == 0x09A1 && decoded.value == 0x09BC) {
            replacement = 0x09DC; // ড়
        } else if (previous == 0x09A2 && decoded.value == 0x09BC) {
            replacement = 0x09DD; // ঢ়
        } else if (previous == 0x09AF && decoded.value == 0x09BC) {
            replacement = 0x09DF; // য়
        } else {
            previousOffset = output.size();
            appendUtf8(output, decoded.value);
            previous = decoded.value;
            offset += decoded.byteLength;
            continue;
        }

        output.resize(previousOffset);
        appendUtf8(output, replacement);
        previous = replacement;
        offset += decoded.byteLength;
    }
    return output;
}

bool isAsciiLetter(char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

char asciiLower(char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    if (byte >= 'A' && byte <= 'Z') {
        return static_cast<char>(byte + ('a' - 'A'));
    }
    return value;
}

std::string asciiLower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char value : text) {
        result.push_back(asciiLower(value));
    }
    return result;
}

void trimAsciiWhitespace(std::string_view input, std::string& output) {
    const auto isSpace = [](char value) {
        return value == ' ' || value == '\t' || value == '\r' || value == '\n';
    };

    std::size_t first = 0;
    while (first < input.size() && isSpace(input[first])) {
        ++first;
    }

    std::size_t last = input.size();
    while (last > first && isSpace(input[last - 1])) {
        --last;
    }

    output.assign(input.substr(first, last - first));
}

} // namespace lekho::bn
