# M10 draft — notes for Shane's proofread (not part of the PR)

Staged updates for the `mister-disc` MultiDatabases entry, drafted against
the live entry fetched 2026-07-30. Nothing has been PR'd anywhere.

## Decisions baked into this draft — veto any of them

1. **Binary destinations renamed to the release names**:
   `MiSTer_Disc` → `MiSTer-disc`, `MiSTer_Disc_RA` → `MiSTer-disc-RA`.
   Consistent with the no-rename convention and the method-1 recipe, BUT
   it breaks existing entry users' ini lines until they update them
   (fail-safe break: their sessions silently run stock). The README's
   "Updating from the first release" section carries the migration note.
   The entry is days old, so the blast radius is minimal — but this is
   the one decision that inconveniences real users. Alternative: keep the
   old placed names forever and diverge from the upstream docs.
2. **MGL folder** `_Disc Cores/` → `_Disc_Cores/` (downloader removes the
   old files automatically; empty old folder may linger).
3. **Asset regexes anchored** (`^MiSTer-disc$`): correction after
   checking the sibling session's notes - `matching_release_asset` uses
   exact-name FULLMATCH, so the unanchored patterns were already safe
   with four release assets. The anchors are kept as harmless
   documentation of intent, not a bug fix.
4. **Translate payload vendored, not zipped**: four files under
   `payload/` in the entry (same pattern as the vendored `mgl/`), mapped
   to SD-root paths. `translate.ini` is deliberately NOT distributed
   (user-owned, holds the API key; the downloader would overwrite user
   edits on every update) — `translate_start.sh` now bootstraps a
   template ini on first run instead (wrapper commit, ships from v0.7.0's
   files onward... NOTE: the v0.7.0 release zip predates the bootstrap;
   the PR must vendor the CURRENT wrapper files, which include it).
   `hotkey.cfg` is also not distributed (SetTranslateHotkey writes it).
5. **README restructure**: two integration ways (full = `MAIN=MiSTer-disc`
   recommended, scoped = `[CD-*]`), translation setup section, RA via
   `[RA_*]` with the Companion copy-over as the legacy alternative.

## PR checklist (after proofread)

The delivery vehicle is the EXISTING clone at
`C:/Users/shane/dev/MultiDatabases_MiSTer` (fork remote, shanemain key;
PR #1 merged from branch add-mister-disc) - new branch off updated main,
same flow as PR #1.

- [ ] Copy `generate_db.py` and `README.md` from here over the entry.
- [ ] Create `mister-disc/payload/translate/` with translate_daemon.py,
      translate_start.sh, README.md and `mister-disc/payload/Scripts/`
      with SetTranslateHotkey.sh — copied from `translate/` in the
      wrapper repo AT CURRENT MAIN (includes the ini bootstrap; the
      v0.7.0 release zip predates it).
- [ ] `mister-disc/mgl/` vendored files: unchanged, no action.
- [ ] **Update `.github/test_entry_generators.py`**: the 5 mister-disc
      tests written for PR #1 assert the OLD placed paths (MiSTer_Disc,
      MiSTer_Disc_RA, `_Disc Cores/`) and will fail against the rename;
      they also need cases for the payload/ files.
- [ ] Root README table row: consider appending "+ on-the-fly
      translation" to the mister-disc description.
- [ ] Run `py .github/test_entry_generators.py` in the clone (was 25/25
      at PR #1) before pushing.
