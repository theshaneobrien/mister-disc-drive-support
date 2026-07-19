# physcd fork integration map

verified against Main_MiSTer master, 2026-07-19. all line refs approximate.

## already done in the fork (branch physcd, commit 1afcd2f, 2026-07-19)

- `cd.h`: added `int phys;` to `toc_t` (zero-init keeps cue/chd paths untouched)
- `support/physcd/mister_physcd.{h,cpp}`: backend (toc build, sg_io raw reads
  with per-track-type flags 0xF8/0x10, CDROMREADRAW fallback, prefetch cache
  with subchannel probe, disc fingerprint). compiles clean, -Wall -Wextra.
- `Makefile`: NO edit needed - `$(wildcard ./support/*/*.cpp)` picks it up.
- megacd section below: FULLY APPLIED (sentinel in Load, phys branches in
  ReadData/ReadCDDA/ReadSubcode, seek hints in SeekToLBA + scan block,
  physcd_close in Unload, fixed save name `physcd.sav` in mcd_set_image).
- input.cpp: `mount_phys <idx>` fifo command dispatching by core type
  (megacd live, others print "not supported yet").
- sentinel is `PHYSCD_SENTINEL` = `*PHYSCD*` from mister_physcd.h.

## per-core patch points

pattern is identical everywhere: every read function already branches
`if (toc.chd_f) ... else FileReadAdv(...)`. add a `toc.phys` branch FIRST.

### megacd (start here, matches forum dev prior art)

`support/megacd/megacdd.cpp`:
- `Load()` (~line 240): accept a sentinel filename (e.g. `*PHYSICAL*` or
  path `/tmp/PHYSCD`), call `physcd_open()` + `physcd_load_toc(&this->toc)`
  instead of LoadCUE/chd. header sniff for sectorSize: call
  `physcd_read_data2048(0x10 area)` or just read sector 0 raw and check
  "SEGADISCSYSTEM" at +16 -> sectorSize stays 2352 for phys (we always
  deliver raw), so force `this->sectorSize = 2352`.
- `ReadData()` (~918): `if (toc.phys) { physcd_read_data2048(this->lba, buf); return; }`
- `ReadCDDA()` (~944): `if (toc.phys)` -> loop `physcd_read_sector(chd_audio_read_lba + i, buf + 2352*i, NULL)`.
  NO byteswap: drive returns cdda little-endian like bin files, and the
  byteswap block in the chd path must NOT run. mirror the file branch,
  not the chd branch.
- `ReadSubcode()` (~1000): `if (toc.phys)` -> `physcd_read_sector(lba, scratch, subc)`
  then existing `InterleaveSubcode(subc, buf)`. if drive can't do subchannel
  (backend zeroes it), return -1 like the "no sub file" path does today.
- `Pause/Seek handlers`: add `physcd_seek_hint(lba)` where FileSeek is called
  (~line 890 block) so prefetch jumps with the head.

`support/megacd/megacd.cpp`:
- `mcd_set_image()` (~119): if filename is the phys sentinel, skip the
  cd_bios.rom-next-to-game logic (no folder), always load boot.rom from
  HomeDir, and skip `mcd_mount_save(filename)` path games use
  -> mount save keyed on disc serial or a fixed "physical" save. v1: fixed name.

### psx

`support/psx/psx.cpp`:
- `load_cd_image()` (~333): phys branch -> `physcd_load_toc(table)`.
  note psx toc convention: fake 150-sector pregap, `indexes[1]` used for
  lba bias (see `psx_read_cd` chd branch). drive TOC lbas are already
  absolute (include the 150), so set `tracks[0].indexes[1] = 150` and keep
  starts as-is; verify against a real disc, this is the one fiddly spot.
- `psx_read_cd()` (~474): phys branch -> `physcd_read_sector(lba - bias, buffer, NULL)`
  per sector. no byteswap (mirror file branch). pregap faking block stays.
- libcrypt: needs accurate subchannel. backend probes drive capability;
  if unsupported keep the existing .sbi mechanism working by disc serial.

### saturn

`support/saturn/saturncdd.cpp`: same trio (Load / data read / cdda read /
subcode) as megacd, same rules.

### pcecd

`support/pcecd/pcecdd.cpp`: same pattern. `seektime.cpp` stays untouched,
it only models simulated seek, which is the reason prefetch must stay ahead.

### neogeo

`support/neogeo/neogeocd.cpp`: same UIO_CD_GET pattern; neogeo cd loads
files off iso9660 rather than streaming sectors in some paths, check
whether it reads via toc tracks or parses the filesystem (if filesystem:
mount /dev/sr0 iso9660 read-only and reuse, kernel has iso9660? CHECK,
likely not built in, may need the one kernel module after all for neogeo).

### ao486 / x86 (bonus)

`ide_cdrom.cpp` already emulates an ide cdrom from an image; phys branch
here gives dos/win9x physical discs. do last.

## mount + autolaunch

- `input.cpp` cmd fifo handler (~6237): add
  `else if (!strncmp(cmd, "mount_phys", 10))` -> dispatch by core type
  exactly like the `user_io.cpp` ~967 boot-config block does
  (is_megacd() -> mcd_set_image(0, PHYS_SENTINEL), etc).
- daemon (shell or tiny c, runs from user-startup): poll
  `physcd_probe`-style ident every 2s; on new disc:
    echo "load_core /media/fat/_Console/<core>.rbf" > /dev/MiSTer_cmd
    sleep until core name matches (see /tmp/CORENAME)
    echo "mount_phys 0" > /dev/MiSTer_cmd
- `/tmp/CORENAME` is maintained by main already, use it for readiness.

## known risks to burn down on hardware

1. drive seek vs simulated seek: cores answer the game's seek with modeled
   latency then expect sectors immediately. cold physical seek is 100-300ms.
   prefetch hides linear reads; seeks depend on `physcd_seek_hint` being
   called early in the cdd seek command, before the modeled latency elapses.
   megacd models ~10 frame latency, that's ~160ms of free head start.
2. cdda into data reads and back (mixed mode): cache is shared, fine, but
   watch index/pregap boundaries, drives round lba differently near track
   starts. drive TOC has no index marks beyond index 1: games that rely on
   index 2+ inside audio tracks (rare) will misbehave.
3. b0260 quirks: probe reports whether READ CD 0xF8 and raw subchannel work.
   some slim drives reject 0xF8 on audio tracks and want 0x10 (user data
   only) for data sectors. backend fallback handles the sub case, may need
   a second fallback for flags byte.
4. usb power: spin-up spikes. powered hub.
5. fileTYPE-containing toc_t is memset in Unload paths; harmless for phys
   (no files opened) but do not FileClose phys tracks.
