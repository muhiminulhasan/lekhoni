# Dictionary source data

`data` is split in two:

```text
data/
├── source/     # text inputs the compilers consume: dictionary TSVs, the
│               #   curated phrase and evaluation sets, third-party
│               #   dictionaries (avro/), and the raw corpus (corpus/)
└── compiled/   # binary artifacts compiled from those inputs and checked in
                #   for the build: LekhoniCore.merged.bgk, LekhoniTrigrams.dat,
                #   avro-layout.dat
```

`LekhoniCore.merged.bgk` is the default compiled core dictionary used by the build and installer. `source\starter.tsv` is the small project-authored fallback source and can be compiled with `-UseStarterDictionary`.

## Licensing status — read before redistributing

| File | License |
| --- | --- |
| `source/starter.tsv` | MIT — original work authored for this project, covered by the repository [LICENSE](../LICENSE). |
| `source/phrases.tsv` | MIT — original work authored for this project, covered by the repository [LICENSE](../LICENSE). |
| `source/eval.tsv` | MIT — original work authored for this project, covered by the repository [LICENSE](../LICENSE). |
| `compiled/LekhoniCore.merged.bgk` | GPL-2.0-or-later — compiled from the Avro Keyboard `Database.db3`. |
| `source/imported-core.tsv` | GPL-2.0-or-later, with MPL-2.0 portions — Avro db3 corpus plus 737 Lekho suffix word forms. |
| `source/imported-keys.tsv` | GPL-2.0-or-later — phonetic keys generated from the merged corpus. |
| `source/imported-autodict.tsv` | MPL-2.0 (Lekho `autocorrect.json`) with GPL-derived merges (Avro `autodict.dct`). |
| `source/imported-autodict-caps.tsv` | Same lineage as `imported-autodict.tsv` — its case-split expansion. |
| `compiled/avro-layout.dat` | GPL-2.0-or-later — compiled from the OpenBangla `avrophonetic.json`. |
| `compiled/LekhoniTrigrams.dat` | CC BY-SA 3.0 / GFDL — compiled from the Bengali Wikipedia sample. |
| `source/corpus/` | CC BY-SA 3.0 / GFDL — Bengali Wikipedia article sample. |

Every third-party-derived file is licensed under its upstream license, not
under the repository MIT grant. Redistribution is permitted under those
terms: the full license texts live in
[`third-party-licenses/`](../third-party-licenses/), the attribution and
derivation record is in [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md),
and the default release package ships both plus the source-form TSVs, so it
can be redistributed as-is. Derivation chains for each file are recorded in
the table at the bottom of this file.

For a package covered by the MIT License alone:

- build and install it with
  `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -UseStarterDictionary`
  (or `package.ps1 -UseStarterDictionary`). The starter build uses only
  `source\starter.tsv` and `source\phrases.tsv`: it skips the autocorrect
  merge into the phrase layer, the Avro layout table, and the trigram table,
  so the IME runs on its built-in rules and the static prediction hints;
- or supply your own authorized TSV with `-DictionarySource`.

## The curated phrase layer

`phrases.tsv` is a small, project-authored override table. It is compiled
separately into `artifacts\LekhoniPhrases.bgk` and loaded at runtime beside
`LekhoniCore.bgk`, so it works with **any** core dictionary and editing a
phrase never requires regenerating a large corpus.

Priority when the whole word has been typed:

```text
curated phrase  >  core dictionary  >  phonetic transliteration
```

Because phrase keys are matched case-insensitively, an **explicit capital
letter bypasses the phrase layer entirely**. That keeps deliberate
disambiguation working: `valobasi` takes the phrase `ভালোবাসি`, while
`valObasI` still yields `ভালোবাসী`.

The layer is optional. Delete `data\source\phrases.tsv` and the build simply stops
producing it; the installer then removes any stale copy from a previous
install so an outdated phrase list cannot linger.

Use it for names, brand spellings, and words the general corpus renders
differently. It is the supported way to correct a spelling without touching
the core dictionary.

## The accuracy evaluation set

`eval.tsv` is hand-checked ground truth in the same TSV shape, with a third
column selecting the metric a row joins (`exact`, `prefix`, or `case`):

```text
bangla	বাংলা	exact
bangla	বাংলা	prefix
valObasI	ভালোবাসী	case
```

It exists because every other suite in the tree is a *correctness* suite: it
checks that images are sorted, deduplicated, checksummed, and that the
documented ranking contract holds. None of them can tell you whether a change
made the IME more or less accurate. This file is what that question is
measured against, and it is deliberately independent of the corpus — measuring
the dictionary against itself would only show that it round-trips.

Measure with:

```powershell
& '.\artifacts\x64\Release\LekhoniDictionaryCompiler.exe' --score `
  '.\data\source\eval.tsv' '.\artifacts\LekhoniCore.bgk' '.\artifacts\LekhoniPhrases.bgk' `
  '.\data\compiled\avro-layout.dat'
```

`build.ps1 -RunTests` prints the same report. Four groups are reported:
`exact` types the whole key and requires the committed text to be right,
`prefix` types the key minus its last two characters (the mid-typing case),
`case` covers deliberate capitalisation, and `typo` is generated automatically
by mutating every `exact` row four ways.

Two details the tool handles for you, both of which were live errors in the first
version of this set:

- **Comparisons are canonicalised on both sides.** The engine always emits
  canonicalised Bangla, so an expectation written with the decomposed nukta form
  (`U+09A1 U+09BC`, `U+09AF U+09BC`) never compares equal to correct output.
  Nine rows were reported as unreachable when the engine had in fact put the right
  word in slot 0.
- **The Avro layout table is loaded** when you pass it, because the IME prefers it
  over the built-in rules at runtime. Without it the report scores the fallback
  path and presents it as the product.

Treat the numbers as a **delta, not a benchmark**. The set is a curated seed of
about seventy rows, which is far too small to be statistically meaningful on
its own — its value is that the same run before and after a change makes the
direction of that change visible. Only add a row when you are confident of the
Bangla spelling; a wrong row makes every metric worse while measuring nothing
real, and `shundor` is written here as a reminder of how easily that happens.


## Format

The source dictionary is UTF-8 tab-separated text. Each non-comment line has:

```text
phonetic<TAB>bangla<TAB>frequency[<TAB>prediction|list]
```

- `phonetic`: the intended Latin key, without a leading or trailing space.
- `bangla`: the preferred Unicode spelling; it must be valid UTF-8 and contain Bangla text.
- `frequency`: a non-negative 32-bit ranking weight. It represents relative importance, not corpus count.
- `prediction|list`: optional Bangla words separated by `|`. They are local next-word hints shown after the previous committed word; they are not automatically learned or sent anywhere.

Example:

```text
bangla	বাংলা	10000	ভাষা|দেশ|শব্দ
ami	আমি	10000	আমরা|আমার|আমাকে
```

The default build copies the checked-in `LekhoniCore.merged.bgk` to the canonical output name `artifacts\LekhoniCore.bgk`. To compile a different authorized TSV, pass `-DictionarySource` to `build.ps1` or `package.ps1`.

A larger corpus must record its source, version, license, and normalization procedure before release. The compiled binary dictionary is generated locally and must not be downloaded at runtime.

## The trigram next-word table

`LekhoniTrigrams.dat` (BGTG1) maps context word pairs to their most frequent
continuations. It is compiled from a sample of Bengali Wikipedia articles
(CC BY-SA 3.0/GFDL dual licensed) by `tools\build_trigrams.py`:

```text
python tools/build_trigrams.py <corpus.txt> data/compiled/LekhoniTrigrams.dat
```

It inherits the corpus license (CC BY-SA 3.0/GFDL), so redistribution must
comply with those terms. The file is optional runtime data; without
it the IME falls back to the static prediction hint lists.

## Third-party source data (removed from the tree)

The following third-party inputs were used offline to derive the third-party-licensed files
above and are intentionally **not** kept in the repository. Re-download them
from the upstream projects if a re-derivation is ever needed:

| Source file | Upstream | License | Produced |
| --- | --- | --- | --- |
| `assets/Database.db3`, `assets/autodict.dct` | github.com/mugli/Avro-Keyboard | GPL | `LekhoniCore.merged.bgk` (via the db3 merge), `data/source/imported-autodict.tsv` (via `tools/avro_autodict`) |
| `data/source/avrophonetic.json` | github.com/OpenBangla/OpenBangla-Keyboard | GPL | `data/compiled/avro-layout.dat` (via `tools/compile_avro_layout.py`) |
| `data/source/dictionary.json` | github.com/OpenBangla/OpenBangla-Keyboard | GPL | cross-check only (same word list as the db3) |
| `data/source/autocorrect.json`, `data/source/suffix.json` | github.com/ARahim3/Lekho | MPL-2.0 | `data/source/imported-autodict.tsv` (supersedes the Avro autodict, 5011 entries) and 737 suffix word forms in `data/source/imported-core.tsv` (via `tools/convert_lekho.py`) |
| upodesh `data/` | github.com/OpenBangla/upodesh | MIT | reference for the suggestion architecture; not compiled in |

Re-derivation chain, from a fresh download of those assets:

```text
python tools/compile_avro_layout.py <avrophonetic.json> data/compiled/avro-layout.dat
avro_autodict data/compiled/avro-layout.dat <autodict.dct> data/source/imported-autodict.tsv
python tools/generate_phonetic_tsv.py data/compiled/LekhoniCore.merged.bgk data/source/imported-merged.tsv data/source/imported-keys.tsv
```

The build itself needs none of the upstream files: every derived artifact it
consumes (`data/compiled/avro-layout.dat`, `data/source/imported-core.tsv`,
`data/source/imported-autodict.tsv`, `data/compiled/LekhoniCore.merged.bgk`) is checked in.
