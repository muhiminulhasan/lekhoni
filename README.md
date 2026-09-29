# Lekhoni

**Lekhoni** (লেখনী) is a free, offline Bangla phonetic keyboard for Windows. It is a
native C++17 Text Services Framework (TSF) input method that types the way
[Avro Phonetic](https://www.omicronlab.com/avro-keyboard.html) does — familiar
keystrokes, familiar results — in both **32-bit** and **64-bit** applications.

Type `bangla` and get বাংলা. Type `Dhaka` and get ঢাকা. Type `swopno` and get
স্বপ্ন. No cloud, no telemetry, no background process — one small DLL and a
local dictionary.

The project is designed around three goals:

1. **Small:** a native DLL with a statically linked MSVC runtime.
2. **Private:** no network access, telemetry, background service, or cloud suggestions.
3. **Self-contained:** no Boost, ICU, SQLite, NuGet, vcpkg, JSON library, or other third-party runtime.

> **Development status:** the current build is an **unsigned integration
> build**, suitable for personal use and testing. It has not yet been
> Authenticode-signed or validated on the full live-host matrix. See
> [Validation and release status](#validation-and-release-status).

---

## Contents

- [Features](#features)
- [How typing works](#how-typing-works)
- [Bangla quick guide](#bangla-quick-guide)
- [Keyboard controls](#keyboard-controls)
- [Requirements](#requirements)
- [Build from source](#build-from-source)
- [Create a release package](#create-a-release-package)
- [Installation](#installation)
- [Uninstallation](#uninstallation)
- [Using the keyboard](#using-the-keyboard)
- [Dictionaries and data](#dictionaries-and-data)
- [Selection memory](#selection-memory)
- [Project structure](#project-structure)
- [Troubleshooting](#troubleshooting)
- [Privacy and security](#privacy-and-security)
- [Validation and release status](#validation-and-release-status)
- [Acknowledgements](#acknowledgements)
- [License](#license)
- [Contributing](#contributing)
- [Roadmap](#roadmap)

---

## Features

### Input

- **The Avro Phonetic layout engine**: the complete 289-pattern layout,
  including its 22 context rules (`rri` → ঋ at word start, ৃ after a
  consonant), case-sensitive consonants (`Dh` → ঢ, `dh` → ধ), and the
  backtick cluster breaker (``k`l`` → কল, `kl` → ক্ল).
- Context-sensitive vowel handling: `ghor` → ঘর, `bhalO` → ভালো.
- Bangla digits, danda, visarga, taka sign, and punctuation.
- Literal escape with `\` (`\e` keeps the Latin `e`).
- Strict UTF-8 handling throughout, with no ICU dependency.

### Suggestions

- Phonetic-prefix completion from a ~123,000-word corpus.
- Whole-word overrides: the dictionary commits `dhaka` as ঢাকা even though
  the raw layout produces ধাকা.
- Juktabarna (conjunct) words are first-class: `montri` → মন্ত্রী,
  `swopno` → স্বপ্ন, `jontro` → যন্ত্র.
- A curated phrase layer (project phrases + the Avro autocorrect corpus)
  corrects whole words before anything else is consulted.
- Bounded one- or two-edit typo correction, gated so only plausible,
  well-known words can hijack a commit.
- Bangla-prefix suggestions for existing Bangla text via `Ctrl+Space`.
- Context-aware next-word prediction from a 54,000-bigram/150,000-trigram
  table trained on Bengali Wikipedia prose (trigram context with bigram
  fallback).
- Local next-word hints attached to dictionary words.
- A private selection memory that learns your picks and promotes them.
- The as-typed layout conversion always remains selectable in the candidate
  list, even when dictionary hits exist.
- Candidates render through the Windows system candidate UI (TSF candidate-list UI element) in hosts that support it - the same mechanism the built-in keyboards use - and on an owned Win32 candidate window everywhere else. No separate suggestion process.

### Windows integration

- Native TSF text service with `ITfTextInputProcessorEx`.
- Key, composition, thread-focus, and thread-manager event handling.
- TSF edit sessions with context-ownership and cross-document protection.
- Display-attribute provider for a subtle dotted composition underline.
- Candidate-list UI element publishing plus the UIElement-enabled TIP capability category.
- Modern TSF profile/category registration; separate x86/x64 COM entries.
- Bengali Bangladesh language profile (`0x0845`).
- Control Flow Guard (/guard:cf) in the shipped DLL.

### Lightweight design

- No persistent background EXE, service, driver, startup task, or tray process.
- Static MSVC runtime; the release DLLs depend only on Windows system components.
- No network client or update mechanism.
- Dictionaries are local files read beside the installed DLL.

---

## How typing works

Every keystroke passes through three layers, most-authoritative first:

```text
1. phrase layer   (LekhoniPhrases.bgk)   whole-word corrections, autocorrect
2. core dictionary (LekhoniCore.bgk)    prefix completions, exact spellings,
                                         fuzzy typo recovery, next-word hints
3. Avro layout     (AvroLayout.dat)      letter-by-letter phonetic conversion
```

When a word is fully typed and committed, an exact phrase hit wins over an
exact dictionary hit, which wins over the raw conversion. When you start the
next word, a trigram table trained on Bengali Wikipedia prose suggests what
statistically follows the word you just committed, before the static hints. While you are still
typing, the dictionary ranks completions and the memory reorders them around
your own past choices. If the layout table file is absent, the bundled
original rules take over layer 3 — a degraded but working keyboard.

---

## Bangla quick guide

Lekhoni installs a Bengali Bangladesh (`0x0845`) profile. Select
**Lekhoni Bangla Phonetic** with `Win+Space`.

| Typed | Result |
|---|---|
| `bangla` | বাংলা |
| `bangladesh` | বাংলাদেশ |
| `ami` | আমি |
| `bhalo` | ভালো |
| `Dhaka` | ঢাকা |
| `dhaka` | ঢাকা (dictionary correction) |
| `swopno` | স্বপ্ন |
| `vasha` | ভাষা |

### বাংলায় ব্যবহার

- `Win+Space` দিয়ে **Lekhoni Bangla Phonetic** নির্বাচন করুন।
- বানান লিখুন — যেমন `bangla`, `ami`, `montri` — আর বাংলা লেখা দেখুন।
- পরামর্শ দেখতে `Down`/`Up` বা `1`–`9` ব্যবহার করুন; `Space` দিয়ে লিখুন।
- পুরোনো বাংলা শব্দের পরামর্শ চাইলে caret-এর পরে `Ctrl+Space` চাপুন।
- ভুল হলে `Backspace`; পুরো শব্দ মুছতে `Ctrl+Backspace`; বাতিল করতে `Esc`।
- আপনি যে পরামর্শ বেছে নেন, Lekhoni পরেরবার সেটিকে প্রথমে দেখায়।

---

## Keyboard controls

| Key | Action |
|---|---|
| `Win+Space` | Select the Lekhoni language profile in Windows. |
| `a`–`z`, `A`–`Z` | Add phonetic input. |
| `0`–`9` | Bangla digits (inside a composition, digits select candidates only when the list is visible). |
| `Down` / `Up` | Select the next or previous suggestion. |
| `Tab` / `Shift+Tab` | Select the next or previous suggestion. |
| `1`–`9` | Commit the corresponding visible suggestion. |
| `Space` | Commit the selected result and insert a space. |
| `Enter` | Commit the current result and insert a line break. |
| `.` (main keyboard) | Dari (।) — inserted immediately after a committed word, and applied by the layout rules inside a composition (`12.5` → ১২.৫). |
| `.` (numpad, `NumLock` on) | A literal doshomik dot, always — even inside a composition. |
| `Esc` | Cancel the current composition or suggestion list. |
| `Backspace` | Remove the last typed character; the word recomposes live. |
| `Ctrl+Backspace` | Delete the whole word being composed and close the candidate window. |
| `Ctrl+Space` | Bangla-prefix suggestions for the word at the caret. |
| `` ` `` (backtick) | Avro cluster breaker: ``k`l`` → কল where `kl` → ক্ল. |
| `\` | Literal escape: `\e` keeps the Latin `e`. |

`Ctrl+Space` is reserved by Lekhoni for Bangla-prefix suggestions; it is not
the Windows language-switch shortcut. `Ctrl+Backspace` is intercepted only
while a composition or suggestion list is active; otherwise the application
handles it as usual.

---

## Requirements

### For building

- Windows 10 or Windows 11
- Visual Studio 2022 Build Tools — **Desktop development with C++**
- Windows 10/11 SDK
- PowerShell 5.1 or newer

No package manager or dependency restore is required.

### For installing

- Administrator privileges
- Windows 10/11
- The built release package or a checkout containing `artifacts\`

---

## Build from source

```powershell
Set-Location C:\path\to\lekhoni

# Clean, build both architectures, run every test suite:
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Clean -RunTests
```

`-Clean` wipes `artifacts\` first and implies `-BuildDictionary`, so the
result can never depend on stale outputs. Without `-Clean` the same command
builds incrementally.

### Options

| Switch | Meaning |
|---|---|
| `-Configuration` | `Release` (default) or `Debug` |
| `-Platform` | `Both` (default), `x64`, or `Win32` |
| `-Clean` | Remove all build output first |
| `-RunTests` | Run all five test suites, including the accuracy report |
| `-BuildDictionary` | Produce `artifacts\LekhoniCore.bgk` from the selected source |
| `-DictionarySource` | Compile this TSV instead of the default corpus |
| `-UseStarterDictionary` | Compile the small project-authored `data\source\starter.tsv`; the build also skips the derived autocorrect merge, the Avro layout table, and the trigram table, so every shipped data file is MIT-licensed |

### Dictionary sources

By default the build compiles the checked-in key-augmented corpus:

```text
data\source\imported-core.tsv   →   artifacts\LekhoniCore.bgk
```

which is `data\source\imported-keys.tsv` (one or more generated phonetic keys per
corpus word, including juktabarna variants) plus `data\source\imported-autodict.tsv`
(the Avro/Lekho autocorrect, converted through the layout engine). The
checked-in precompiled `data\compiled\LekhoniCore.merged.bgk` is the fallback when the
TSV set is absent, and `-UseStarterDictionary` builds the 211-row project
seed. See [`data/README.md`](data/README.md) for the derivation chain and the
license of each file.

### Other scripts

- `clean.ps1` — remove all build output only.
- `package.ps1` — hermetic release ZIP: `-Clean` build, all suites, package.
- `install.ps1` / `uninstall.ps1` — machine registration.

### Test suites

`-RunTests` executes, per architecture: core engine tests (including the
selection-memory and score-floor suites), the COM/TSF smoke tests, the
robustness suite (~109,000 checks: corruption fuzz, truncation, hostile
headers, UTF-8 boundaries, transliteration invariants, and the 358-vector
Avro conformance file), the end-to-end typing suite, and a black-box audit of
the exact shipped dictionary images, including cross-architecture byte-parity
checking.

---

## Create a release package

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\package.ps1
```

This always performs a hermetic `-Clean` build with tests, then produces:

```text
artifacts\Lekhoni-win-x86-x64.zip
```

Package contents (per architecture):

```text
bin\<arch>\LekhoniIME.dll
bin\<arch>\LekhoniCore.bgk
bin\<arch>\LekhoniPhrases.bgk
bin\<arch>\AvroLayout.dat
bin\<arch>\LekhoniRegistration.exe
LekhoniSetup.exe
install.ps1, uninstall.ps1, README.md, RELEASE_CHECKLIST.md, LICENSE
```

The default package contains third-party-derived data that is licensed under
its upstream licenses (GPL/MPL-2.0/CC BY-SA), not MIT — redistributing it
means meeting those terms; see [License](#license). For an MIT-only package,
pass `-UseStarterDictionary`.

---

## Installation

> **Warning:** the build is unsigned. Test on a non-production account first.
> Installation modifies machine-wide COM/TSF registration and requires
> administrator approval. Install from a quiet tree — running an install
> while another build is regenerating `artifacts\` can produce an incomplete
> installation.

### With the setup program (release packages)

The release ZIP contains `LekhoniSetup.exe` at its root. Extract the archive
completely, then double-click it: it asks for elevation itself, installs both
architectures, and adds an entry to **Settings > Apps** for removal.
`LekhoniSetup.exe --dry-run` prints everything it would do without touching
the machine, and `--uninstall` removes the installation. The setup program
is a 64-bit executable; on 32-bit Windows use `install.ps1` instead.

### With the PowerShell scripts

The scripts remain the scriptable interface and install to the exact same
layout, so either path can install and either can uninstall:

```powershell
# build first, then elevated:
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
```

From the ZIP: extract the archive completely (do not run `install.ps1` from
inside it), open an elevated PowerShell in the extracted folder, and run
`install.ps1`.

### With a custom dictionary

```powershell
.\install.ps1 -DictionaryPath .\artifacts\LekhoniCore.custom.bgk
```

### What installation does

1. Creates `%ProgramFiles%\Lekhoni\bin32` and `bin64`.
2. Copies the IME DLL, core dictionary, phrase layer, and layout table to each.
3. Registers the matching-bitness COM server for each architecture.
4. Registers the TIP categories and the Bengali Bangladesh TSF profile.
5. Best-effort enables the profile for the current user.
6. `LekhoniSetup.exe` additionally copies itself into the install directory
   and registers a Settings > Apps entry that invokes `--uninstall`.

It does **not** install a driver or service, create a startup task, start a
background process, change your default keyboard, or download anything.

### After installation

Sign out and back in if the profile is not listed, press `Win+Space`, select
**Lekhoni Bangla Phonetic**, and test in Notepad, a browser field, and a
32-bit application. Restart applications that were already running — TSF
loads the DLL per process.

---

## Uninstallation

```powershell
# elevated, from the same checkout or extracted package used to install:
powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1
```

The uninstaller reverses both architectures' registration, verifies the COM
keys are gone, removes `%ProgramFiles%\Lekhoni`, and leaves your source
checkout and `%LOCALAPPDATA%\Lekhoni` (the selection memory) untouched. Sign
out and back in afterwards.

---

## Using the keyboard

### Compose, choose, commit

Type a phonetic word — the composing text updates live in the application.
When the candidate window appears (two or more options), pick with `Up`/
`Down`/`Tab`, commit with `1`–`9` or a mouse click, or accept the highlighted
candidate with `Space`. `Backspace` rewinds one character with live
recomposition (`valObasi` → `valObas` → ভালোবাস …), `Ctrl+Backspace` deletes
the whole word, and `Esc` cancels.

### Juktabarna (conjunct) words

Conjuncts compose as you type: `montri` → মন্ত্রী, `prem` → প্রেম,
`srishti` → সৃষ্টি — no special mode. The dictionary also carries typable
keys for tens of thousands of conjunct words (`swopno`, `jontro`, `kormo`),
so the preferred spelling commits directly. If the layout joins a cluster you
did not want, the backtick splits it: `kl` → ক্ল but ``k`l`` → কল.

### Existing Bangla text

Place the caret after a Bangla word and press `Ctrl+Space`. Lekhoni reads the
bounded word before the caret, normalizes it, and suggests completions from
the dictionary — with your remembered picks ranked first.

### Case sensitivity

Capitalization is meaningful, exactly as in Avro Phonetic: `Dhaka` → ঢাকা
versus `dhaka`, and `valObasI` → ভালোবাসী versus `valobasi` → ভালোবাসি.

---

## Dictionaries and data

The runtime formats are compact, checksummed binaries:

- **BGDX1** (`.bgk`) — dictionary images: fixed-width little-endian records,
  a UTF-8 blob, section CRCs, and identity hashes. Portable across x86/x64
  and deterministic across builds. An invalid dictionary disables suggestions
  but never blocks typing.
- **BGLT1** (`AvroLayout.dat`) — the compiled Avro Phonetic layout table,
  produced from the upstream layout JSON by
  `tools\compile_avro_layout.py`.
- **BGTG1** (`LekhoniTrigrams.dat`)— the trigram next-word table,
  built by `tools\build_trigrams.py`.

Dictionary source is UTF-8 TSV:

```text
phonetic<TAB>bangla<TAB>frequency[<TAB>prediction|list]
```

The default core corpus is not maintained by hand: `tools\generate_phonetic_tsv.py`
reverse-transliterates every corpus word into the keys a user would type —
including inherent-vowel and `j`-variants for juktabarna and word-initial য —
and `tools\avro_autodict` converts the upstream autocorrect through the real
layout engine. The full derivation chain, including how to re-derive every
third-party-derived file from upstream sources, is documented in
[`data/README.md`](data/README.md).

---

## Selection memory

Lekhoni learns from you, privately. When you pick a candidate that was not
already first — by keyboard, digit, or mouse — it is recorded under the typed
key (or the Bangla prefix, for `Ctrl+Space` picks). Those words rank up on
the next keystroke.

- Stored at `%LOCALAPPDATA%\Lekhoni\selections.dat` (BGS2): a 128-bit hash of
  the key — never the key itself — plus the chosen word, a pick counter, and
  a coarse day stamp.
- Up to three words per key, ranked by count with a 14-day half-life: recent
  picks win, stale habits fade.
- 20,000 entries maximum with lowest-score eviction; atomic writes.
- Reset by deleting `selections.dat`. A corrupt file is ignored; typing never
  depends on it.

Your explicit picks also outrank phrase-layer heuristics for the same key —
the memory is treated as the strongest statement of intent you can make.

---

## Project structure

```text
lekhoni/
├── src/
│   ├── core/                     # Platform-independent engine (no Windows, no COM)
│   │   ├── AvroLayoutEngine.*    # Avro Phonetic layout table engine
│   │   ├── Dictionary.*          # BGDX1 builder, safe loader, searches
│   │   ├── InputEngine.*         # Composition state and candidate model
│   │   ├── PhoneticTransliterator.*  # Original rule engine (fallback)
│   │   ├── SelectionMemory.*     # BGS2 user pick memory
│   │   ├── Utf.*                 # Strict UTF-8
│   │   └── WordPrefix.*          # Phonetic/Bangla prefix extraction
│   └── ime/                      # Windows TSF adapter
│       ├── CandidateWindow.*     # Owned Win32 candidate UI
│       ├── ClassFactory.* / DllMain.*
│       ├── DisplayAttribute.*    # Composition underline provider
│       ├── TextService.*         # Activation, keys, edit sessions, memory I/O
│       └── WinText.*             # UTF-8/UTF-16 and normalization
├── tools/
│   ├── DictionaryCompiler/       # TSV → BGDX compiler (--audit, --score)
│   ├── Registration/             # Matching-bitness TSF registration helper
│   ├── Setup/                    # LekhoniSetup.exe native installer
│   ├── avro_autodict/            # Autocorrect → TSV through the layout engine
│   ├── compile_avro_layout.py    # Layout JSON → BGLT1
│   ├── convert_lekho.py          # Lekho JSON → pipeline inputs
│   ├── generate_phonetic_tsv.py  # Corpus → typable phonetic keys
│   └── bgk_dump.py               # BGDX1 debugging dumper
├── data/                         # Build-time data; see data/README.md
│   ├── source/                   # TSV inputs: dictionaries, phrases, eval, corpus
│   └── compiled/                 # Checked-in binaries: core bgk, trigrams, layout
├── tests/                        # Core, smoke, robustness, audit, E2E suites
├── docs/                         # ARCHITECTURE.md, TESTING.md
├── assets/                       # Icon
├── build.ps1 / clean.ps1 / package.ps1 / install.ps1 / uninstall.ps1
└── LICENSE
```

---

## Troubleshooting

### The profile is not visible in `Win+Space`

Confirm the installer completed without an error, that PowerShell was
elevated, and sign out and back in. Then check
**Settings → Time & language → Language & region**. Do not manually edit
`HKLM\SOFTWARE\Microsoft\CTF`; the registration helper uses the TSF APIs.

### Suggestions never appear (but Bangla types fine)

The candidate window only opens with two or more options. Check that all
runtime files sit beside the installed DLL:

```text
%ProgramFiles%\Lekhoni\bin64\LekhoniCore.bgk
%ProgramFiles%\Lekhoni\bin64\LekhoniPhrases.bgk
%ProgramFiles%\Lekhoni\bin64\AvroLayout.dat
```

A missing dictionary or layout table silently degrades to fewer suggestions
and the fallback rules — by design, typing never breaks. Without the layout
table, spellings revert to the built-in rules (for example `vasha` instead of
ভাষা).

Some applications cannot report the on-screen rectangle of the composition;
in those the window is placed from the application's own caret or the last
known position instead of hiding. If a host still never shows the list,
note the application name — it is a host-integration gap worth reporting.

### A 64-bit or 32-bit app does not use Lekhoni

Both architectures are registered independently. Verify the COM key for the
failing bitness exists under
`HKLM\SOFTWARE\Classes\CLSID\{6E315A4D-B7F3-4C4A-91E2-59A830447D19}`
(and `WOW6432Node` for 32-bit), and that the matching `bin` folder is
complete. If in doubt, re-run `install.ps1` elevated and restart the app.

### Access denied / uninstall fails

Run PowerShell as administrator, close apps that may have loaded the DLL, and
use the same checkout or package that installed it. Do not delete TSF
registry keys manually.

---

## Privacy and security

- No network access, no telemetry, no companion process, no update channel.
- Dictionaries, the phrase layer, and the layout table are read-only files
  beside the installed DLL.
- The selection memory stores a hash of the typed key plus the chosen word —
  never documents, sentences, or key sequences — under `%LOCALAPPDATA%\Lekhoni`,
  and never leaves the machine. Delete `selections.dat` to reset.
- Registration changes machine-wide COM/TSF state and requires administrator
  approval.
- Current binaries are not Authenticode-signed.

Only Windows operating-system libraries are linked; no third-party DLL ships
in the package.

---

## Validation and release status

Automated validation covers: strict UTF-8 decode/encode; Bangla code-point
detection; layout and rule conversion (including the 358-vector Avro
conformance file); composition flows; backspace/Ctrl+Backspace editing; the
phrase layer and prediction hints; phonetic and Bangla-channel lookups; exact
ranking, canonical deduplication, and the gated fuzzy-typo path; the
selection memory (ranking, decay, eviction, BGS1 migration, corruption);
dictionary checksum and per-byte corruption rejection; x86 and x64 execution
with cross-architecture byte parity; COM loading and interface queries; and
an accuracy report over the hand-checked pairs in `data\source\eval.tsv`.

The accuracy report is the only check that can tell whether a change made
the IME *more* accurate rather than merely still correct. Current figures on
the shipped corpus, as a baseline rather than a benchmark:

```text
exact    n=58   top-1 96.55%   top-5 98.28%   MRR 0.9766
case     n=5    top-1 100.00%  top-5 100.00%  MRR 1.0000
typo     n=207  top-1 39.13%   top-5 76.81%   MRR 0.5451
prefix   n=8    top-1  0.00%   top-5 62.50%   MRR 0.2783
```

Mid-word (`prefix`), the first slot is whichever completion the corpus ranks
highest — read its top-5 and MRR instead. `typo` has the most headroom.

### Open release gates

- [ ] Fill in the `[COPYRIGHT HOLDER]` placeholder in [`LICENSE`](LICENSE).
- [ ] Sign all release binaries with an Authenticode certificate.
- [ ] Run [`docs/TESTING.md`](docs/TESTING.md) across Windows 10/11 hosts, 32- and 64-bit.
- [ ] Test clean install, upgrade, uninstall, and profile recovery.
- [ ] Meet the upstream license obligations (GPL/MPL-2.0/CC BY-SA texts, attribution, source availability) for the third-party-derived data (below), or ship `-UseStarterDictionary`.

---

## Acknowledgements

### ওমিক্রনল্যাব — ধন্যবাদ

**Special thanks to [OmicronLab](https://www.omicronlab.com/), the pioneer of
free Bangla phonetic computing.** Avro Keyboard proved, more than two decades
ago, that typing Bangla could be effortless, free, and yours — and an entire
generation of Bengali computer users learned to type on it. Lekhoni exists
because that groundwork existed: its typing system is the one Lekhoni brings
to Windows TSF, and its dictionaries and autocorrect seeded the corpus behind
these suggestions. This project stands on the shoulders of that pioneer, and
we are grateful.

Lekhoni also thanks:

- **[OpenBangla Keyboard](https://openbangla.github.io/)** — whose
  distribution of the Avro Phonetic layout specification powers Lekhoni's
  layout engine, and whose `riti`/`upodesh` work charted the
  dictionary-backed suggestion architecture.
- **[Lekho](https://github.com/ARahim3/Lekho)** — a native macOS Avro
  keyboard (MPL-2.0) whose expanded autocorrect and suffix word forms are
  merged into the suggestion corpus.
- **[BornoTSF](https://github.com/JayedAhsan/BornoTSF/)** — an early native
  TSF reference for Bangla on Windows.

These projects were used as data sources and behavioral references. Their
code was not copied; see [License](#license) for what that means for the
derived data files.

---

## License

The source code, scripts, documentation, and `data\source\starter.tsv` (plus
`phrases.tsv` and `eval.tsv`) are released
under the **MIT License**. See [`LICENSE`](LICENSE).

**The third-party-derived data files are the exception.**
`data\compiled\LekhoniCore.merged.bgk`, `data\source\imported-core.tsv`,
`data\source\imported-keys.tsv`, `data\source\imported-autodict.tsv`, and
`data\compiled\avro-layout.dat` derive from third-party sources (Avro Keyboard
and OpenBangla data under the GPL; Lekho data under MPL-2.0), and
`data\compiled\LekhoniTrigrams.dat` plus `data\source\corpus\` derive from
Bengali Wikipedia (CC BY-SA 3.0/GFDL). They are **not covered by MIT** and are
licensed under their upstream licenses: redistributing them requires shipping
the upstream license text, attribution and share-alike notices where required,
and the corresponding source data where the license requires it. A package
covered by the MIT License alone builds with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\package.ps1 -UseStarterDictionary
```

The derivation tooling in `tools\` and the provenance record in
[`data/README.md`](data/README.md) document exactly how every third-party-derived
file is produced, so the corpus can be rebuilt from any sources you are authorized
to use. Contributions are accepted under the MIT License; contributed corpora must
document source, version, license, and normalization.

---

## Contributing

Contributions are welcome — phonetic rule coverage and regression cases,
clean-license Bangla corpora, Windows TSF compatibility fixes, candidate-UI
accessibility and DPI work, x86/x64 testing, documentation and localization.

Before submitting: keep the runtime dependency-free, add or update a
regression test, build both architectures with `-RunTests`, do not add
network access to the runtime DLL, and document the source and license of any
external data.

---

## Roadmap

- Larger, properly licensed Bangla corpora and a clear redistribution path.
- Selection-memory refinements: prefix-length generalization and guarded
  out-of-vocabulary promotion.
- Richer next-word prediction beyond local hint lists and bigram learning.
- Settings UI; improved UI Automation and accessibility.
- TSF UIElement (UILess) candidates for immersive hosts; DPI hardening.
- Signed release automation; Bengali India profile option.
