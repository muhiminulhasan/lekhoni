#pragma once

#include <guiddef.h>

namespace lekho::win {

// Project-owned COM identifiers. Do not reuse identifiers from another IME.
constexpr CLSID kTextServiceClsid = {
    0x6E315A4D, 0xB7F3, 0x4C4A,
    {0x91, 0xE2, 0x59, 0xA8, 0x30, 0x44, 0x7D, 0x19}};

constexpr GUID kLanguageProfileGuid = {
    0x0B3C773A, 0x728F, 0x4A3C,
    {0xB5, 0x4B, 0xB1, 0x92, 0x6D, 0x11, 0x74, 0xEC}};

// Identifies the single display attribute this text service publishes. Hosts
// resolve GUID_PROP_ATTRIBUTE values through ITfCategoryMgr, so the identifier
// has to stay stable for the lifetime of the profile.
constexpr GUID kCompositionDisplayAttributeGuid = {
    0x7B9F89E8, 0x70F7, 0x439C,
    {0x8B, 0x74, 0x46, 0x0E, 0x35, 0xD4, 0x3C, 0x28}};

} // namespace lekho::win
