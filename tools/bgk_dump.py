#!/usr/bin/env python3
"""Development-time BGDX1 dictionary dumper for Lekhoni.

Reads a compiled .bgk image and prints its contents as Lekhoni TSV
(phonetic<TAB>bangla<TAB>frequency) on stdout, plus statistics on stderr.
Format reference: src/core/Dictionary.cpp (BGDX1, little-endian, fixed width).
"""

import io
import struct
import sys


def u16(data, off):
    return struct.unpack_from("<H", data, off)[0]


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def u64(data, off):
    return struct.unpack_from("<Q", data, off)[0]


def main(path):
    data = io.open(path, "rb").read()
    if data[:4] != b"BGDX":
        sys.exit("not a BGDX image")
    section_count = u32(data, 0x38)
    table_offset = u64(data, 0x28)
    sections = {}
    for i in range(section_count):
        entry = table_offset + i * 40
        sid = u32(data, entry)
        element_size = u16(data, entry + 6)
        offset = u64(data, entry + 8)
        size = u64(data, entry + 16)
        records = u64(data, entry + 24)
        sections[sid] = (element_size, offset, size, records)

    meta_id = struct.unpack("<I", b"BGM1")[0]
    form_id = struct.unpack("<I", b"FORM")[0]
    word_id = struct.unpack("<I", b"WORD")[0]
    blob_id = struct.unpack("<I", b"BLOB")[0]

    (meta_es, meta_off, meta_size, meta_records) = sections[meta_id]
    (form_es, form_off, form_size, form_records) = sections[form_id]
    (word_es, word_off, word_size, word_records) = sections[word_id]
    (blob_es, blob_off, blob_size, blob_records) = sections[blob_id]
    blob = data[blob_off:blob_off + blob_size]

    words = []
    for w in range(word_records):
        base = word_off + w * 32
        display_offset = u32(data, base)
        display_length = u32(data, base + 4)
        hint_offset = u32(data, base + 8)
        hint_length = u32(data, base + 12)
        display = blob[display_offset:display_offset + display_length].decode("utf-8")
        hint = blob[hint_offset:hint_offset + hint_length].decode("utf-8")
        words.append((display, hint))

    forms = []
    for f in range(form_records):
        base = form_off + f * 32
        wid = u32(data, base)
        key_offset = u32(data, base + 4)
        key_length = u32(data, base + 8)
        channel = data[base + 14]
        score = u32(data, base + 16)
        key = blob[key_offset:key_offset + key_length].decode("utf-8")
        forms.append((key, wid, channel, score))

    phonetic = [f for f in forms if f[2] == 1]
    bangla = [f for f in forms if f[2] == 2]
    print(
        f"words={len(words)} forms={len(forms)} "
        f"phoneticForms={len(phonetic)} banglaForms={len(bangla)}",
        file=sys.stderr,
    )

    seen = set()
    out = io.open(sys.stdout.fileno(), "w", encoding="utf-8", newline="\n")
    for key, wid, channel, score in forms:
        if channel != 1:
            continue
        display, hint = words[wid]
        if key in seen:
            continue
        seen.add(key)
        row = f"{key}\t{display}\t{score}"
        if hint:
            row += f"\t{hint}"
        out.write(row + "\n")


if __name__ == "__main__":
    main(sys.argv[1])
