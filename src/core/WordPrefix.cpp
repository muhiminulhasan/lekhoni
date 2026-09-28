#include "lekhoni/WordPrefix.h"

#include "lekhoni/Utf.h"

#include <algorithm>
#include <cstddef>

namespace lekho::bn {
namespace {

bool isPhoneticCharacter(char value) noexcept {
    return isAsciiLetter(value) || value == '\'' || value == '-';
}

} // namespace

WordPrefix findWordPrefix(std::string_view text, std::size_t maximumBytes) noexcept {
    try {
        if (text.empty() || maximumBytes == 0) {
            return {};
        }

        const std::size_t searchStart = text.size() > maximumBytes
                                            ? text.size() - maximumBytes
                                            : 0;
        std::size_t tokenStart = text.size();

        while (tokenStart > searchStart) {
            const unsigned char previous = static_cast<unsigned char>(text[tokenStart - 1]);
            if (previous < 0x80) {
                if (!isPhoneticCharacter(text[tokenStart - 1])) {
                    break;
                }
                --tokenStart;
                continue;
            }

            std::size_t scan = tokenStart;
            while (scan > searchStart &&
                   (static_cast<unsigned char>(text[scan - 1]) & 0xC0) == 0x80) {
                --scan;
            }
            if (scan == searchStart) {
                break;
            }

            DecodedCodePoint decoded{};
            if (!decodeUtf8(text, scan - 1, decoded) ||
                decoded.byteLength != tokenStart - (scan - 1)) {
                break;
            }

            if (!isBanglaCodePoint(decoded.value)) {
                break;
            }
            tokenStart = scan - 1;
        }

        if (tokenStart == text.size()) {
            return {};
        }

        std::string token(text.substr(tokenStart, text.size() - tokenStart));
        const WordKind kind = containsBangla(token) ? WordKind::Bangla : WordKind::Phonetic;
        return {kind, std::move(token)};
    } catch (...) {
        // Prefix detection is a convenience path. Allocation failure must not
        // prevent the IME from forwarding a key to the application.
        return {};
    }
}

} // namespace lekho::bn
