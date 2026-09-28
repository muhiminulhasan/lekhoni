#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace lekho::bn {

struct AvroLayoutTable;

// A transliteration engine driven by the Avro Phonetic layout table
// (compiled to BGLT1 by tools/compile_avro_layout.py). The table is GPL
// derived third-party data and is loaded at runtime from beside the IME DLL;
// see data/README.md before redistributing.
//
// When no table is loaded the built-in PhoneticTransliterator rules remain in
// charge, so a missing or invalid table only costs Avro-layout fidelity.
class AvroLayoutEngine final {
public:
    // Validates and owns a copy of the compiled table.
    bool load(std::string bytes);
    bool isLoaded() const noexcept { return table_ != nullptr; }

    // Converts the full input (letters, digits, punctuation, Bangla
    // passthrough). The result is valid UTF-8; malformed input bytes are
    // preserved one byte at a time.
    std::string transliterate(std::string_view input) const;

private:
    std::shared_ptr<const AvroLayoutTable> table_;
};

// Process-wide optional layout, loaded once by the IME at activation. The
// table is immutable after load; readers may call layoutActive() freely.
void setSharedAvroLayout(std::string bytes);
bool layoutActive() noexcept;

// Runs the shared layout over the input. Only meaningful when
// layoutActive() is true.
std::string transliterateWithAvroLayout(std::string_view input);

} // namespace lekho::bn
