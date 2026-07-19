# mister physcd: physical usb cd-rom support for MiSTer FPGA

project handoff doc. self-contained: everything verified so far, the
architecture, and a phased task list with acceptance criteria. companion
files ship alongside this doc:

- `physcd_probe.c` + `physcd_probe` (prebuilt static armhf binary)
- `mister_physcd.h` / `mister_physcd.cpp` (backend scaffold, superseded
  by the copy in the fork, kept for reference)
- `PATCHPOINTS.md` (per-core integration map, treat as part of this doc)
- `physcdd.c` + `physcdd` (phase 5 autodetect daemon, static armhf)
- `Main_MiSTer/` (the fork, branch `physcd`, builds clean)
- `tools/` (windows docker cross-compile: `.\tools\build.ps1`)

## 0. status as of 2026-07-19

phases 2-5 are CODE-COMPLETE and compiling (commit 1afcd2f on branch
`physcd`); phase 1 (hardware validation) is now the only blocker and
needs the physical mister + b0260 + discs. deploy `bin/MiSTer` +
`physcd_probe` + `physcdd` and follow section 5.

prior art update (supersedes section 2 item 9): two public gpl
implementations exist and were mined for drive quirks -
- github.com/Anime0t4ku/Main_MiSTer_Physical_Disc (pushed 2026-07-19!):
  mature megacd+psx backend. key stolen ideas: READ CD flags byte per
  track type (0xF8 data / 0x10 cdda, both 2352b), CDROMREADRAW cooked
  fallback for bridges that reject raw MMC, CDROM_SELECT_SPEED 0,
  prefetch thread affinity on both cores (main() pins itself to cpu1).
  psx toc bias confirmed: mister lbas already include the 150 lead-in.
  no subchannel support (ours probes and uses it when the drive can).
- github.com/sidneivl/Main_MiSTer branch feature/use-cdrom (the forum
  t=10235 dev): cruder reads but has the autodetect daemon pattern
  (2s CDROM_DRIVE_STATUS poll, newest-rbf resolution, door-open faking).
- retro-remake / taki udon have published NOTHING (superdock disc boot
  has not shipped; no gpl trigger). nothing to wait for there.
- full diffs saved by the research agent; see also
  /tmp scratchpad copies referenced in the session log.

if zaparoo is installed on the mister, disable its optical polling -
it fights over /dev/sr0 (anime0t4ku doc warning).

## 1. goal

fork Main_MiSTer so cd-based cores (megacd first, then psx, saturn,
pcecd, neogeo cd, ao486) can boot games directly from a physical usb
cd-rom drive, with automatic disc-type detection and automatic
core-load + mount on disc insert. target hardware: DE10-nano mister,
generic usb mass storage cd drive (user's unit: "B0260" slim usb drive).

upstream will never merge this (their FAQ explicitly rules out usb cd
drives), so this is a permanent fork. keep the diff surface small and
rebasable: one new backend directory, one field in cd.h, small guarded
branches in per-core read functions, one new fifo command.

## 2. verified findings (do not re-derive, these were checked against source)

1. **no fpga work.** cores never touch storage. main (the arm/linux
   binary) answers all cd sector requests over spi into ddr3. rbf files
   stay stock. the entire project is linux userspace.
2. **no kernel work for the core path.** the stock mister kernel
   defconfig (MiSTer-devel/Linux-Kernel_MiSTer,
   arch/arm/configs/MiSTer_defconfig) has CONFIG_CDROM=y,
   CONFIG_BLK_DEV_SR=y, CONFIG_USB_STORAGE=y. the drive enumerates as
   /dev/sr0 on stock firmware. SG_IO works on /dev/sr0 without
   CHR_DEV_SG. possible exception: iso9660 fs mount for neogeo cd
   (verify CONFIG_ISO9660_FS, likely off; only matters for that core).
3. **the request path** (identical shape in all cd cores): a poll
   function on a 10-13ms timer sends UIO_CD_GET (0x34) over spi, reads
   the pending cdd command, runs a cdd state machine
   (SetCommand/CommandExec/Update), and pushes sectors to the fpga via
   a SendData callback (ddr3 write). files:
   - megacd: support/megacd/megacd.cpp (mcd_poll), megacdd.cpp (cdd_t)
   - psx: support/psx/psx.cpp (psx_read_cd is the whole read path)
   - saturn: support/saturn/saturncdd.cpp
   - pcecd: support/pcecd/pcecdd.cpp (+ seektime.cpp, simulation only)
   - neogeo: support/neogeo/neogeocd.cpp
   - ao486: ide_cdrom.cpp
4. **every read site is a two-way branch** on `toc.chd_f` (chd backend,
   shared support/chd/mister_chd.cpp) vs fileTYPE reads (cue/bin). the
   physical drive is a third backend with the same shape. cores check
   `toc.phys` first.
5. **audio byteswap trap:** chd stores cdda big-endian and cores
   byteswap after chd reads. the drive returns little-endian like bin
   files. phys branches must mirror the FILE branch, never the chd
   branch.
6. **mount dispatch exists:** user_io.cpp (~line 967) already
   dispatches mcd_set_image / pcecd_set_image / x86_set_image etc by
   core type when restoring the `<core>.sN` boot config. reuse this
   dispatch for the new fifo command.
7. **cmd fifo:** input.cpp (~line 6237) handles /dev/MiSTer_cmd with
   load_core, screenshot, volume. no mount command exists. add
   `mount_phys <idx>`.
8. **timing headroom:** cores model their own seek latency before
   expecting data (megacd cdd latency ~10 frames, ~160ms). that models
   most of a physical cold seek, provided prefetch gets the seek hint
   at command time, not at first read.
9. **prior art:** misterfpga.org/viewtopic.php?t=10235, a dev got
   megacd running from physical disc with exactly this approach
   (linux answers UIO_CD_GET from the real drive), planning psx,
   saturn, neogeo next. also the SuperStation One SuperDock ships
   physical disc boot for psx + megacd + saturn on a mister-derived
   stack; Main_MiSTer is gpl so hunt Retro-Remake / Takiiiiiiii github
   orgs for source before writing anything they already wrote.

## 3. architecture

```
                 +--------------------------------------+
   usb cd drive  |  Main_MiSTer fork (arm linux)        |   fpga (stock rbf)
  /dev/sr0 ------+  support/physcd/  <- new backend     |
   SG_IO 0xBE    |    - toc from CDROMREADTOC* ioctls   |
   raw 2352(+96) |    - prefetch thread + sector cache  +-- spi UIO_CD_GET/SET
                 |  per-core cdd state machines         |   sectors via ddr3
                 |    (unmodified logic, new branch)    |
                 |  input.cpp: mount_phys fifo cmd      |
                 +--------------------------------------+
                          ^
   physcdd daemon --------+   (disc insert -> identify -> load_core -> mount_phys)
```

sentinel path convention: `set_image` functions receive the string
`*PHYSCD*` instead of a file path to mean "mount the physical drive".

## 4. environment and build

- repo: cloned to `Main_MiSTer/`, branch `physcd` (upstream master
  7317947 + our commit). toolchain arm-none-linux-gnueabihf gcc 10.2.
- backup remote: ssh://git@192.168.68.65:22222/shane/mister-disc.git
  (home gitea, key via ~/.ssh/config). branch `main` = this project
  root; branch `physcd` = the Main_MiSTer fork with full upstream
  history. two unrelated histories in one repo, on purpose.
- windows build: `.\tools\build.ps1` (docker image `mister-armcc`,
  object cache in volume `mister-objcache`, binary lands in
  `Main_MiSTer\bin\MiSTer`). tools: `arm-none-linux-gnueabihf-gcc -O2
  -static` inside the same image.
- integration was: `support/physcd/` (picked up automatically by the
  Makefile `$(wildcard ./support/*/*.cpp)` - no Makefile edit), plus
  `int phys;` in cd.h toc_t after `int sectorSize;`. DONE.
- deploy loop: scp the built MiSTer binary to /media/fat/ on the
  device, `sync`, reboot or `killall MiSTer` (it restarts). logs:
  main prints to serial/console, or run MiSTer from ssh in the
  foreground to see printf output. keep a known-good binary as
  /media/fat/MiSTer.bak.
- runtime telemetry: /tmp/physcd_stats.log, rewritten every 5s while
  streaming (cache hit/miss, worst miss ms, per-window cursors). first
  thing to check for any "is it the drive?" question.

### operational gotcha: never run a ripper against a mounted drive

once a phys disc is mounted, main holds /dev/srN open and a prefetch
thread issues SG_IO reads against it. anything else touching the same
device (cdrdao, dd, zaparoo's optical polling) is then FIGHTING our
prefetcher for the head - reads slow to a crawl and error recovery
multiplies. `killall MiSTer` before ripping ON the mister. same reason
the anime0t4ku doc warns about zaparoo. (recorded as general practice;
it was NOT the cause of the 2026-07-19 drive failure below, where the
rip ran on a separate pc with the drive moved to it.)

### 2026-07-19: drive stopped reading after a marathon cdrdao rip

symptoms, in the order observed: disc spinning up over and over (not
as loud as the cdrdao grind); `physcd_probe` reporting drive NOT READY;
then after a remount reporting drive status OK but failing
CDROMREADTOCHDR - no toc. /dev/sr0 enumerates normally on a rebooted
mister with the drive attached.

what that rules OUT: usb enumeration, the bridge, power delivery, and
device naming are all fine - the drive is present, claims media, and
answers ioctls. this is a READ capability failure, not a plumbing one.

the toc lives in the lead-in at the INNER edge, and repeated spin-up
cycles are what a drive does when it cannot focus/lock there. so the
suspects are (a) that specific disc's lead-in, or (b) the drive's
optics/calibration after an hour of continuous retry (slim usb drives
run hot; thermal drift is real).

decisive test is a DIFFERENT, healthy disc - and the same drive on the
pc it was just ripping on, which cross-checks drive health independent
of the mister entirely. matrix:
  other disc works on mister -> drive fine, that disc's lead-in is gone
  no disc works on mister but works on pc -> mister-side, investigate
  nothing works anywhere -> drive; let it cool, then replace
`dmesg | tail -50` distinguishes further: sense keys of NOT READY vs
MEDIUM ERROR separate "cannot spin up / focus" from "read it, bad data".

do not judge drive health with the PAL sonic cd - cdrdao already
showed it to be marginal at the outer edge.

### test hardware and media on hand (2026-07-19)

- de10-nano mister; b0260 slim usb cd drive (qualified: 741 KB/s,
  248ms worst seek, raw subchannel supported)
- discs: sonic cd (PAL, mega cd), plus psx, saturn, neogeo cd and
  pc dos (ao486) titles for phase 6. no pcecd/turbografx disc yet.
- an NTSC sonic cd .chd for A/B comparison against the PAL disc

## 5. phases and acceptance criteria

### phase 1: hardware validation (blocking, THE remaining prerequisite)
run the rebuilt `physcd_probe` (now also prints a flags matrix +
CDROMREADRAW fallback check) on the mister with the b0260 and a
handful of real discs (at minimum: one mega cd, one psx, one mixed-mode
disc with audio tracks). record for each:
- toc correctness vs known disc layout
- disc type fingerprint correct
- sequential raw KB/s (need >= 172 sustained; expect far more)
- worst-case seek+read ms
- raw subchannel supported yes/no
- flags matrix: data 0xF8, audio 0xF8 vs 0x10, CDROMREADRAW

acceptance: fingerprints correct, sequential comfortably above 172,
worst seek under ~400ms. if READ CD with flags 0xF8 fails on this
drive, adjust the flags byte strategy in the backend (0x10 for data,
0xF8 for audio) before proceeding.

#### results: b0260, 2026-07-19, mega cd disc (1 data + 34 audio, 556MB)

| metric | measured | acceptance | verdict |
|---|---|---|---|
| toc | 35 tracks, data lba 0, leadout 248060 | plausible layout | pass |
| fingerprint | "mega cd" | correct | pass |
| sequential raw | 740.9 KB/s (~4.3x, 322 sectors/s) | >= 172 | pass |
| seek+read | avg 194ms, worst 248ms | worst < 400ms | pass |
| raw subchannel | SUPPORTED | either | pass (bonus) |

subchannel support is better than assumed: the backend's probe will
enable it, ReadSubcode's phys branch delivers real subcode instead of
returning -1, and psx libcrypt should not need .sbi files later.
cost is 2448 vs 2352 bytes per sector, ~4% of a 740 KB/s budget.

seek margin (why 248ms worst is fine): SeekToLBA sets
`latency = 11 + (distance * 120) / 270000` frames for play, and
latency decrements once per mcd_poll (~13.5ms). the probe's ~40000
sector jump models 11+17 = 28 frames ~= 378ms against 248ms measured,
so ~130ms of slack. longer seeks model proportionally more (a
full-disc seek models ~1.6s). the thin case is a SHORT data seek,
where the distance term rounds to 0 and non-play seeks start from
latency 0 - that one relies entirely on prefetch, so watch for audio
hiccups at track transitions during the phase 3 game test.

STILL MISSING: the flags matrix. the run above used a stale probe
binary (pre-2026-07-19-20:23) that predates the flags-matrix block, so
`data 0xF8 / audio 0xF8 / audio 0x10 / CDROMREADRAW` are unverified.
copy the current `physcd_probe` to the mister and re-run on this same
disc - with 34 audio tracks it is the ideal disc for the audio-flags
question, which is risk #1 for this drive.

### phase 2: backend hardening [code done, on-device test pending]
- wire mister_physcd.cpp into the build. DONE
- flags-per-track-type + burst clamping at track boundaries + cooked
  fallback + drive speed + thread affinity: DONE preemptively from
  prior-art findings. tune further from phase 1 results.
- unit-style test: small test main() that loads the toc and streams
  sectors 0..5000 + random seeks, run on device, compare a data
  sector against a known dump of the same disc (bit-exact for the
  2048 user bytes; sync+header must match lba). TODO on hardware.

acceptance: bit-exact user data vs a known rip, no stalls > 100ms on
cached reads, prefetch keeps linear streaming at 0 misses.

### phase 3: megacd integration [code done, acceptance pending hardware]
follow PATCHPOINTS.md megacd section exactly. summary:
- cdd_t::Load accepts `*PHYSCD*`, calls physcd_open + physcd_load_toc,
  forces sectorSize 2352
- ReadData/ReadCDDA/ReadSubcode get `toc.phys` branches mirroring the
  FILE branch (no byteswap)
- seek sites call physcd_seek_hint(lba)
- mcd_set_image handles the sentinel: bios from HomeDir() chosen by
  detected disc region (boot_EU/US/JP.rom, then boot.rom), fixed save
  name for v1
- menu: not needed for v1, mount via fifo

acceptance: a real mega cd game boots from disc and is playable
including cdda audio tracks, on a core loaded normally from the menu
then mounted via `echo mount_phys 0 > /dev/MiSTer_cmd`.

#### result 2026-07-19: PASS. sonic cd boots and plays well from disc.

no byteswap decision confirmed correct (audio is music, not static).
remaining defect: the opening fmv stutters intermittently - beyond its
inherently low framerate. diagnosed as a mixed-mode cache problem, not
a bandwidth one (740 KB/s vs the ~344 KB/s that intro needs for both
streams):
  1. one shared prefetch cursor, slammed by every read, so the data
     stream and the cdda stream yanked it back and forth and the
     prefetch thread never got ahead of either.
  2. cache slots mapped `lba % 4096`, so streams congruent mod 4096
     evicted each other. sonic cd's audio starts at lba 55248 =
     slot 2000, exactly where the animation stream sweeps through.
fix in commit fc6b7ab: one cache window per track type (data / cdda),
each with its own slot slice and cursor; reads and seek hints retarget
only the stream that moved; drive transactions serialized behind an io
mutex so a synchronous miss and the prefetch thread stop making the
head seesaw. UNVERIFIED ON HARDWARE - confirm via the new
/tmp/physcd_stats.log (want hit rate >99% and worst miss well under
the ~13.5ms per-sector budget during the intro).

known narrow race, deferred to phase 7 disc swap: a fill_cache in
flight during physcd_load_toc can land old-disc sectors in freshly
invalidated slots. harmless in v1 (no swap support).

#### 2026-07-19 later: cache fix CONFIRMED, then a device-naming bug

/tmp/physcd_stats.log from the fixed binary during sonic cd:
`hit 750 miss 0  hitrate 100.0%  worst miss 0 ms`, both windows active
(data cursor 169, cdda cursor 147006). the two-window split works -
mixed-mode streaming is now a perfect hit rate.

then a reboot renamed the drive /dev/sr0 -> /dev/sr1 and nothing
mounted: PHYSCD_DEV_DEFAULT was hardcoded and physcd_open(NULL) just
failed. usb enumeration order is NOT stable, never assume a name.
fixed (commit 82a6d16):
- backend scans /dev/sr0../dev/sr7 and prefers a drive with media;
  physcd_set_device() pins one explicitly; physcd_open reopens if the
  pinned device differs from the current one.
- `mount_phys <n>` now means DRIVE n (/dev/srN) - the intuitive
  reading, and what a user trying to work around this reaches for.
  bare `mount_phys` autodetects. it used to be the image-slot index,
  which mcd_set_image ignores entirely, so the arg did nothing.
- physcdd autodetects the same way and sends `mount_phys <n>` for the
  drive it actually fingerprinted.
- mcd_set_image: the phys sentinel is one fixed string, so remounting
  always looked like `same_game` and skipped the bios load + reset.
  after a FAILED mount that meant retries could never recover. phys
  mounts now always re-init.

#### 2026-07-19 later still: autodetect verified; stutter is NOT the backend

bare `mount_phys` boots the game with the drive on /dev/sr1. fix
verified on hardware.

stutter status: persists with the two-window cache, but the stats
exonerate the backend - during the stuttering intro:
`hit 750-752 miss 0  hitrate 100.0%  worst miss 0 ms`, both windows
active. every sector is served from ram in microseconds; the core is
not waiting on the drive. sector starvation cannot be the cause.

crucial new variable: the physical disc is a PAL sonic cd (plays clean
on real hardware); the comparison copy that runs smooth on mister is
an NTSC chd. that comparison changes two things at once (pal/ntsc AND
disc/chd), so it does not implicate the backend. leading hypothesis:
50hz-content cadence judder - pal content on a 60hz output shows
"periodic stutter on top of low fps", real pal hardware on a pal tv
at 50hz doesn't, and an ntsc chd at 60-on-60 doesn't either.

#### 2026-07-19: SOLVED - it was a bios region mismatch

both my hypotheses were wrong. a PAL chd stuttered identically, ruling
out the physical path; the actual cause was the PAL game running on a
US cd bios. swapping to an EU bios fixed it completely. the backend,
the disc and the cache were all fine the whole time. worth remembering
how confidently the wrong causes were argued: the correct experiment
(same content, different bios) was cheaper than either hypothesis and
was not run first.

why it matters for this project specifically: file-backed games load
`cd_bios.rom` from the GAME'S OWN FOLDER, so multi-region collections
already get a per-game bios. a physical disc has no folder, so it fell
through to a single fixed `HomeDir()/boot.rom` - every disc got
whatever region that happened to be. physical mounts are exactly the
case where region auto-detection is REQUIRED rather than nice.

implemented (commit 6f10eeb):
- `physcd_region()` reads the mega drive style header the disc mirrors
  in its first data sector ("SEGA" at 0x100, region field at 0x1F0),
  handling both the old J/U/E letter form and the newer hex bitfield
  (bit0 japan / bit2 americas / bit3 europe). multi-region discs
  prefer US then EU then JP - only reachable when all are valid.
- mcd_set_image opens the drive EARLY (the bios is chosen before
  cdd.Load() runs) and loads `HomeDir()/boot_<REGION>.rom`, also
  accepting `bios_<REGION>.rom`, falling back to plain `boot.rom`.
  existing single-bios setups are unaffected.
- if it does fall back, it reads the bios rom's OWN header (same
  parser - mega cd bios roms carry the same mega drive header) and
  raises a printf + osd Info() on mismatch: "EU disc on US BIOS - add
  boot_EU.rom". the exact evening this cost, surfaced in 6 seconds.
- physcd_probe prints `disc region:` so a disc can be checked without
  deploying anything.

setup note for users: put region bioses in the megacd home dir as
boot_EU.rom / boot_US.rom / boot_JP.rom. boot.rom still works as the
catch-all.

not done, unverified: whether the megacd core ALSO has a region/video
status bit that should be set (the core appears to take its region
from the bios). psx needs none of this - it already has its own
region detection (psx_get_region / region_info_table in psx.cpp) which
will work through psx_read_cd once the phys branch is wired, so phase
6 gets it for free.

#### 2026-07-19: the disc itself is a suspect (competing hypothesis)

cdrdao ripping this disc stalls hard around 47:10:00 (lba ~212250, in
the audio tracks near the OUTER edge), audibly retrying for 30-60s per
minute of audio. that region is physically marginal.

this exposed a hole in the telemetry that invalidated my earlier "the
backend is exonerated" claim: unreadable sectors are zero-filled and
the slot is then marked VALID, so every later read of it counts as a
cache HIT. if the prefetch thread is the one that hit the bad sectors,
no miss and no latency are recorded either. a rotting disc could
therefore report a flawless 100% hit rate while feeding the core
silent zeros. fixed in commit 9df265b - stats now report `BAD n
sectors served as zeros` (cumulative per mount) and worst drive io ms.

so there are now TWO live hypotheses for the stutter, distinguishable
by one number:
  BAD == 0  -> reads are clean, stutter is timing/video (pal cadence)
  BAD  > 0  -> disc damage; the core is getting zeroed sectors
note the fmv data lives in the data track at the INNER edge, which
cdrdao got through fine, so pal cadence remains the front-runner - but
this is now measured rather than assumed.

next experiments, in order of effort:
0. redeploy and read the BAD count during the stuttering intro. this
   is now the cheapest and most decisive single number.
1. zero effort: check MiSTer.ini vsync_adjust and whether the display
   is running 50hz for this core; check the megacd osd region setting
   and which bios boot.rom actually is (eu bios for a pal disc).
2. decisive: rip the PAL disc to chd on the pc with the same usb
   drive (chdman createcd), play that chd - if it stutters the same,
   the fork is fully exonerated and it's core/video-timing territory.
3. bonus symmetric test: chdman extractcd the ntsc chd, burn to cd-r
   (megacd has no copy protection, burns boot), play physically -
   tests ntsc+physical.

### phase 4: mount_phys command [code done]
- input.cpp fifo handler: `mount_phys <idx>` dispatches by core type
  like the user_io.cpp boot-config block (is_megacd -> mcd_set_image
  with sentinel, is_psx -> psx_mount_cd, etc). unsupported core:
  print and ignore.

acceptance: works for megacd; stubs print for others.

### phase 5: autodetect daemon (physcdd) [code done]
small c daemon (reuse probe code) started from user-startup:
- poll CDROM_MEDIA_CHANGED / CDROM_DRIVE_STATUS every 2s
- on new disc: identify, map to rbf path (config file
  /media/fat/physcd.ini mapping type -> rbf path, with sane defaults
  resolving the newest _Console/<name>_*.rbf), then:
  - `load_core <rbf>` via fifo
  - wait for /tmp/CORENAME to match (timeout 15s)
  - `mount_phys 0` via fifo
- on eject: nothing in v1 (leave core running)

acceptance: insert mega cd disc from the main menu, game boots hands-free.

### phase 6: more cores

test media ON HAND (2026-07-19): psx, saturn, neogeo cd, pc dos
(ao486). NOT on hand: a pcecd/turbografx disc - that core is blocked
on acquiring media, so it moves to the back regardless of difficulty.

order:
1. **psx** - CODE DONE (commit c384cf6), untested on hardware.
   what the integration turned out to need:
   - toc convention differs from megacd in TWO ways, not one. psx
     uses INCLUSIVE track ends (`lba >= start && lba <= end`) where
     megacd/physcd use exclusive, AND fakes a 150-sector pregap so
     core lba 150 is the first sector of track 1. `load_phys()`
     converts both, shifting every track by the same 150 so
     `psx_read_cd` subtracts one uniform bias - matching what the
     chd branch already does (`lba - toc.tracks[0].indexes[1]`).
   - indexes[1] stays 0 on later tracks: the drive reports each start
     as its INDEX 1 position, so no further correction applies.
     `pregap` stays 0 too - a cue whose file omits the gap has to
     fake zeros, but a real disc HAS those sectors, so just read them.
   - region + game id come free: psx_get_region reads sector 154 and
     psx_get_game_info parses iso9660 at lba 172, both through
     psx_read_cd, so both work on a physical mount with no plumbing.
     that also makes the sbi.zip/<game_id>.sbi lookup work, so
     libcrypt titles are covered by the existing mechanism.
   - a physical disc has no path, and the sentinel is not a legal
     filename, so saves/savestates/gameid key off the disc's own game
     id instead. NOTE file-backed psx games key the memory card off
     the game FOLDER (psx_mount_save(last_dir)), so a physical copy
     and a ripped copy of the same game get SEPARATE memory cards.
     unavoidable - a disc cannot know what folder you filed its rip
     under - but worth documenting.
   - no bios work needed unlike megacd: psx sends region to the core
     in the disk metadata block, so the core adjusts itself.
   subchannel works on this drive, so real libcrypt (rather than .sbi)
   remains open as a phase 7 improvement - a differentiator vs both
   public forks, neither of which implements subchannel.
2. **saturn** - same trio as megacd, same rules.
3. **neogeo cd** - the open question is whether it streams sectors via
   the toc or parses iso9660 for files. if filesystem: prefer a small
   userspace iso9660 reader over a kernel module, to keep the "stock
   kernel" property. NOTE the payoff is double - psx achievement
   hashing in phase 8 also needs to locate a file via iso9660, so the
   same reader serves both. write it once, reuse.
4. **ao486 (pc dos)** - listed last originally, but reconsider: it is
   ATAPI request/response through ide_cdrom.cpp, with no cdd state
   machine and no realtime audio deadline, so it may be the SIMPLEST
   of the four rather than the hardest. different shape though: back
   the ATAPI READ commands with physcd instead of adding a toc.phys
   branch to a cdd read function. data-only dos discs have no cdda
   timing pressure at all.
5. **pcecd** - blocked on media. `seektime.cpp` stays untouched (it
   models simulated seek, which is exactly why prefetch must stay
   ahead).

acceptance per core: one known-good disc boots and plays with audio.
record /tmp/physcd_stats.log hit rate per core - a core whose read
pattern defeats the two-window cache will show up as misses there.

### phase 7: polish
- scratched-disc watchdog behavior review (backend currently serves
  zeros after 3 retries so cores don't hang; verify cores tolerate it)
- disc swap for multi-disc games (eject detection -> cdd open/close
  status transitions; megacd cdd already has CD_STAT_OPEN). NOTE this
  is where the deferred load_toc/fill_cache race must be fixed - see
  the phase 3 results notes.
- psx libcrypt: subchannel IS supported on the b0260, so try reading
  real subchannel first; keep the existing .sbi lookup by disc serial
  as the fallback for drives that can't
- osd feedback: Info() popup on disc detect ("physcd: PSX disc
  detected") is a two-line add in the daemon-adjacent main code

### phase 8: retroachievements coexistence

why it is a phase and not a setting: the mister RA stack installs its
own Main_MiSTer fork AS /media/fat/MiSTer - the same binary slot as
ours. you cannot run both. see section 6c for how that stack is built;
this is the task list.

1. **merge.** `git remote add odelot <fork>`, merge into `physcd`,
   resolve. both are gpl forks of the same upstream. expected overlap
   is near zero: they hook the main loop / ddram / user_io, we touch
   megacd cdd internals + support/physcd + one fifo command. do this
   merge FIRST and keep it rebased - the longer both forks drift, the
   worse it gets.
2. **verify nothing regressed.** cue and chd games must still work on
   the merged binary, and RA must still fire on file-backed games.
   this is the regression floor before any physical work.
3. **rbf/launcher side: expected to need nothing.** RA symlinks
   _Console launcher paths at _RA_Cores builds, and physcdd resolves
   cores through _Console, so with RA active the daemon should load
   the RA core automatically. the RA megacd rbf keeps the MEGACD
   corename, so is_megacd() and mount_phys work unchanged. VERIFY,
   don't assume.
4. **the actual work: achievement identification.** rcheevos hashes cd
   games by reading early disc sectors through a cdreader abstraction
   (open_track / read_sector / close_track function pointers), NOT by
   hashing an image file - which is lucky, because a physical mount
   has no file. register a cdreader backed by physcd_read_sector when
   the filename is the physcd sentinel; the sectors it wants are
   already in our cache. verify the exact struct/callback names
   against odelot's vendored rcheevos copy rather than trusting this
   description. sega cd and saturn hash the disc header near the start
   of the data track (easy); psx must locate its boot executable via
   iso9660 (needs the phase 6 item 3 reader).

acceptance: on one merged binary - (a) a cue/chd game still earns
achievements, proving no regression; (b) a physical disc boots on the
RA megacd core AND starts an RA session with the correct game
identified; (c) an achievement actually triggers from physical media.

fallback if the merge turns ugly: ship two binaries and a switcher
script (MiSTer vs MiSTer_RA), which is what the physical-disc prior
art does for its own routing. strictly worse - two builds to maintain,
no achievements when using physical discs - so treat it as a retreat,
not a plan.

## 6. risks and mitigations

| risk | mitigation |
|---|---|
| drive rejects READ CD 0xF8 | per-track-type flags fallback in sg_read_cd |
| cold seeks exceed modeled latency | seek hint at cdd command time + 10MB prefetch; if still short, add optional "instant seek off" tolerance testing |
| index >1 audio positions (rare games) | drive toc lacks index marks; accept as known limitation, document |
| usb power spikes on spin-up | powered hub, document requirement |
| bios region does not match disc region (plays, but stutters) | detect region from the disc header and load boot_<REGION>.rom; warn via osd when falling back to a mismatched boot.rom. file-backed games dodge this via per-folder cd_bios.rom, physical discs cannot |
| aging/rotting discs read marginally, esp. outer edge | backend zero-fills after retries so the core never hangs, and now COUNTS it (stats `BAD`) so it can't masquerade as a clean read. possible enhancement: we currently request CDROM_SELECT_SPEED 0 = MAXIMUM, which is right for prefetch headroom on a good disc but wrong for a marginal one - slower reads recover more. consider dropping speed adaptively once BAD goes nonzero |
| upstream drift | keep all changes behind toc.phys / sentinel; rebase quarterly |
| neogeo needs iso9660 | prefer tiny userspace iso9660 reader over kernel module to keep "stock kernel" property; the same reader is needed for psx achievement hashing, so it pays for itself twice |
| RA fork and our fork claim the same /media/fat/MiSTer slot | merge the two forks (phase 8), do it early and rebase often; two-binary switcher only as a retreat |

## 6b. drive compatibility (design position)

any usb optical drive that enumerates as /dev/srN (usb mass storage /
uas) should work - that is effectively all of them, including dvd and
bd drives, which read cds faster than we need. per-drive variance is
exactly three things, each probed or fallback'd at runtime:
1. READ CD 0xF8 on data tracks - near universal; bridges that hide raw
   MMC entirely fall back to kernel CDROMREADRAW (slower ceiling,
   still >1x).
2. audio-track flags quirk - handled by the 0xF8/0x10 per-type split.
3. raw subchannel - probed per disc, graceful degrade (only cd+g and
   psx libcrypt care).
speed floor is 172 KB/s sustained + <400ms worst seek: any drive from
this century clears it several times over. `physcd_probe` is the
30-second qualification card for a new drive (flags matrix section
reports all three variances). known trouble: apple usb superdrive
(needs a vendor init command, won't work stock), bus-powered slims
browning out the de10 usb on spin-up (powered hub), and dvd DATA
media (different read path, not implemented - cd media only).
multiple drives: autodetect prefers the one with media, `mount_phys
<n>` pins /dev/srN.

## 6c. retroachievements coexistence (background for phase 8)

the mister RA stack (manyhats-mike/mister-fpga-retroachievements,
installed via mister companion) = odelot's fork of Main_MiSTer (reads
core ram over ddram each frame, evaluates rcheevos) + patched rbf per
system, installed AS /media/fat/MiSTer. that is the same binary slot
as our fork: you cannot run both, so coexistence means MERGING the two
main forks (both gpl forks of the same upstream). our diff surface was
kept deliberately small for exactly this: expected overlap is near
zero (they touch main-loop/ddram/user_io hooks; we touch megacd cdd
internals, support/physcd, one fifo command).

plan when wanted:
1. git remote add odelot's fork, merge into physcd branch, resolve.
2. rbf side: RA installs symlinks so _Console launcher paths point at
   _RA_Cores builds - our daemon's newest-rbf scan follows _Console,
   so with RA active it would load the RA core automatically. RA
   megacd rbf keeps the MEGACD corename, so is_megacd()/mount_phys
   work unchanged.
3. the real work: achievement identification. rcheevos hashes cd games
   by reading early disc sectors through its cdreader abstraction (not
   by hashing the whole image file). physical mounts have no file, so
   odelot's hash path must be pointed at physcd_read_sector when the
   filename is the physcd sentinel - the needed sectors are already in
   our cache. megacd hashing reads the header region of the data
   track; feasible. until that's wired, physical boots on a merged
   binary would play fine but start no RA session.

## 7. explicit non-goals

- dreamcast (gd-rom unreadable on standard drives)
- ripping/dumping to storage (different project; xsuite territory)
- upstreaming
- windows/dos burner features in ao486 (read-only)

## 8. style

match existing Main_MiSTer conventions: tabs, printf logging with the
existing color escape style, no exceptions/no stl containers in hot
paths, c-with-classes like the rest of the codebase.
