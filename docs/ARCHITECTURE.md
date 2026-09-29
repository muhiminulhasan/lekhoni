# Lekhoni architecture

## Goals

Lekhoni is designed around these constraints:

1. Native Windows integration through TSF.
2. No persistent companion EXE.
3. No third-party runtime or package-manager dependency.
4. Identical source builds for x86 and x64.
5. Fully offline operation and no typing telemetry.
6. Small release artifacts and bounded memory use.

The initial size target is a release DLL below 1 MB per architecture, excluding the optional dictionary. A compact starter dictionary should remain below 500 KB. These are budgets, not guarantees, and CI should fail if size grows unexpectedly.

## Build and hardening settings

These are set once in `Directory.Build.props` and apply to every project:

| Setting | Value | Reason |
| --- | --- | --- |
| `RuntimeLibrary` | `MultiThreaded` (static CRT) | Satisfies "no third-party runtime". The TIP is loaded into every GUI process that uses the profile, so this costs some per-process footprint; that is the deliberate trade for never requiring a redistributable. |
| `WarningLevel` / `TreatWarningAsError` | `Level4` / true | Keeps the x86 and x64 builds warning-free. |
| `SDLCheck`, `ConformanceMode`, `/utf-8` | on | Buffer and conformance hardening; `/utf-8` avoids locale-dependent source parsing. |
| `ControlFlowGuard` | `guard` | The DLL is mapped into untrusted processes, so an indirect-call hijack should not be exploitable. `/guard:cf` ships with the base toolset. |
| `SpectreMitigation` | off by default | Requires the optional "Spectre-mitigated libs" component. Enable with `-DLekhoniSpectreMitigations=true` on a machine that has it. |
| `GenerateDebugInformation` | `true` in Release | Release builds emit PDBs so shipped crashes can be triaged. |

Release binaries are still unsigned; see `RELEASE_CHECKLIST.md`.

## Layers

```text
Windows application
        |
Windows TSF host (ctfmon)
        |
Lekhoni IME DLL
  - key event handling
  - composition and edit sessions
  - candidate window
        |
Lekhoni core
  - phonetic transliteration
  - dictionary lookup and ranking
  - local user-frequency model
        |
versioned read-only dictionary + local user data
```

### Core

The core is platform-independent C++17. It owns all conversion, normalization, ranking, and binary parsing behavior. It must remain usable in a console test executable without loading Windows or COM.

### Windows adapter

The adapter owns only Windows-specific concerns:

- COM and TSF registration through the matching-bitness registry view;
- `ITfTextInputProcessorEx`, key, composition, thread-focus, and thread-manager events;
- edit sessions and context ownership;
- candidate-window placement and owned-window lifetime;
- binary dictionary loading from the DLL directory;
- optional TSF display attributes for the active composition;
- per-user storage paths in future revisions.

The adapter must not contain phonetic or ranking rules. The candidate list is published twice: as a TSF candidate-list UI element for hosts that render the system candidate UI (Chromium, UWP, immersive surfaces), and on an owned Win32 candidate window for hosts that decline it. The UI element capability category is registered.

## Suggestion flow

For a Latin phonetic composition:

1. Keep the unmodified typed prefix as the source key.
2. Convert the complete prefix to Bangla for the direct candidate.
3. Normalize phonetic dictionary keys to lowercase ASCII for prefix lookup.
4. Merge static frequency, user frequency, and a bounded recency score.
5. Remove duplicate Bangla words after canonicalizing precomposed/decomposed Bengali letters.
6. Prefer an exact reviewed dictionary spelling over a different direct conversion.
7. If there is no prefix hit, try a bounded one- or two-edit phonetic correction.
8. Add the local prediction hints attached to the previously committed word.
9. Keep the direct/canonical conversion first unless a user selection has already been made.
10. Limit the UI to the first nine results.

For an already committed Bangla word, normalize only the current word at the caret and perform a Unicode-code-point prefix lookup. No locale-dependent operations are allowed because Windows does not provide a Bangla collation locale by default.

## Dictionary safety

The starter compiler emits the fixed-width `BGDX1` format. It contains a 96-byte header, a small section directory, and `META`, `FORM`, `WORD`, and UTF-8 `BLOB` sections. All integers are little-endian; no native `size_t`, pointer, `wchar_t`, or compiler structure is serialized. The same image is loaded by x86 and x64. The current UTF/normalization contract is version 2 and canonicalizes the common Bengali precomposed/decomposed letter pairs.

A dictionary is accepted only when all of these checks pass:

- exact magic bytes;
- supported format version;
- declared header size and exact file size;
- header and section-table CRCs;
- checked section ranges, counts, and element sizes;
- non-overlapping, aligned sections;
- valid record IDs and BLOB offsets;
- valid UTF-8 without embedded NULs;
- sorted form records;
- parser/normalization version hashes and dictionary identity.

Malformed or unsupported dictionaries disable suggestions but never block typing. There are no raw pointers into an unvalidated file. Lookup uses binary search plus a bounded top-N scan; the current starter format intentionally omits a serialized trie to keep the implementation small.

## Privacy and local learning

User frequency records should be stored under `%LOCALAPPDATA%\\Lekhoni`. They must contain only a hash of the normalized phonetic prefix, the Bangla word, a counter, and a coarse recency value. Raw documents and sentences are never stored.

Writes should be atomic: write a temporary file, flush it, then replace the prior file. A compact binary format is preferred over JSON or SQLite.

## Threading

TSF may call the text service on application UI threads. Candidate-window operations therefore occur only on the thread that created the window. The read-only dictionary is immutable after loading. User-frequency updates are short critical sections and file writes are deferred to avoid blocking key handling.

## Supply-chain boundary

Only Microsoft compiler headers, the Windows SDK, and the C++ standard library are permitted. The repository must not add NuGet, vcpkg, Boost, ICU, SQLite, JSON, or generated third-party code. Every binary dictionary input must have a recorded source and license.
