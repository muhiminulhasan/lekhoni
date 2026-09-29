# Manual integration test checklist

Run this after installing on a Windows 10/11 test machine. Installation is
either `LekhoniSetup.exe` from a release package (double-click; it elevates
itself) or `install.ps1` from an elevated PowerShell session — both install
the same layout, so run the steps below against each path once.

## Language profile

1. Install (`LekhoniSetup.exe` or `install.ps1`) and sign out/in if requested.
   Before installing with the setup program, run `LekhoniSetup.exe --dry-run`
   once and confirm it reports both architectures and the dictionary.
2. Press `Win+Space` and select **Lekhoni Bangla Phonetic**.
3. Confirm the same profile works in a 64-bit Notepad, a 32-bit Notepad, File Explorer, and a browser text field.
4. Confirm the profile remains available after an upgrade, disappears after
   `uninstall.ps1` and after `LekhoniSetup.exe --uninstall` (or Settings >
   Apps), and that the entry is gone from Settings > Apps afterwards.
5. While some Lekhoni files are still loaded (type in an app, keep it open),
   run `uninstall.ps1`: confirm it reports the locked leftovers, schedules the
   `LekhoniCleanup` task, asks about signing out, and that the leftovers and
   the task are gone after the next sign-in.

## Phonetic composition

1. Type `bangla` and confirm the composing text is `বাংলা`.
2. Type `bangladesh` and confirm the composing text is `বাংলাদেশ`.
3. Type `dhaka`; confirm the preferred dictionary spelling `ঢাকা` is shown and committed.
4. Type `bangaldesh`; confirm the typo candidate `বাংলাদেশ` is offered.
5. Type `sonar`; confirm the composing text is `সোনার`, and that `mone` stays `মনে`, `kemon` stays `কেমন`, and `sonta` stays `সন্টা`.
6. Type a phrase from `data\source\phrases.tsv` such as `jiboner`; confirm `জীবনের` commits.
7. Type `valobasi` and confirm the phrase gives `ভালোবাসি`, then type `valObasI` and confirm the capital bypasses the phrase and gives `ভালোবাসী`.
8. Confirm the aspirate and conjunct keys: `chhobi` is `ছবি`, `ch` is `ছ`, `c` is `চ`, `ss` is `ষ`, `kkh` is `ক্খ`, `kk` is `ক্ক`, and `kt` is `ক্ত`.
9. Confirm the anusvara only closes a syllable: `bangla` is `বাংলা` and `bangladesh` is `বাংলাদেশ`, but `nga` is `ঙা` and `ng` alone is `ং`.
10. Confirm the active word has a subtle dotted underline where the host supports TSF display attributes; confirm it is removed on `Space`/`Enter`.
11. Use `Down`/`Up`, `Tab`/`Shift+Tab`, digits `1`–`9`, `Space`, `Enter`, `Esc`, and `Backspace`.
12. Switch between two text fields while composing; the old composition must not be written into the new field.
13. Test Caps Lock, Shift, rapid key repeat, Ctrl/Alt shortcuts, and an unmodified non-text key.

## Backspace editing

1. Type `bangladesh`; the composing text is `বাংলাদেশ` with `বাংলাদেশি` offered.
2. Press `Backspace` once; the composing text becomes `বাংলাদেস` and the candidate list updates.
3. Press `Backspace` again and then retype `sh`; the result must be `বাংলাদেশ` again, with no leftover `দ` and no duplicated characters.
4. Press `Backspace` several times in a row to shorten the word, then type again; the recomposed text must match the letters actually typed.
5. Press `Backspace` until the composition is empty; the underline and the candidate window both disappear and nothing is left in the document.
6. Type a word and press `Ctrl+Backspace`; the whole composed word must disappear, with nothing left in the document and no stale composition state.
7. With nothing being composed, press `Ctrl+Backspace`; the application's own word delete must run normally.
8. With a `Ctrl+Space` suggestion list open, press `Ctrl+Backspace`; the list must close.
9. Repeat the whole sequence in a 32-bit host and a 64-bit host.

## Bangla-prefix and next-word suggestions

1. Place existing Bangla text such as `বাংলা` at the caret.
2. Press `Ctrl+Space`.
3. Confirm the candidate window appears and `1`–`9`, arrows, `Space`, `Enter`, `Esc`, and mouse selection work.
4. Press the shortcut again after moving the caret; no stale range may be replaced.
5. Commit `bangla`, then begin another phonetic word and confirm the local hint list is available without any network access.

## Dari and doshomik keys

1. Type `ami`, press `Space`, then press the main keyboard `.`; the document
   must contain `আমি ।` — the dari (`U+0964`), not a raw ASCII dot.
2. Type `ami` and press `.` without committing; the composition shows
   `আমি।` and `Space` commits it with the underline released.
3. With `NumLock` on, press the numpad `.`; a literal dot is typed (after a
   committed word it appears immediately, inside a composition it joins the
   word as `১.` for `1` + numpad `.`).
4. Type `12.5`; the result is `১২.৫` with the doshomik between the digits.
5. With a `Ctrl+Space` suggestion list open, press `.`; the picked word is
   committed and followed by the dari.
6. Repeat step 1 in a host that showed no candidate window before; the dari
   must still appear (this path does not depend on the host's composition
   rectangle reporting).

## UI and lifecycle

1. Move the candidate window between monitors with different DPI settings.
2. Change focus while the list is visible; it must hide.
3. Minimize the owning application; no popup may remain stranded.
4. Open a read-only text control; typing must not corrupt the document.
5. Close the application during an active composition and restart it.
6. In an application where the candidate window previously never appeared
   (Bangla composed fine but no list), type a two-candidate word; the list
   must now appear, anchored to the host caret or the last known position.

## Registration diagnostics

The registration helper uses the modern TSF profile/category APIs. It does not directly edit `HKLM\\SOFTWARE\\Microsoft\\CTF`. If Windows does not show the profile, check:

- both matching-bitness COM registrations;
- the TIP category entries;
- the display-attribute provider category;
- the current user's language list;
- whether a sign-out/sign-in or TSF host restart is required.
