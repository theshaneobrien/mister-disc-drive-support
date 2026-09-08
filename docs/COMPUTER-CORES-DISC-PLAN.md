# Physical discs on the IDE-family cores — work order

Targets: **AO486**, **CD32**, **CDTV**, **Amiga (Gayle IDE)**.

Written 2026-09-08, against Main 20260907 (fork merge `fc44c31`). Supersedes the
"AO486 CD = the 7th CD integration" scoping from 2026-07-22, which assumed AO486
was a lone target.

---

## 1. Why this is now one job, not four

The 20260907 sync brought in MiSTer-devel's own native CD32 (Akiko, #1258), CDTV,
and Mac CD-ROM support. The decisive detail is that **the two new Amiga CD files
carry no readers of their own.**

`support/minimig/akiko_cd32.cpp` and `support/minimig/cdtv_cd.cpp` call:

- `cdrom_read_raw_sector(drive_t*, lba, buf)` — `ide_cdrom.cpp:1105`
- `cdrom_read_track_raw(track_t*, lba, buf, buflen)` — `ide_cdrom.cpp:1202`

over the **same `drive_t` / `track_t` from `ide.h:58,72`** that AO486 uses. And
`ide.cpp:1120` gates the generic IDE path on `!is_minimig() || (ide_cfg & 1 &&
hardfile[unit].cfg)` — so plain Amiga on Gayle IDE reaches `cdrom_parse()` at
`ide.cpp:1142` as well.

So four cores share two read functions and two near-identical mount parsers:

| Target | Mount entry | Read path |
|---|---|---|
| AO486 | `cdrom_parse()` via `support/x86/x86.cpp:485` | shared |
| Amiga (Gayle IDE) | `cdrom_parse()` via `ide.cpp:1142` | shared |
| CD32 | `cd_drive_parse()` via `minimig_config.cpp:991` | shared |
| CDTV | same, slot 1 | shared |

`cdrom_parse()` (`ide_cdrom.cpp:1787`) and `cd_drive_parse()`
(`ide_cdrom.cpp:1843`) are near-duplicate twins — one indexes `ide_inst`, the
other takes a `drive_t*` directly. Both do: same-path check → close chd+tracks →
`getFullPath()` → `load_chd_file` → `load_cue_file` → `load_iso_file`.

**Consequence: the TOC adapter — the one novel piece versus the console cores —
is written once and serves all four.** That is what makes this worth doing as a
block rather than one core at a time.

## 2. Gateware: verified, not a blocker

The open question from July was whether a *released* Minimig RBF exposes the
Akiko/CDTV CD channels. It does:

- `Implement CD32 and CDTV.` landed in Minimig-AGA_MiSTer on 2026-08-23, plus
  `Akiko: PIO command register, and DRIVEXMIT for zero-result commands (#229)`.
- Released RBFs `Minimig_20260823.rbf` and `Minimig_20260907.rbf` both postdate it.
- RTL present: `rtl/akiko.v`, `akiko_hps_bridge.v`, `akiko_nvram.v`,
  `cdtv_bridge.v`, `cdtv_nvram.v`, with simulation testbenches.
- `hps_ext.v` decodes the exact ext-bus contract our ARM side uses: `0xF4xx` base
  with sub-selects for `0xF500` sector, `0xF440` NVRAM, `0xF410` subcode, and
  status command `0x63`.

Unlike RetroAchievements, **no patched core is required.** Stock released Minimig
is sufficient.

## 3. The shared foundation (do this first)

### 3a. `physcd_attach_drive(drive_t *drv)` — the TOC adapter

Fill from physcd's `toc_t` into `drive_t`:

- `drv->track[i]`: `start`, `length`, `skip`, `sectorSize`, `attr`, `mode2`,
  `number`, `chd_offset`
- `drv->track_cnt`, `drv->data_num`, `drv->cd = 1`

`read_toc` / `get_subchan` / `mode_sense` / `play_audio` then consume it
unchanged. Mind the boundary convention — decide inclusive vs exclusive `end`
against `get_track_from_lba()` and write it down, the way `apply_phys_bias()`
documents it for the console `toc_t`. This is where an off-by-one will hide.

### 3b. Sentinel handling in both parsers

`getFullPath()` fails on the non-file sentinel, so the branch must come **before**
it in both `cdrom_parse()` and `cd_drive_parse()`. Add `load_phys` alongside the
chd/cue/iso ladder.

`ide_img_mount()` (`ide.cpp:94`) already returns 1 for `.chd` **without opening a
file** — that is the precedent for making `present` true with no backing file.
Reuse that shape rather than inventing one.

### 3c. Read-side branches

- `cdrom_read_raw_sector()` — has the `drive_t`, so a `drv->phys` branch at the
  top is straightforward.
- `cdrom_read_track_raw()` — **takes only a `track_t`** and early-returns on
  `!track->f.opened()`. There is no drive to test. Add `uint8_t phys` to
  `track_t`, mirroring the `toc_t.phys` the fork already added to `cd.h`.

**CDDA byteswap is caller-side and must stay that way.** `cdtv_cd.cpp` byteswaps
after reading; `akiko_cd32.cpp:967` does not; AO486 is little-endian no-swap. Our
branch returns raw bytes and every existing caller's swap logic keeps working. Do
not put swap logic in the phys branch.

## 4. Per-target work on top of the foundation

### AO486 — live drive as `D:`

Smallest delta once the foundation lands. Live-drive model, no autoboot (a DOS
data disc returns `PHYSCD_DISC_UNKNOWN` and autoboot correctly refuses it). Needs
`OAKCDROM.SYS` + `MSCDEX` on the guest. Testable immediately with existing DOS
CDs — **use this to validate the foundation before touching Amiga.**

### CD32 — the headline feature

A console, so autoboot applies: insert disc → load Minimig with the CD32 config →
mount. Work items beyond the foundation:

1. **Disc identification.** `physcd_identify` must recognise CD32 media and route
   it. Currently returns `PHYSCD_DISC_UNKNOWN`.
2. **Autoboot ladder entry.** `physcd_mount_current_core`
   (`physcd_autoboot.cpp:131-170`) is a hardcoded if/else over the six console
   cores; Minimig is not in it. Needs the CD32 *config* selected, not just the
   core loaded.
3. **The NVRAM save path** (reassessed 2026-09-08, milder than first written).
   `akiko_cd32_set_cd_path()` (`akiko_cd32.cpp:1292`) derives a per-game NVRAM
   save from the CD path via `compute_save_path()`, which hashes only the
   **basename**. The sentinel is a fixed string, so it hashes to a stable
   `cd32-<hash>.nvr` — every physical disc shares one NVRAM, which is exactly
   the behaviour we want. So this is not the cdi.cpp bug repeating: it degrades
   gracefully rather than corrupting. Two things still worth doing when CD32 is
   wired: make the shared-NVRAM choice explicit rather than accidental, and skip
   the `open()` + `posix_fadvise` that `set_cd_path` performs on what is not a
   file (it fails harmlessly today, guarded by the `fd >= 0` check).
   Still check every new core for the per-game-save-from-path pattern.

### CDTV — nearly free after CD32

Same seam, same `drive_t`, slot 1 instead of 0. Needs its own identify branch and
the same save-path guard in `cdtv_cd_set_cd_path()` (`cdtv_cd.cpp:1100`). Upstream
already handles guest-visible swap (STCH / removal-edge commits in the
20260823–20260907 range), so the watcher only has to drive it.

### Amiga (Gayle IDE ATAPI) — cheap code, weak UX

Essentially free once the foundation exists, but be honest about it:

- Mounts through the **HDD slot**, not a CD menu — the user picks the sentinel
  from a hardfile slot, which reads oddly.
- Requires `ide_cfg & 1` and a configured hardfile slot (`ide.cpp:1120`).
- Needs CDFS software on the Amiga side (AmiCDROM / IDEfix / CacheCDFS).

Ship as a documented capability, not a headline. Do not build UI for it.

## 5. Recommended order

1. **Shared foundation** (§3) — the bulk of the work.
2. **AO486** — validates the foundation against hardware testable today.
3. **CD32** — highest user value; identify + autoboot + NVRAM guard.
4. **CDTV** — small increment on CD32.
5. **Amiga IDE** — document what already works; no dedicated UI.

Do not start at CD32. AO486 proves the shared layer with the fewest moving parts,
and every bug found there is a bug not debugged through the Akiko bridge.

## 6. Risks and open questions

- **Cold-drive first-seek latency vs ATAPI timeouts.** DOS/MSCDEX enforces a
  timeout the console cores never applied. Verify the prewarm/seek-hint
  mitigation carries to the IDE path — the most likely source of a "drive not
  ready" failure on first access.
- **Realtime blocking.** Same concern as the console path: a seek miss must not
  stall the main loop.
- **Track-boundary convention** in the adapter (§3a) — get it written down.
- **Physical hot-swap for AO486** is unwired: the menu remount sets `mcr_flag`
  (ATAPI Unit Attention), but a swap *at the drive* does not. Later add.
- **CD32 multi-disc titles** are rare; confirm whether swap is worth wiring
  beyond what upstream already does.
- **Open question:** should CD32 physical discs get autoboot by default, given
  Minimig needs a config (CD32 vs CDTV vs plain Amiga) selected and not just a
  core loaded? This is the one genuinely new autoboot shape — every existing
  console core is one RBF, one mode.

## 7. Verification

Per target: boot from a real pressed disc, one data read path, one CDDA/audio
path, one eject/insert cycle. Then the standing fork checks — clean `-Clean`
build with our files warning-free, physcd marker strings present, no RA strings
on v1, and a `-testN` tag on gitea before any release.

CD32 and CDTV additionally need the NVRAM save to survive a physical-disc session
without creating a junk save file.
