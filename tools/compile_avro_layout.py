#!/usr/bin/env python3
"""Compiles the Avro Phonetic layout JSON into Lekhoni's binary BGLT1 table.

The JSON is GPL-licensed third-party data (OpenBangla/Avro), so the compiled
table inherits the GPL and is loaded at runtime from beside the IME DLL.
See data/README.md.

Table layout (little-endian):
  u32  magic 'BGLT'
  u16  version = 1
  u16  reserved = 0
  u32  pattern count
  u8   caseSet length, bytes (letters matched case-sensitively)
  u8   vowelSet length, bytes
  u8   consonantSet length, bytes
  u8   numberSet length, bytes
  per pattern, in source order:
    u16 findLen, find bytes (UTF-8)
    u16 replaceLen, replace bytes
    u16 ruleCount
    per rule:
      u16 replaceLen, replace bytes
      u16 matchCount
      per match:
        u8  type (0 = prefix, 1 = suffix)
        u8  negated (0/1)
        u8  scope (0 = vowel, 1 = consonant, 2 = punctuation,
                   3 = number, 4 = exact)
        u8  valueLen, value bytes (exact scope only)
  u64  fnv1a64 hash of all preceding bytes
"""

import io
import json
import struct
import sys

MAGIC = 0x544C4742  # 'BGLT' little-endian
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3


def fnv1a64(data):
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def pack_str16(text):
    raw = text.encode("utf-8")
    if len(raw) > 0xFFFF:
        raise ValueError("string too long")
    return struct.pack("<H", len(raw)) + raw


def main(src, dst):
    doc = json.load(io.open(src, encoding="utf-8"))
    layout = doc["layout"]
    patterns = layout["patterns"]
    case_set = layout.get("casesensitive", "")
    vowel_set = layout.get("vowel", "aeiou")
    consonant_set = layout.get("consonant", "")
    number_set = layout.get("number", "1234567890")

    seen = set()
    out = io.BytesIO()
    out.write(struct.pack("<IHHI", MAGIC, 1, 0, len(patterns)))
    for text in (case_set, vowel_set, consonant_set, number_set):
        raw = text.encode("utf-8")
        if len(raw) > 0xFF:
            raise ValueError("set too long")
        out.write(struct.pack("<B", len(raw)) + raw)

    for p in patterns:
        find = p["find"]
        replace = p["replace"]
        if not find:
            raise ValueError("empty find")
        seen.add(find)
        out.write(pack_str16(find))
        out.write(pack_str16(replace))
        rules = p.get("rules", [])
        out.write(struct.pack("<H", len(rules)))
        for rule in rules:
            out.write(pack_str16(rule["replace"]))
            matches = rule.get("matches", [])
            out.write(struct.pack("<H", len(matches)))
            for m in matches:
                mtype = {"prefix": 0, "suffix": 1}[m["type"]]
                scope_text = m["scope"]
                negated = scope_text.startswith("!")
                if negated:
                    scope_text = scope_text[1:]
                if scope_text == "exact":
                    kind = 4
                elif scope_text == "vowel":
                    kind = 0
                elif scope_text == "consonant":
                    kind = 1
                elif scope_text == "punctuation":
                    kind = 2
                elif scope_text == "number":
                    kind = 3
                else:
                    raise ValueError(f"unknown scope {scope_text}")
                value = m.get("value", "")
                if kind == 4 and not value:
                    raise ValueError("exact scope without value")
                out.write(struct.pack("<BBBB", mtype, 1 if negated else 0, kind, len(value.encode("utf-8"))))
                if value:
                    out.write(value.encode("utf-8"))

    body = out.getvalue()
    with open(dst, "wb") as f:
        f.write(body)
        f.write(struct.pack("<Q", fnv1a64(body)))
    print(f"patterns={len(patterns)} uniqueFinds={len(seen)} bytes={len(body) + 8} -> {dst}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: compile_avro_layout.py <avrophonetic.json> <output.dat>")
    main(sys.argv[1], sys.argv[2])
