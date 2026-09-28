# Test plan

What is automated in this repository, what is not, and why. A test type that
is listed as manual is manual for a specific reason, not by oversight.

## Suites

| Suite | Binary | What it covers |
| --- | --- | --- |
| Core unit tests | `LekhoniCoreTests.exe` | UTF-8, dictionary builder and image, transliteration rules, engine behaviour, backspace editing, phrase layer |
| Robustness and fuzz | `LekhoniRobustnessTests.exe` | Binary format attacks, structural soundness, determinism, boundaries, concurrency, conformance vectors |
| Dictionary audit | `LekhoniDictionaryAuditTests.exe` | The images this project actually ships, black-box through the public API |
| IME COM smoke | `LekhoniIMESmokeTests.exe` | TSF interface negotiation, display attribute provider, text helpers |
| End to end | `LekhoniEndToEndTests.exe` | Whole typing sessions: keystrokes, editing, commit, corrections, predictions, compatibility invariants |
| Manual integration | [`TESTING.md`](TESTING.md) | Everything that needs a live TSF host |

All five binaries build for x86 and x64 and are run by `build.ps1 -RunTests`.

## The taxonomy, and where each type lives

### Unit testing — automated
Smallest testable units in isolation: `canonicalizeBangla`, `editDistance`,
`PhoneticTransliterator`, `DictionaryBuilder::add`. No file or COM
involvement. Lives in `CoreTests.cpp`; the exhaustive sweeps in
`RobustnessTests.cpp` are unit tests at scale.

### Component testing — automated
A whole component exercised through its public interface rather than its
internals: `Dictionary` loaded from a real image and queried; `InputEngine`
driven by appending code points. `RobustnessTests.cpp` round-trips the builder
through the loader; `EndToEndTests.cpp` drives the engine as a component.

### Integration testing — partly automated, partly manual
*Automated:* the dictionary compiler produces an image, the loader validates
it, and the engine searches it. `build.ps1` also asserts the x86 and x64
dictionaries are byte-identical.
*Manual:* the IME DLL talking to ctfmon, a real application, and the
registration helper running elevated. See `TESTING.md`.

### Smoke testing — automated
The cheapest possible check that a build is alive: both executables run, the
DLL loads, `DllGetClassObject` returns a factory, and every TSF interface
answers `QueryInterface`. A failure here means stop, not investigate.

### Sanity testing — automated, and a subset of smoke
A narrower pass over the essential path: UTF-8 decodes, the dictionary builds
and loads, `bangla` becomes বাংলা, `sonar` becomes সোনার, backspace removes a
character. If this fails the build is not runnable at all. It is the first
block of `CoreTests.cpp` and the conformance vectors.

### Functional testing — automated
Specified behaviour, input to output. This is the bulk of `CoreTests.cpp` and
`EndToEndTests.cpp`: the capital vowel rules, the `jibon` stem, the anusvara
rule, the phrase priority matrix, the candidate list invariants.

### Regression testing — automated
Every bug fixed in this repository is pinned by a test so it cannot come back.
The named regressions are listed as their own section of
`tests/conformance.txt`, and the capital vowel, `kt`, `ngo`, and `jibon`
behaviours each have explicit assertions. A retest guard in
`EndToEndTests.cpp` confirms a repeated input gives a repeated answer, so an
intermittent failure cannot be mistaken for a real change.

### Retesting — automated
Re-running a failing test after a fix. The same mechanism as regression
testing; the suites are deterministic and seeded, so a failure reproduces
exactly and a fix is confirmed by re-running.

### Compatibility testing — automated, except across Windows versions
*Automated:* the core does not depend on locale, keyboard layout, or ambient
process state. `EndToEndTests.cpp` asserts two independent engines produce
identical text, that a repeated session reproduces it, that reloading the
dictionary is idempotent, and that typing still works with no dictionary
loaded. `build.ps1` asserts the x86 and x64 dictionary images are identical.
*Manual:* the same behaviour on Windows 10 and Windows 11, and in both a
32-bit and a 64-bit application, because TSF surfaces differ between hosts.
See `TESTING.md`.

### System testing — manual
The complete product on a real machine: install, register, sign out and in,
type, uninstall, confirm the profile is gone. `TESTING.md` and
`RELEASE_CHECKLIST.md`.

### End-to-end testing — automated at the engine, manual through the host
*Automated:* a keystroke stream is fed to `InputEngine`, a composition is
committed the way `TextService::finishComposition` does, and the resulting
Bangla text is compared. This covers the whole composition pipeline including
editing, corrections, and the phrase layer.
*Manual:* the same journey through a live TSF host, because the composition
range, edit sessions, and candidate window live in the IME layer and cannot be
driven without a real application. `TESTING.md`.

### Acceptance testing — manual, owner decision
Does the product meet the criteria the owner set. Not a technical test type.
The criteria are the open items in `RELEASE_CHECKLIST.md`.

### User acceptance testing (UAT) — manual, needs real users
Whether actual Bangla speakers find the typing behaviour acceptable. This
cannot be automated: it needs people typing real sentences and judging the
output. The phrase list in `data/source/phrases.tsv` exists to collect exactly this
kind of feedback.

### Exploratory testing — manual, deliberately unscripted
Deliberately not automated, because its value is in finding what nobody
thought to write down. Recommended focus areas that the automated suite does
not reach: composing in applications with rich text (Word, browsers), switching
input mid-word, IME running in a UAC prompt, and multiple monitors with
mixed DPI.

### Ad-hoc testing — manual, short and opportunistic
Quick checks without a formal plan, for example typing a sentence from memory
after a release. Useful for catching what a fixed script misses, but it is not
evidence of coverage and nothing here depends on it.

### Structural soundness testing — automated
Valid UTF-8 is a weak guarantee: a string can be perfectly valid UTF-8 and
still render as broken glyphs. `RobustnessTests.cpp` asserts three structural
properties over **every three character ASCII input** (857,375 of them) plus
20,000 longer seeded samples:

- no output ends with a lone virama
- no output opens with a dependent vowel sign
- no two dependent signs appear in a row

These are properties rather than vectors, so they catch a whole class of rule
table mistakes instead of one word at a time. They are also why a violation
prints the offending input: it is short enough to paste straight into
`tests/conformance.txt`. This found a real bug, an `oo` at the start of a word
emitting an orphan u-kar, which the vector tests could not see because no
fixture covered a word-initial `oo`.

### Dictionary auditing — automated, against the shipped artifact
`LekhoniDictionaryAuditTests.exe` loads the real BGDX files through the public
API only. Every other suite builds its own small dictionary, which is the right
way to test logic and says nothing about the bytes a user receives. The audit
covers:

- the image loads, reports no error, and has at least one form per word
- every one and two letter prefix sweep returns a well-formed list, capped at
  nine, ranked by the documented order, with no duplicates
- `predictions()` works against real words harvested from the Bangla channel
- `lookupFuzzyPhonetic` returns a known word at distance zero and recovers
  single-edit typos at distance one
- a second load of the same bytes gives identical answers
- the x86 and x64 images are byte-identical, when both are supplied

It found the most important defect in this repository's history that no other
suite could see: **the merged core dictionary carries no prediction lists at
all**, so the next-word prediction feature did nothing in a shipped build even
though every unit test passed. The engine was only ever asking the core
dictionary, and the core cannot be regenerated. Prediction data now ships in the
project-authored phrase layer, and the audit asserts that at least one shipped
image carries some, so the feature cannot silently die again.

### Malformed input on every shipped file — automated
The phrase image is a second untrusted byte buffer that ships beside the DLL and
goes through the same loader, so it gets the same adversarial treatment as the
core: truncation at intervals, single byte corruption across the header,
hostile sizes, and all-NUL content. It also asserts that a rejected phrase image
leaves the core working, because the phrase layer is optional by design and a bad
one is a degraded IME rather than a broken one.

## Coverage that is deliberately not claimed

- **No live host automation.** Every keystroke, composition, and candidate
  window assertion in this repository is either at the engine level or is a
  COM query. Actual TSF edit sessions are only covered by `TESTING.md`.
- **No sanitiser build.** The fuzz suite is written to be sanitiser friendly,
  but it is not currently compiled with ASan or UBSan. Adding a sanitiser
  configuration to the solution would be the single highest value improvement
  to the format-attack coverage.
- **No performance regression gate beyond a smoke threshold.** The end-to-end
  suite measures per-keystroke cost and fails only above 50 ms, which catches
  an order of magnitude regression and nothing finer.
- **Raw transliteration is not the same as what users get.** The conformance
  runner applies `canonicalizeBangla` so vectors describe the committed text.
  Known rule table gaps are listed at the end of `tests/conformance.txt`
  rather than being hidden, because the dictionary covers most of them.
- **The audit cannot enumerate the dictionary.** The public API exposes no word
  iteration, so the audit reaches words by prefix sweep and by a probe list. It
  is broad, not exhaustive, and it cannot prove the absence of a bad record
  that no prefix it queries happens to reach.

## Running everything

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 `
  -Configuration Release -Platform Both -RunTests
```

To run a suite on its own, from `tests\` so the conformance file is found:

```powershell
..\artifacts\x64\Release\LekhoniCoreTests.exe
..\artifacts\x64\Release\LekhoniRobustnessTests.exe
..\artifacts\x64\Release\LekhoniIMESmokeTests.exe
..\artifacts\x64\Release\LekhoniEndToEndTests.exe
..\artifacts\x64\Release\LekhoniDictionaryAuditTests.exe
```

The audit takes the images as arguments and prints what it found, which is worth
reading after any dictionary change:

```powershell
..\artifacts\x64\Release\LekhoniDictionaryAuditTests.exe `
  ..\artifacts\LekhoniCore.bgk `
  ..\artifacts\LekhoniPhrases.bgk `
  ..\artifacts\Win32\Release\LekhoniCore.bgk
```

## Adding a fixture

Append `input<TAB>expected` to `tests/conformance.txt` under the right section.
The runner canonicalizes the actual output the same way the engine does, so
the expected column is the text a user would commit. If a case is a known gap,
put it in the `KNOWN GAPS` section at the bottom with a one-line reason rather
than adding a row that silently pins wrong output.
