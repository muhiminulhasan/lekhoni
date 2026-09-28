#include "lekhoni/AvroLayoutEngine.h"
#include "lekhoni/Dictionary.h"
#include "lekhoni/TrigramModel.h"
#include "lekhoni/InputEngine.h"
#include "lekhoni/PhoneticTransliterator.h"
#include "lekhoni/Utf.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <windows.h>

namespace {

struct AuditEntry {
    std::string phonetic;
    std::string bangla;
};

struct EvalRow {
    std::string phonetic;
    std::string bangla;
    std::string group;
};

bool readFile(const std::filesystem::path& path, std::string& bytes) {
    std::error_code error;
    const auto length = std::filesystem::file_size(path, error);
    if (error || length > 128U * 1024U * 1024U) {
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    bytes.resize(static_cast<std::size_t>(length));
    input.read(bytes.data(), static_cast<std::streamsize>(length));
    return input.gcount() == static_cast<std::streamsize>(length);
}

bool parseScore(std::string_view text, std::uint32_t& score) {
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), score, 10);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// Splits a UTF-8 TSV line on tabs. Returns false when the line has no tabs.
bool splitTabs(std::string_view line, std::vector<std::string_view>& fields) {
    fields.clear();
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string_view::npos) {
            fields.push_back(line.substr(start));
            return true;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

bool readEvalFile(const std::filesystem::path& path, std::vector<EvalRow>& rows) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    std::vector<std::string_view> fields;
    std::size_t offset = 0;
    while (offset < content.size()) {
        std::size_t newline = content.find('\n', offset);
        const std::size_t end = newline == std::string::npos ? content.size() : newline;
        std::string_view line(content.data() + offset, end - offset);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        // Advance past the line, not one byte at a time. Leaving `end` pinned
        // to the first newline while `offset` crept forward produced a garbage
        // row for every remaining byte of the file, which is how a 64-row set
        // turned into 1279 nonsense cases and a meaningless 3.5% top-1.
        offset = newline == std::string::npos ? content.size() : newline + 1;

        if (line.empty() || line.front() == '#') {
            continue;
        }
        splitTabs(line, fields);
        if (fields.size() < 2 || fields[0].empty() || fields[1].empty()) {
            continue;
        }
        rows.push_back({std::string(fields[0]), std::string(fields[1]),
                        fields.size() >= 3 ? std::string(fields[2]) : std::string("exact")});
    }
    return true;
}

// Feeds a key to the engine one code point at a time, which is the only way to
// exercise it: the candidate list is rebuilt on every keystroke, so composing a
// key as a single append would skip every intermediate state.
void typeKey(lekho::bn::InputEngine& engine, std::string_view key) {
    engine.reset();
    std::size_t offset = 0;
    while (offset < key.size()) {
        lekho::bn::DecodedCodePoint decoded{};
        if (!lekho::bn::decodeUtf8(key, offset, decoded)) {
            ++offset;
            continue;
        }
        engine.appendCodePoint(decoded.value);
        offset += decoded.byteLength;
    }
}

// What the user actually gets, in the order they meet it.
//
// This must mirror TextService::finishComposition, which commits
// CandidateWindow::selected() and only falls back to directOutput when the
// candidate list is empty. CandidateWindow::selected() returns candidates[0]
// for any non-empty list, because selectedIndex_ defaults to 0 and is reset to
// 0 by setCandidates(). So a fuzzy correction placed in slot 0 is what Space
// commits -- directOutput is *not* what the user gets just because it is the
// engine's "direct" conversion. Ranking directOutput first therefore reported
// every successful correction as a miss, which is how the typo group read 0.97%
// when the correction was in fact landing in the committed slot.
std::vector<std::string> visibleOutcome(const lekho::bn::InputEngine& engine) {
    std::vector<std::string> outcome;
    outcome.reserve(10);
    for (const auto& candidate : engine.candidates()) {
        if (std::find(outcome.begin(), outcome.end(), candidate.utf8) == outcome.end()) {
            outcome.push_back(candidate.utf8);
        }
        if (outcome.size() >= 9) {
            break;
        }
    }
    if (outcome.empty() && !engine.directOutput().empty()) {
        outcome.push_back(engine.directOutput());
    }
    return outcome;
}

struct Metrics {
    std::string label;
    long long total = 0;
    long long topOne = 0;
    long long topFive = 0;
    double reciprocalRank = 0.0;
    void add(long long rank) {
        ++total;
        if (rank == 0) {
            ++topOne;
        }
        if (rank >= 0 && rank < 5) {
            ++topFive;
        }
        if (rank >= 0) {
            reciprocalRank += 1.0 / static_cast<double>(rank + 1);
        }
    }
};

void report(const Metrics& metrics) {
    const auto asPercent = [total = metrics.total](long long count) {
        return total == 0 ? 0.0
                          : 100.0 * static_cast<double>(count) / static_cast<double>(total);
    };
    const double mrr = metrics.total == 0
                           ? 0.0
                           : metrics.reciprocalRank / static_cast<double>(metrics.total);
    std::printf("  %-8s n=%-4lld top-1 %6.2f%%   top-5 %6.2f%%   MRR %.4f\n",
                metrics.label.c_str(), metrics.total, asPercent(metrics.topOne),
                asPercent(metrics.topFive), mrr);
}

// Types `typed` and returns the zero-based position of `expected` in the
// outcome list, or -1 when the user could not reach the word at all. A rank of
// 0 means the committed text was already right, so the user needed no
// correction keypresses; rank 1 means the first arrow-down would have found it.
//
// The comparison is canonical on both sides. The engine always emits
// canonicalised Bangla -- PhoneticTransliterator and DictionaryBuilder both run
// canonicalizeBangla -- so an expectation written with the decomposed nukta
// spelling (U+09A1 U+09BC for U+09DC, U+09AF U+09BC for U+09DF) can never
// string-compare equal to correct output. Nine rows of bari/boro/ghora/choy were
// reported as unreachable when the engine had in fact put the right word in
// slot 0. Canonicalising the expectation is the same normalisation the engine
// applies to its own output, so it removes a spelling-form artefact without
// weakening the test: a genuinely wrong word still does not match.
long long rankOf(lekho::bn::InputEngine& engine, std::string_view typed,
                 const std::string& expected) {
    typeKey(engine, typed);
    const std::vector<std::string> outcome = visibleOutcome(engine);
    const std::string wanted = lekho::bn::canonicalizeBangla(expected);
    const auto found = std::find(outcome.begin(), outcome.end(), wanted);
    return found == outcome.end() ? -1 : static_cast<long long>(found - outcome.begin());
}

// Prints a word as space-separated code points. A miss where the offered word
// renders identically to the expected one is a normalisation difference, not a
// wrong answer, and the only way to tell them apart is to look at the bytes:
// canonicalizeBangla maps U+09DC (ড়) to U+09A1 U+09BC and U+09DF (য়) to
// U+09AF U+09BC, so a hand-written expectation that used the precomposed form
// never string-compares equal to the engine's canonical output.
void printCodePoints(std::string_view word) {
    std::size_t offset = 0;
    bool first = true;
    while (offset < word.size()) {
        lekho::bn::DecodedCodePoint decoded{};
        if (!lekho::bn::decodeUtf8(word, offset, decoded)) {
            break;
        }
        std::printf("%sU+%04X", first ? "" : " ", static_cast<unsigned>(decoded.value));
        first = false;
        offset += decoded.byteLength;
    }
    if (first) {
        std::printf("(empty)");
    }
}

void printMiss(lekho::bn::InputEngine& engine, std::string_view typed,
               const std::string& expected) {
    const std::vector<std::string> outcome = visibleOutcome(engine);
    std::printf("    MISS  %-16s expected \"%s\"\n", std::string(typed).c_str(),
                expected.c_str());
    if (outcome.empty()) {
        std::printf("          (nothing was offered)\n");
        return;
    }
    std::printf("          direct:    \"%s\"\n", engine.directOutput().c_str());
    std::printf("          slot 0:    \"%s\"", outcome.front().c_str());
    for (std::size_t index = 1; index < outcome.size(); ++index) {
        std::printf(", \"%s\"", outcome[index].c_str());
    }
    std::printf("\n");

    // Flag the case that reads as a bug and is not one: an expectation that
    // differs from slot 0 only in spelling form rather than in content.
    if (outcome.front() != expected) {
        const std::string canonical = lekho::bn::canonicalizeBangla(expected);
        if (canonical == outcome.front()) {
            std::printf("          NOTE: expectation is not canonical; slot 0 matches it "
                        "after canonicalizeBangla. Fix the eval row.\n");
            std::printf("          expected bytes:  ");
            printCodePoints(expected);
            std::printf("\n          canonical:       ");
            printCodePoints(canonical);
            std::printf("\n");
        } else {
            // A genuine miss. Show the code points for both so a difference
            // that renders invisibly is still visible.
            std::printf("          slot 0 bytes:   ");
            printCodePoints(outcome.front());
            std::printf("\n          expected bytes: ");
            printCodePoints(expected);
            std::printf("\n");
        }
    }
}

bool writeAtomically(const std::filesystem::path& path, std::string_view bytes) {
    std::error_code error;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return false;
        }
    }

    std::filesystem::path temporary = path;
    temporary += L".tmp";
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool success = true;
    std::size_t writtenTotal = 0;
    while (writtenTotal < bytes.size()) {
        const std::size_t remaining = bytes.size() - writtenTotal;
        const DWORD chunk = static_cast<DWORD>((std::min)(
            remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + writtenTotal, chunk, &written, nullptr) || written == 0) {
            success = false;
            break;
        }
        writtenTotal += written;
    }
    if (success && !FlushFileBuffers(file)) {
        success = false;
    }
    CloseHandle(file);

    if (!success || !MoveFileExW(temporary.c_str(), path.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

int runEvaluation(const std::filesystem::path& evalPath,
                  const std::filesystem::path& corePath,
                  const std::filesystem::path& phrasePath,
                  const std::filesystem::path& layoutPath) {
    std::vector<EvalRow> rows;
    if (!readEvalFile(evalPath, rows)) {
        std::wcerr << L"Cannot read the evaluation set.\n";
        return 3;
    }
    if (rows.empty()) {
        std::wcerr << L"The evaluation set is empty.\n";
        return 3;
    }

    // The IME prefers the Avro Phonetic layout table whenever one is installed
    // beside the module, and only falls back to the built-in rules when it is
    // absent. Measuring without loading it would score the fallback path and
    // report it as if it were the product, so load it here too.
    std::string layoutBytes;
    bool layoutLoaded = false;
    if (!layoutPath.empty() && readFile(layoutPath, layoutBytes) && !layoutBytes.empty()) {
        lekho::bn::setSharedAvroLayout(layoutBytes);
        layoutLoaded = lekho::bn::layoutActive();
    }
    if (!layoutLoaded) {
        std::cout << "NOTE: no Avro layout table at "
                  << (layoutPath.empty() ? std::string("(none given)") : layoutPath.string())
                  << "; scoring the built-in fallback rules instead of the shipped"
                     " transliteration path.\n";
    }

    lekho::bn::InputEngine engine;

    std::string coreBytes;
    if (!readFile(corePath, coreBytes)) {
        std::wcerr << L"Cannot read the core dictionary image.\n";
        return 3;
    }
    if (!engine.loadDictionary(coreBytes)) {
        std::cerr << "Core dictionary failed to load; accuracy cannot be measured.\n";
        return 3;
    }

    std::string phraseBytes;
    if (readFile(phrasePath, phraseBytes) && !phraseBytes.empty()) {
        if (!engine.loadPhrases(phraseBytes)) {
            std::cerr << "WARNING: the phrase image failed to load; phrase cases will "
                         "under-report.\n";
        }
    } else {
        std::cout << "NOTE: no phrase image at " << phrasePath.string()
                  << "; the phrase layer is being measured as absent.\n";
    }

    std::printf("Accuracy against %s\n", evalPath.filename().string().c_str());
    std::printf("core: %zu words, %zu forms, median phonetic score %u, transliteration: %s\n\n",
                engine.dictionaryWordCount(), engine.dictionaryFormCount(),
                engine.dictionaryScoreFloor(),
                layoutLoaded ? "Avro layout table" : "built-in fallback rules");

    Metrics exact{"exact"};
    Metrics prefix{"prefix"};
    Metrics caseGroup{"case"};
    Metrics typo{"typo"};

    long long misses = 0;
    for (const EvalRow& row : rows) {
        Metrics* target = &exact;
        std::string typed = row.phonetic;
        if (row.group == "prefix") {
            target = &prefix;
            // Drop the last two characters so the engine is mid-word. A short
            // truncated prefix has no meaningful slot 0: for "pa" the top
            // suggestion is legitimately পা and for "ch" it is legitimately ছ,
            // so requiring the full word there scored the engine for being
            // right. Four characters is the shortest prefix that expresses a
            // preference rather than a coincidence.
            if (row.phonetic.size() < 6) {
                continue;
            }
            typed = row.phonetic.substr(0, row.phonetic.size() - 2);
        } else if (row.group == "case") {
            target = &caseGroup;
        }

        const long long rank = rankOf(engine, typed, row.bangla);
        target->add(rank);
        if (rank < 0) {
            ++misses;
            if (misses <= 25) {
                printMiss(engine, typed, row.bangla);
            }
        }
    }

    // Typo recovery is generated, not listed: every `exact` row of at least four
    // characters is mutated in the four ways real typists get a word wrong.
    long long typoTested = 0;
    for (const EvalRow& row : rows) {
        if (row.group != "exact" || row.phonetic.size() < 4) {
            continue;
        }
        const std::string key = row.phonetic;
        std::vector<std::string> mutations;
        mutations.push_back(key.substr(0, key.size() - 1));                    // dropped letter
        mutations.push_back(key.substr(0, key.size() / 2) + key.substr(key.size() / 2 + 1));
        std::string swapped = key;                                             // transposition
        std::swap(swapped[key.size() / 2], swapped[key.size() / 2 + 1]);
        mutations.push_back(swapped);
        std::string doubled = key;                                             // doubled letter
        doubled.insert(doubled.begin() + 1, key[1]);
        mutations.push_back(doubled);

        for (const std::string& mutation : mutations) {
            if (mutation == key) {
                continue;
            }
            ++typoTested;
            typo.add(rankOf(engine, mutation, row.bangla));
        }
    }

    report(exact);
    report(caseGroup);
    report(typo);
    // top-1 is printed for completeness but is not the meaningful figure for
    // this group: mid-word, the engine's first suggestion is whichever
    // completion the corpus ranks highest, which is frequently a shorter and
    // more common word than the one being typed. Reachability is the question,
    // so top-5 and MRR are the numbers to watch.
    report(prefix);
    std::printf("  (prefix top-1 is not a meaningful target; read top-5 and MRR)\n");
    std::printf("\n  rows read: %zu, typo mutations generated: %lld, total misses: %lld\n",
                rows.size(), typoTested, misses);
    return 0;
}

} // namespace

// Measures how often the trigram table's stored continuations contain the
// actual next word over a corpus. This is training-corpus coverage: an upper
// bound on real next-word hit rate, useful as a regression guard and for
// judging whether the top-K lists hold the right words at all.
int runNextWord(const std::filesystem::path& corpusPath,
                const std::filesystem::path& tablePath) {
    std::ifstream corpus(corpusPath, std::ios::binary);
    if (!corpus) {
        std::wcerr << L"Cannot read corpus." << std::endl;
        return 3;
    }
    const std::string content((std::istreambuf_iterator<char>(corpus)),
                              std::istreambuf_iterator<char>());

    lekho::bn::TrigramModel model;
    std::ifstream tableFile(tablePath, std::ios::binary);
    const std::string table((std::istreambuf_iterator<char>(tableFile)),
                            std::istreambuf_iterator<char>());
    if (!model.load(table)) {
        std::wcerr << L"Trigram table failed validation." << std::endl;
        return 3;
    }

    std::vector<std::string> sentence;
    sentence.reserve(64);
    std::uint64_t positions = 0;
    std::uint64_t top1 = 0;
    std::uint64_t top3 = 0;

    const auto flushSentence = [&]() {
        for (std::size_t i = 2; i < sentence.size(); ++i) {
            const auto suggestions =
                model.next(sentence[i - 2], sentence[i - 1], 3);
            if (suggestions.empty()) {
                continue;
            }
            ++positions;
            if (suggestions[0] == sentence[i]) {
                ++top1;
            }
            for (const auto& suggestion : suggestions) {
                if (suggestion == sentence[i]) {
                    ++top3;
                    break;
                }
            }
        }
        sentence.clear();
    };

    std::string word;
    std::size_t sinceFlush = 0;
    for (const char byte : content) {
        const unsigned char value = static_cast<unsigned char>(byte);
        if (value >= 0x80 || (value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z')) {
            word.push_back(byte);
            continue;
        }
        if (!word.empty()) {
            sentence.push_back(word);
            word.clear();
            ++sinceFlush;
            if (sinceFlush >= 200000) {
                flushSentence();
                sinceFlush = 0;
            }
        }
    }
    if (!word.empty()) {
        sentence.push_back(word);
    }
    if (!sentence.empty()) {
        flushSentence();
    }

    std::cout << "next-word coverage: positions=" << positions
              << " top-1="
              << (positions ? 100.0 * top1 / positions : 0.0) << "%"
              << " top-3="
              << (positions ? 100.0 * top3 / positions : 0.0) << "%"
              << " (training-corpus upper bound)" << std::endl;
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--score") {
        if (argc != 5 && argc != 6) {
            std::wcerr << L"Usage: LekhoniDictionaryCompiler --score <eval.tsv> "
                          L"<core.bgk> <phrases.bgk> [avro-layout.dat]\n";
            return 2;
        }
        return runEvaluation(std::filesystem::path(argv[2]), std::filesystem::path(argv[3]),
                             std::filesystem::path(argv[4]),
                             argc == 6 ? std::filesystem::path(argv[5])
                                       : std::filesystem::path());
    }

    if (argc == 4 && std::wstring_view(argv[1]) == L"--nextword") {
        return runNextWord(std::filesystem::path(argv[2]),
                           std::filesystem::path(argv[3]));
    }

    const bool auditOnly = argc == 3 && std::wstring_view(argv[1]) == L"--audit";
    if (argc != 3) {
        std::wcerr << L"Usage: LekhoniDictionaryCompiler <input.tsv> <output.bgk>\n"
                      L"       LekhoniDictionaryCompiler --audit <input.tsv>\n"
                      L"       LekhoniDictionaryCompiler --nextword <corpus.txt> <trigrams.dat>\n"
                      L"       LekhoniDictionaryCompiler --score <eval.tsv> <core.bgk> "
                      L"<phrases.bgk> [avro-layout.dat]\n";
        return 2;
    }

    const std::filesystem::path inputPath = auditOnly ? std::filesystem::path(argv[2])
                                                       : std::filesystem::path(argv[1]);
    const std::filesystem::path outputPath = auditOnly ? std::filesystem::path()
                                                       : std::filesystem::path(argv[2]);
    std::string content;
    if (!readFile(inputPath, content)) {
        std::wcerr << L"Cannot read input dictionary.\n";
        return 3;
    }

    lekho::bn::DictionaryBuilder builder;
    std::vector<AuditEntry> entries;
    std::size_t lineNumber = 0;
    std::size_t accepted = 0;
    std::size_t offset = content.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    while (offset <= content.size()) {
        const std::size_t newline = content.find('\n', offset);
        const std::size_t end = newline == std::string::npos ? content.size() : newline;
        std::string_view line(content.data() + offset, end - offset);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        ++lineNumber;

        if (!line.empty() && line.front() != '#') {
            const std::size_t firstTab = line.find('\t');
            const std::size_t secondTab = firstTab == std::string_view::npos
                                              ? std::string_view::npos
                                              : line.find('\t', firstTab + 1);
            const std::size_t thirdTab = secondTab == std::string_view::npos
                                             ? std::string_view::npos
                                             : line.find('\t', secondTab + 1);
            if (firstTab == std::string_view::npos || secondTab == std::string_view::npos ||
                (thirdTab != std::string_view::npos &&
                 line.find('\t', thirdTab + 1) != std::string_view::npos)) {
                std::wcerr << L"Invalid TSV at line " << lineNumber << L".\n";
                return 4;
            }

            const std::string_view phonetic = line.substr(0, firstTab);
            const std::string_view bangla = line.substr(firstTab + 1, secondTab - firstTab - 1);
            const std::size_t scoreEnd = thirdTab == std::string_view::npos
                                            ? line.size()
                                            : thirdTab;
            const std::string_view scoreText = line.substr(secondTab + 1,
                                                           scoreEnd - secondTab - 1);
            const std::string_view prediction = thirdTab == std::string_view::npos
                                                    ? std::string_view()
                                                    : line.substr(thirdTab + 1);
            std::uint32_t score = 0;
            if (!parseScore(scoreText, score) ||
                !builder.add(phonetic, bangla, score, prediction)) {
                std::wcerr << L"Invalid dictionary entry at line " << lineNumber << L".\n";
                return 5;
            }
            ++accepted;
            if (auditOnly) {
                entries.push_back({std::string(phonetic), std::string(bangla)});
            }
        }

        if (newline == std::string::npos) {
            break;
        }
        offset = newline + 1;
    }

    if (auditOnly) {
        const lekho::bn::PhoneticTransliterator converter;
        std::size_t mismatches = 0;
        for (const auto& entry : entries) {
            const std::string direct = lekho::bn::canonicalizeBangla(
                converter.transliterate(entry.phonetic));
            const std::string preferred = lekho::bn::canonicalizeBangla(entry.bangla);
            if (direct != preferred) {
                ++mismatches;
                std::cout << entry.phonetic << " -> " << direct << " (preferred "
                          << preferred << ")\n";
            }
        }
        std::cout << "Audit: " << entries.size() << " rows, " << mismatches
                  << " direct/preferred differences (exact dictionary ranking resolves them).\n";
        return 0;
    }

    const std::string image = builder.build();
    if (image.empty()) {
        std::wcerr << L"Dictionary build failed.\n";
        return 6;
    }
    lekho::bn::Dictionary verification;
    if (!verification.load(image)) {
        std::cerr << "Generated dictionary failed validation: "
                  << verification.lastError() << '\n';
        return 7;
    }
    if (!writeAtomically(outputPath, image)) {
        std::wcerr << L"Dictionary atomic write failed.\n";
        return 6;
    }

    std::wcout << L"Compiled " << accepted << L" source entries into "
               << verification.wordCount() << L" words and " << verification.formCount()
               << L" forms (" << image.size() << L" bytes).\n";
    return 0;
}
