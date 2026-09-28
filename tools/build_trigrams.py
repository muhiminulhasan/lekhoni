#!/usr/bin/env python3
"""Builds the Lekhoni trigram next-word table (BGTG1) from a Bangla corpus.

Input: UTF-8 prose, one or more documents. Output: data/compiled/LekhoniTrigrams.dat,
loaded beside the IME DLL. The table maps a context word pair to its most
frequent continuations, ordered by frequency; lookup falls back from the
trigram context to the bigram tail.

The output is derived from the input corpus and inherits its license terms;
see data/README.md before redistributing.

File layout (little-endian), matching src/core/TrigramModel.cpp:
  u32 magic 'BGTG', u16 version 1, u16 reserved,
  u32 bigramContexts, u32 trigramContexts, u32 entryBytes,
  per bigram context sorted by hash:  u64 hash, u32 offset, u8 count,
  per trigram context sorted by hash: u64 hash, u32 offset, u8 count,
  continuation blob: per context, (u16 byteLen + UTF-8 bytes) per word,
  u64 FNV-1a hash of everything before.

Context hash: FNV-1a64 over w1 UTF-8 + 0x00 + w2 UTF-8; bigrams use an
empty w1.
"""

import io
import re
import struct
import sys
import unicodedata

MAGIC = 0x47544742  # 'BGTG'
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MAX_WORD_BYTES = 400
MAX_CONTINUATIONS = 6
MIN_BIGRAM_COUNT = 5
MIN_TRIGRAM_COUNT = 3
MAX_CONTEXTS = 150_000
BANGLA_RUN = re.compile(r"[\u0980-\u09FF\u200C\u200D]+")
SENTENCE_SPLIT = re.compile(r"[।!?,;:\(\)\[\]{}\"“”‘’\n\r\t0-9\-–—…]|[a-zA-Z]+")


def fnv(data):
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def canonical(word):
    """NFC plus the project's nukta mapping, matching canonicalizeBangla."""
    word = unicodedata.normalize("NFC", word)
    for decomposed, composed in (
        ("\u09a1\u09bc", "\u09dc"),
        ("\u09a2\u09bc", "\u09dd"),
        ("\u09af\u09bc", "\u09df"),
    ):
        word = word.replace(decomposed, composed)
    return word.strip("\u200c\u200d")


def context_hash(w1, w2):
    return fnv(w1.encode("utf-8") + b"\x00" + w2.encode("utf-8"))


def main(corpus_path, output_path):
    bigram_counts = {}   # w2 -> {next: n}
    trigram_counts = {}  # (w1, w2) -> {next: n}

    def sentences():
        with io.open(corpus_path, encoding="utf-8") as f:
            for line in f:
                for sentence in SENTENCE_SPLIT.split(line):
                    yield sentence

    words_in_sentence = []
    sentence_count = 0

    def prune_low(threshold):
        """Lossy-counting guard: drop rare continuations so the counters stay
        bounded over very large corpora. Frequent contexts are unaffected."""
        for counts in (bigram_counts, trigram_counts):
            for key in list(counts.keys()):
                counter = counts[key]
                for word in list(counter.keys()):
                    if counter[word] < threshold:
                        del counter[word]
                if not counter:
                    del counts[key]

    for sentence in sentences():
        sentence_count += 1
        if sentence_count % 1_000_000 == 0:
            prune_low(2 + sentence_count // 2_000_000)
        for raw in BANGLA_RUN.findall(sentence):
            word = canonical(raw)
            if not word:
                continue
            raw_bytes = len(word.encode("utf-8"))
            if raw_bytes == 0 or raw_bytes > MAX_WORD_BYTES:
                words_in_sentence.clear()
                continue
            words_in_sentence.append(word)
            if len(words_in_sentence) >= 3:
                w1, w2, w3 = words_in_sentence[-3:]
                trigram_counts.setdefault((w1, w2), {})
                trigram_counts[(w1, w2)][w3] = trigram_counts[(w1, w2)].get(w3, 0) + 1
                bigram_counts.setdefault(w2, {})
                bigram_counts[w2][w3] = bigram_counts[w2].get(w3, 0) + 1
            elif len(words_in_sentence) == 2:
                w2 = words_in_sentence[1]
                bigram_counts.setdefault(w2, {})
        if len(words_in_sentence) > 4096:
            words_in_sentence = words_in_sentence[-2:]

    def prune(counts, min_total, label):
        contexts = []
        for key, counter in counts.items():
            total = sum(counter.values())
            if total < min_total:
                continue
            best = sorted(counter.items(), key=lambda kv: (-kv[1], kv[0]))[:MAX_CONTINUATIONS]
            contexts.append((key, best, total))
        contexts.sort(key=lambda item: -item[2])
        contexts = contexts[:MAX_CONTEXTS]
        print(f"{label}: kept {len(contexts)} contexts")
        return contexts

    bigrams = prune(bigram_counts, MIN_BIGRAM_COUNT, "bigrams")
    trigrams = prune(trigram_counts, MIN_TRIGRAM_COUNT, "trigrams")

    # Serialize: entries blob first, then contexts referencing offsets.
    blob = bytearray()
    bigram_rows = []
    for key, best, _total in bigrams:
        w2 = key
        offset = len(blob)
        for word, _count in best:
            raw = word.encode("utf-8")
            blob += struct.pack("<H", len(raw)) + raw
        bigram_rows.append((context_hash("", w2), offset, len(best)))
    trigram_rows = []
    for key, best, _total in trigrams:
        w1, w2 = key
        offset = len(blob)
        for word, _count in best:
            raw = word.encode("utf-8")
            blob += struct.pack("<H", len(raw)) + raw
        trigram_rows.append((context_hash(w1, w2), offset, len(best)))

    bigram_rows.sort(key=lambda r: r[0])
    trigram_rows.sort(key=lambda r: r[0])

    out = io.BytesIO()
    out.write(struct.pack("<IHH", MAGIC, 1, 0))
    out.write(struct.pack("<III", len(bigram_rows), len(trigram_rows), len(blob)))
    for h, offset, count in bigram_rows:
        out.write(struct.pack("<QIB", h, offset, count))
    for h, offset, count in trigram_rows:
        out.write(struct.pack("<QIB", h, offset, count))
    out.write(bytes(blob))
    body = out.getvalue()
    with open(output_path, "wb") as f:
        f.write(body)
        f.write(struct.pack("<Q", fnv(body)))
    print(f"wrote {output_path}: bigrams={len(bigram_rows)} trigrams={len(trigram_rows)} "
          f"bytes={len(body) + 8}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: build_trigrams.py <corpus.txt> <output.dat>")
    main(sys.argv[1], sys.argv[2])
