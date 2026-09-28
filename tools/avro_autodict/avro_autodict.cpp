// Development-time converter: Avro Keyboard autodict.dct -> Lekhoni TSV.
//
// Avro's autodict maps a typed Latin sequence to a corrected Avro-phonetic
// spelling (latin -> latin). This tool runs the corrected spelling through the
// real Lekhoni transliterator so the emitted Bangla is exactly what the IME
// would commit, and writes Lekhoni TSV rows for the core dictionary.
// Entries whose result contains no Bangla (emoticon passthroughs) are skipped,
// as are keys outside the phonetic key charset. Avro's backtick cluster
// splitter is stripped: Lekhoni resolves those clusters without it.
// The output is Avro-derived data; see data/README.md before redistributing.

#include "lekhoni/AvroLayoutEngine.h"
#include "lekhoni/PhoneticTransliterator.h"
#include "lekhoni/Utf.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_set>

namespace {

bool isPhoneticKey(const std::string& text) {
    if (text.empty() || text.size() > 64) {
        return false;
    }
    for (const char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: avro_autodict <avro-layout.dat> <autodict.dct> <output.tsv>\n";
        return 2;
    }

    // The corrected spellings are Avro-phonetic, so they must be converted by
    // the same layout engine the IME uses; the built-in rules read S and h
    // differently and would freeze wrong words into the dictionary.
    std::ifstream layoutFile(argv[1], std::ios::binary);
    if (!layoutFile) {
        std::cerr << "cannot read layout table " << argv[1] << '\n';
        return 3;
    }
    std::string layout((std::istreambuf_iterator<char>(layoutFile)),
                       std::istreambuf_iterator<char>());
    lekho::bn::setSharedAvroLayout(layout);
    if (!lekho::bn::layoutActive()) {
        std::cerr << "layout table failed to load\n";
        return 3;
    }

    std::ifstream input(argv[2], std::ios::binary);
    if (!input) {
        std::cerr << "cannot read " << argv[2] << '\n';
        return 3;
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());

    const lekho::bn::PhoneticTransliterator converter;
    std::unordered_set<std::string> claimed;
    std::string output;
    std::size_t total = 0;
    std::size_t kept = 0;
    std::size_t offset = 0;
    while (offset <= content.size()) {
        const std::size_t newline = content.find('\n', offset);
        const std::size_t end = newline == std::string::npos ? content.size() : newline;
        std::string_view line(content.data() + offset, end - offset);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (newline == std::string::npos) {
            break;
        }
        offset = newline + 1;
        ++total;
        if (line.empty() || line[0] == '/' || line[0] == '#') {
            continue;
        }
        const std::size_t separator = line.find(' ');
        if (separator == std::string_view::npos) {
            continue;
        }
        const std::string key(line.substr(0, separator));
        std::string value(line.substr(separator + 1));
        // Avro uses identical key/value pairs to keep input literal. The
        // dictionary format can only store Bangla, so those are skipped. The
        // raw value must be compared: a backtick in the value is a conjunct-
        // break correction (valobastam -> valobas`t a m style), not a
        // passthrough.
        if (value == key) {
            continue;
        }
        // Collapse Avro's cluster-splitting backticks, which the layout
        // engine consumes itself.
        value.erase(std::remove(value.begin(), value.end(), '`'), value.end());
        if (!isPhoneticKey(key) || claimed.count(key) != 0) {
            continue;
        }
        std::string bangla = lekho::bn::canonicalizeBangla(converter.transliterate(value));
        if (!lekho::bn::containsBangla(bangla)) {
            continue;
        }
        claimed.insert(key);
        output += key;
        output += '\t';
        output += bangla;
        output += "\t11000\n";
        ++kept;
    }

    std::ofstream out(argv[3], std::ios::binary);
    out << output;
    out.close();
    std::cout << "autodict: " << total << " lines, " << kept << " converted\n";
    return 0;
}
