#!/usr/bin/env python3
"""Generates per-word phonetic keys for the Lekhoni core dictionary.

Reads the compiled merged BGDX1 image, reverse-transliterates every Bangla
word into the phonetic key a user would type (Avro-style), and emits a Lekhoni
TSV with one row per (word, key). Curated keys that already exist in the image
(starter words) are preserved and outrank generated keys through scoring.

Juktabarna (conjunct) words get two keys when they differ:
  - the consonant skeleton (montri spelling: mntri), and
  - an inherent-o variant that mirrors how the word is pronounced and typed
    (montri, swopno, yontro/jontro).
Words starting with ya also get a leading-j variant, matching the way the
letter is pronounced and the way Avro's own word lists index it.

Development-time tool. The output TSV is Avro-derived (GPL-2.0-or-later);
see data/README.md.
"""

import io
import struct
import sys

VOWELS = {
    # Keys mirror the Avro Phonetic layout: capital O carries the O-sign,
    # OI/OU the diphthongs, and word-initial o yields the O letter.
    "\u0985": "o", "\u0986": "a", "\u0987": "i", "\u0988": "i", "\u0989": "u",
    "\u098A": "u", "\u098B": "rri",
    "\u098F": "e", "\u0990": "OI", "\u0993": "w", "\u0994": "OU",
    "\u09BE": "a", "\u09BF": "i", "\u09C0": "i", "\u09C1": "u", "\u09C2": "u",
    "\u09C3": "rri", "\u09C7": "e",
    "\u09C8": "OI", "\u09CB": "O", "\u09CC": "OU",
    "\u0982": "ng", "\u0983": "", "\u0981": "", "\u09CE": "t",
}

CONSONANTS = {
    "\u0995": "k", "\u0996": "kh", "\u0997": "g", "\u0998": "gh", "\u0999": "Ng",
    "\u099A": "ch", "\u099B": "Ch", "\u099C": "j", "\u099D": "jh", "\u099E": "NG",
    "\u099F": "T", "\u09A0": "Th", "\u09A1": "D", "\u09A2": "Dh", "\u09A3": "N",
    "\u09A4": "t", "\u09A5": "th", "\u09A6": "d", "\u09A7": "dh", "\u09A8": "n",
    "\u09AA": "p", "\u09AB": "ph", "\u09AC": "b", "\u09AD": "bh", "\u09AE": "m",
    "\u09AF": "y", "\u09B0": "r", "\u09B2": "l", "\u09B6": "sh", "\u09B7": "Sh",
    "\u09B8": "s", "\u09B9": "h",
    "\u09A1\u09BC": "R", "\u09A2\u09BC": "Rh", "\u09AF\u09BC": "Y",
    "ড়": "R", "ঢ়": "Rh", "য়": "Y",
}

HASANTA = "\u09CD"
YA = "\u09AF"
BA = "\u09AC"


def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def u64(d, o):
    return struct.unpack_from("<Q", d, o)[0]


def read_bgk(path):
    data = io.open(path, "rb").read()
    assert data[:4] == b"BGDX", "not a BGDX image"
    table_offset = u64(data, 0x28)
    count = u32(data, 0x38)
    sections = {}
    for i in range(count):
        e = table_offset + i * 40
        sections[u32(data, e)] = (u64(data, e + 8), u64(data, e + 16), u64(data, e + 24))
    form_off, _, form_records = sections[struct.unpack("<I", b"FORM")[0]]
    word_off, _, word_records = sections[struct.unpack("<I", b"WORD")[0]]
    blob_off, blob_size, _ = sections[struct.unpack("<I", b"BLOB")[0]]
    blob = data[blob_off:blob_off + blob_size]

    words = []
    for w in range(word_records):
        base = word_off + w * 32
        display = blob[u32(data, base):u32(data, base) + u32(data, base + 4)].decode("utf-8")
        hint = blob[u32(data, base + 8):u32(data, base + 8) + u32(data, base + 12)].decode("utf-8")
        words.append((display, hint))

    scores = {}
    phonetic_keys = set()
    for f in range(form_records):
        base = form_off + f * 32
        wid = u32(data, base)
        key = blob[u32(data, base + 4):u32(data, base + 4) + u32(data, base + 8)].decode("utf-8")
        channel = data[base + 14]
        score = u32(data, base + 16)
        if channel == 2 and wid not in scores:
            scores[wid] = score
        if channel == 1:
            phonetic_keys.add(key)
    return words, scores, phonetic_keys


def is_phonetic_key(text):
    return bool(text) and all(
        ("a" <= c <= "z") or ("A" <= c <= "Z") or c in "'-" for c in text
    )


def split_units(word):
    """Splits a word into units.

    A hasanta group is a list of consonants joined by hasanta; every other
    mappable character becomes a single-char unit (string). Returns None for
    characters the engine cannot map.
    """
    units = []
    i = 0
    n = len(word)
    while i < n:
        ch = word[i]
        if i + 1 < n and ch + word[i + 1] in CONSONANTS:  # nukta pairs
            units.append(ch + word[i + 1])
            i += 2
            continue
        if ch == HASANTA:
            return None
        if ch in CONSONANTS:
            group = [ch]
            while (i + 2 < n and word[i + 1] == HASANTA and word[i + 2] != HASANTA
                   and (word[i + 2] in CONSONANTS
                        or (i + 3 < n and word[i + 2] + word[i + 3] in CONSONANTS))):
                nxt = word[i + 2]
                if i + 3 < n and nxt + word[i + 3] in CONSONANTS:
                    nxt = nxt + word[i + 3]
                    i += 1
                group.append(nxt)
                i += 2
            units.append(group)
            i += 1
            continue
        if ch in VOWELS:
            units.append(ch)
            i += 1
            continue
        return None
    return units


def unit_phonetic(unit):
    """Maps one unit (group or single char) to the phonetic a user types."""
    if isinstance(unit, list):
        # Ba-phala (X+hasanta+ba) is typed X + w: swa -> sw, dwa -> dw.
        if len(unit) >= 2 and unit[-1] == BA:
            return "".join(CONSONANTS[c] for c in unit[:-1]) + "w"
        return "".join(CONSONANTS[c] for c in unit)
    if unit in CONSONANTS:
        return CONSONANTS[unit]
    return VOWELS[unit]


def keys_for_word(word):
    """Returns the list of typable keys for one Bangla word."""
    units = split_units(word)
    if units is None:
        return []

    skeleton = "".join(unit_phonetic(u) for u in units)
    keys = [skeleton]

    # Juktabarna words also get the inherent-o variant users actually type:
    # an "o" is inserted between two adjacent consonant units, and a trailing
    # "o" closes a word that ends in a consonant group.
    if any(isinstance(u, list) and len(u) > 1 for u in units):
        parts = [unit_phonetic(u) for u in units]
        hybrid = ""
        for index, part in enumerate(parts):
            if (index > 0 and isinstance(units[index], list)
                    and parts[index - 1] != ""
                    and not (isinstance(units[index - 1], str)
                             and units[index - 1] in VOWELS)):
                hybrid += "o"
            hybrid += part
        if units and isinstance(units[-1], list):
            hybrid += "o"
        if hybrid != skeleton:
            keys.append(hybrid)

    # Word-initial ya is pronounced j: add the leading-j variant of every key.
    if word.startswith(YA):
        for key in list(keys):
            if key.startswith("y"):
                candidate = "j" + key[1:]
                if candidate not in keys:
                    keys.append(candidate)

    return [k for k in keys if is_phonetic_key(k)]


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else "data/compiled/LekhoniCore.merged.bgk"
    existing = sys.argv[2] if len(sys.argv) > 2 else "data/source/imported-merged.tsv"
    output = sys.argv[3] if len(sys.argv) > 3 else "data/source/imported-keys.tsv"

    words, scores, phonetic_keys = read_bgk(source)
    kept = io.open(existing, encoding="utf-8").read().splitlines()

    rows = list(kept)
    generated = 0
    skipped = 0
    claimed = set()
    for wid, (display, hint) in enumerate(words):
        word_keys = keys_for_word(display)
        if not word_keys:
            skipped += 1
            continue
        score = min(scores.get(wid, 1000), 9000)
        added_for_word = 0
        for key in word_keys:
            if key in claimed or key in phonetic_keys:
                continue
            claimed.add(key)
            row = key + "\t" + display + "\t" + str(score)
            if hint:
                row += "\t" + hint
            rows.append(row)
            generated += 1
            added_for_word += 1
        if added_for_word == 0:
            skipped += 1

    io.open(output, "w", encoding="utf-8", newline="\n").write("\n".join(rows) + "\n")
    print("words=%d generated=%d skipped=%d total=%d" % (len(words), generated, skipped,
                                                         len(rows)), file=sys.stderr)


if __name__ == "__main__":
    main()
