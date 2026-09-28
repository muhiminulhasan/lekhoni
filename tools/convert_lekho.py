#!/usr/bin/env python3
"""Converts Lekho (macOS Avro keyboard) JSON data into Lekhoni pipeline inputs.

autocorrect.json (latin -> corrected Avro-phonetic spelling) is written in the
space-separated .dct format that tools/avro_autodict consumes.
suffix.json (latin -> Bangla word form) is written directly as Lekhoni TSV.

Development-time tool. The inputs are MPL-2.0 third-party data; the outputs
inherit that license. See data/README.md.
"""

import io
import json
import sys

PHONETIC = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'-")
# Values may additionally carry Avro's cluster breaker (`) and the visarga
# sandhi colon (mono:koShT -> মনঃকষ্ট); the layout engine consumes both.
VALUE_EXTRA = set(":`")


def is_phonetic_key(text):
    return bool(text) and all(c in PHONETIC for c in text)


def is_phonetic_value(text):
    return bool(text) and all(c in PHONETIC or c in VALUE_EXTRA for c in text)


def main(autocorrect_path, suffix_path, dct_out, suffix_tsv_out):
    ac = json.load(io.open(autocorrect_path, encoding='utf-8'))
    lines = []
    skipped_literal = 0
    skipped_charset = 0
    for key, value in ac.items():
        if not isinstance(value, str):
            continue
        if key == value:
            skipped_literal += 1
            continue
        if ' ' in key or ' ' in value or not is_phonetic_key(key) or not is_phonetic_value(value):
            skipped_charset += 1
            continue
        lines.append(key + ' ' + value)
    io.open(dct_out, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
    print(f"autocorrect: {len(ac)} entries -> {len(lines)} dct lines "
          f"(skipped {skipped_literal} literals, {skipped_charset} off-charset)")

    sfx = json.load(io.open(suffix_path, encoding='utf-8'))
    rows = []
    skipped_bangla = 0
    for key, value in sfx.items():
        if not isinstance(value, str) or not value:
            continue
        if not any(0x0980 <= ord(c) <= 0x09FF for c in value):
            skipped_bangla += 1
            continue
        if '\t' in key or '\t' in value or not is_phonetic_key(key) or key != key.strip():
            continue
        rows.append(f"{key}\t{value}\t2500")
    io.open(suffix_tsv_out, 'w', encoding='utf-8', newline='\n').write('\n'.join(rows) + '\n')
    print(f"suffix: {len(sfx)} entries -> {len(rows)} tsv rows "
          f"(skipped {skipped_bangla} without Bangla)")


if __name__ == '__main__':
    if len(sys.argv) != 5:
        sys.exit('usage: convert_lekho.py <autocorrect.json> <suffix.json> <out.dct> <out.tsv>')
    main(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
