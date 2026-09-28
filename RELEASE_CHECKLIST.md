# Release checklist

The current ZIP is an **unsigned integration build**, not a signed public release.

Before distributing it:

- [x] Choose and add the project license. MIT in [`LICENSE`](LICENSE), covering
      code, scripts, docs, and `data\source\starter.tsv` (plus `phrases.tsv` and `eval.tsv`).
- [ ] Fill in the `[COPYRIGHT HOLDER]` placeholder in [`LICENSE`](LICENSE) before publishing.
- [ ] Build from a clean checkout with `package.ps1` (it always performs a fresh build).
- [ ] Sign `LekhoniIME.dll`, the registration helpers, and the installer/archive with the project Authenticode certificate.
- [ ] Verify signatures on every x86/x64 binary after packaging.
- [ ] Run `docs/TESTING.md` on Windows 10 and Windows 11, in 32-bit and 64-bit hosts.
- [ ] Test the release package installer end to end: `LekhoniSetup.exe --dry-run`,
      install, Win+Space selection, uninstall from Settings > Apps, and sign-out
      refresh. Confirm `install.ps1`/`uninstall.ps1` still work on the same layout.
- [ ] Test clean install, repair/upgrade, uninstall, sign-out/sign-in, and profile recovery.
- [x] Review the starter dictionary's source, prediction hints, and redistribution rights.
      It is project-authored and covered by [`LICENSE`](LICENSE).
- [ ] **Blocking:** before shipping a package with the default dictionary, confirm the
      compliance set is inside the archive: `third-party-licenses\` (GPL-2.0, MPL-2.0,
      CC BY-SA 3.0 texts), `THIRD_PARTY_NOTICES.md`, and `source\` with the four
      imported TSVs (see `data/README.md`). Build with `-UseStarterDictionary` to
      ship an MIT-only package instead.
- [ ] Re-run registration after upgrading so the display-attribute provider category is present.
- [ ] Record the final version, SHA-256 hashes, and release notes.
