#include "WinText.h"

#include "lekhoni/Utf.h"

#include <windows.h>
#include <winnls.h>

#include <limits>
#include <vector>

namespace lekho::win {

std::wstring toWide(std::string_view utf8) noexcept {
    if (utf8.empty()) {
        return {};
    }
    if (utf8.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), output.data(), required) != required) {
        return {};
    }
    return output;
}

std::string toUtf8(std::wstring_view text) {
    std::string output;
    output.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        char32_t value = static_cast<char16_t>(text[index]);
        if (value >= 0xD800 && value <= 0xDBFF) {
            if (index + 1 >= text.size()) {
                return {};
            }
            const char32_t low = static_cast<char16_t>(text[index + 1]);
            if (low < 0xDC00 || low > 0xDFFF) {
                return {};
            }
            value = 0x10000 + ((value - 0xD800) << 10) + (low - 0xDC00);
            ++index;
        } else if (value >= 0xDC00 && value <= 0xDFFF) {
            return {};
        }
        bn::appendUtf8(output, value);
    }
    return output;
}

std::wstring normalizeBangla(std::wstring_view text) {
    if (text.empty() || text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        !bn::containsBangla(toUtf8(text))) {
        return std::wstring(text);
    }

    try {
        // The source must be the caller's text and the destination a separate
        // buffer. Passing the destination as the source normalizes whatever the
        // buffer happened to contain instead of the input.
        //
        // When the destination is too small NormalizeString returns the
        // negative of the number of characters required, so grow and retry.
        std::size_t capacity = text.size() * 2 + 8;
        for (int attempt = 0; attempt < 4; ++attempt) {
            std::vector<wchar_t> buffer(capacity, L'\0');
            const int length = NormalizeString(
                NormalizationC, text.data(), static_cast<int>(text.size()), buffer.data(),
                static_cast<int>(buffer.size()));
            if (length > 0) {
                return std::wstring(buffer.data(), static_cast<std::size_t>(length));
            }
            if (length == 0) {
                // Invalid input. Keeping the original text is safer than
                // returning a partially rewritten string.
                break;
            }
            const auto required = static_cast<std::size_t>(-static_cast<long long>(length));
            if (required <= capacity) {
                // No forward progress, so stop instead of looping.
                break;
            }
            capacity = required;
        }
    } catch (...) {
        // Normalization is an improvement, never a requirement. Any failure
        // leaves the text exactly as the caller supplied it.
    }
    return std::wstring(text);
}

std::string normalizeBanglaUtf8(std::string_view utf8) {
    const std::wstring wide = toWide(utf8);
    if (wide.empty()) {
        return {};
    }
    return toUtf8(normalizeBangla(wide));
}

} // namespace lekho::win
