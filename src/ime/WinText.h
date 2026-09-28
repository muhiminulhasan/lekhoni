#pragma once

#include <string>
#include <string_view>

namespace lekho::win {

std::wstring toWide(std::string_view utf8) noexcept;
std::string toUtf8(std::wstring_view text);
std::wstring normalizeBangla(std::wstring_view text);
std::string normalizeBanglaUtf8(std::string_view utf8);

} // namespace lekho::win
