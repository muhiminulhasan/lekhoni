# Third-party notices

Lekhoni's source code is MIT-licensed (see [LICENSE](LICENSE)). Some data
files in this repository and in the release packages derive from third-party
works and are redistributed under their upstream licenses. The full license
texts are in [`third-party-licenses/`](third-party-licenses/). This file
records what was used, from where, under which license, and what was done to
it. Keep this file, the license texts, and the `source\` data folder with any
redistribution of the derived files.

## Avro Keyboard — GPL-2.0-or-later

- Upstream: <https://github.com/mugli/Avro-Keyboard> (GPL-2.0-or-later)
- Used: `Database.db3` (word frequency corpus), `autodict.dct` (autocorrect
  dictionary)
- Produced:
  - `data\compiled\LekhoniCore.merged.bgk` — the wordlist and frequency data
    of `Database.db3`, merged with autocorrect-derived corrections and
    recompiled into Lekhoni's binary BGDX format by `tools\`
    (see [data/README.md](data/README.md) for the full derivation chain).
  - `data\source\imported-keys.tsv` — phonetic keys generated from that
    corpus.
  - `data\source\imported-autodict.tsv` — Avro autocorrect corrections
    merged into the Lekho-derived table.
- License: GPL-2.0-or-later ([text](third-party-licenses/GPL-2.0.txt)). The
  source form shipped for these files is the TSV set in `source\` plus the
  derivation tooling in `tools\` of this repository.

## OpenBangla Keyboard — GPL-2.0-or-later

- Upstream: <https://github.com/OpenBangla/OpenBangla-Keyboard> (GPL-3.0)
- Used: `avrophonetic.json` (the Avro Phonetic layout rules; originally from
  the Avro Keyboard project)
- Produced: `data\compiled\avro-layout.dat` — the layout rules compiled into
  Lekhoni's binary BGLT1 format by `tools\compile_avro_layout.py`.
- License: GPL ([text](third-party-licenses/GPL-2.0.txt)). The source form
  is `tools\compile_avro_layout.py` applied to the upstream JSON, documented
  in [data/README.md](data/README.md).

## Lekho — MPL-2.0

- Upstream: <https://github.com/ARahim3/Lekho> (MPL-2.0)
- Used: `autocorrect.json` (latin → corrected Avro-phonetic spelling),
  `suffix.json` (latin → Bangla word form)
- Produced:
  - `data\source\imported-autodict.tsv` — the autocorrect conversions
    (5011 entries; supersedes the Avro-derived version, which is later
    merged back in as corrections).
  - `data\source\imported-autodict-caps.tsv` — the case-class split of the
    same table.
  - 737 suffix word forms merged into `data\source\imported-core.tsv`.
- License: MPL-2.0 ([text](third-party-licenses/MPL-2.0.txt)). These files
  remain available under MPL-2.0; the rest of the package is unaffected.

## Bengali Wikipedia — CC BY-SA 3.0 / GFDL

- Upstream: Bengali Wikipedia article text
  (<https://bn.wikipedia.org>, dual-licensed CC BY-SA 3.0 Unported / GFDL)
- Used: a sample of article prose (`data\source\corpus\bn-wiki-sample.txt`)
- Produced: `data\compiled\LekhoniTrigrams.dat` — a context-pair →
  continuation frequency table compiled from that sample by
  `tools\build_trigrams.py`.
- License: redistributed under CC BY-SA 3.0 Unported
  ([text](third-party-licenses/CC-BY-SA-3.0.txt)). Contains text from
  Bengali Wikipedia contributors. The derived trigram table is an
  adaptation and remains under CC BY-SA 3.0 (share-alike); it may not be
  relicensed as MIT.
