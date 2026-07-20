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

windows agreed with the mister and named the failure precisely:
`Win32_CDROMDrive.MediaLoaded = False`, `Status = OK`, and NO cdrom
errors in the event log at all. that is a media DETECTION failure, not
a read failure - the drive never got far enough to error. which also
identified the hardware properly: an HL-DT-ST GTA0N 24x tray dvd
writer behind an Initio INIC-1618L usb-sata bridge (so "b0260" is the
enclosure, not the drive).

RESOLVED: eject, insert any other disc, reinsert the original -> media
detected, `SONIC_CD___`, CDFS, HealthStatus Healthy. drive and disc
both fine. the data track reads out at 112,533,504 bytes = 54948
sectors, which lines up with the 55248-sector track 1 in our toc, so
the disc is intact too.

full sequence, which rules out nearly everything:
  sonic cd, cdrdao rip fails
  -> mister: ps1 game FAIL, sonic cd FAIL
  -> linux tablet: sonic cd + two ps1 games FAIL
  -> windows pc: sonic cd FAIL
  -> windows pc: MUSIC CD **WORKS**
  -> windows pc: sonic cd **WORKS**, and stays working

so it is NOT: mechanical unseating (the tray was cycled many times
with several discs), usb/power/enumeration (clean throughout), disc
damage (every disc works now), or os-specific (failed on three).

what is left is drive/bridge firmware state that survived power
cycles, machine changes and tray cycles - and that a successful read
CLEARED. note the one disc that worked first was the only CD-DA in
the set; every failing disc was a data disc. whether audio-vs-data is
the actual trigger or a coincidence is unproven on one sample, but a
successful media detection is what reset it.

this drive class has form: the user reports the same behaviour from
another of these usb optical units previously. treat it as a known
flakiness of cheap usb optical bridges rather than a fault to chase.

RECOVERY RECIPE (empirical, worked): put a known-good AUDIO CD in. if
it reads, the drive is unstuck and data discs work again. do that
before rebooting, replugging or condemning the drive - none of those
fixed it here.

possible small enhancement, NOT yet implemented: when physcd_open
finds a drive present but physcd_load_toc keeps failing with no
medium, print that recovery hint rather than a bare error. cheap, and
this has now cost real debugging time twice.

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

#### 2026-07-20: extended play - stalls are IDLE-WAKE, not seek current

x-files (psx, all fmv): 15 minutes clean, no hitches. wipeout 2097
froze ~20s into a race, recovered by itself, then played fine. sonic cd
stalled at the same time-travel white screen, drive audibly spun DOWN,
minutes of nothing, spun back up, resumed.

stats during the sonic cd session:
  hit 143 miss 1  hitrate 99.3%  worst miss 102ms
  BAD 0 sectors served as zeros  worst drive io 289ms
so the cache is healthy and NO sector was ever served as zeros - the
read path is not the fault. dmesg shows the real event: repeated
"reset high-speed USB device", plus one full disconnect that brought
the drive back as a new device number (that is what moved it sr0 ->
sr1 mid-session).

my first reading was power brownout under seek load. the user's
counter-argument is better and fits the evidence: x-files reads
CONTINUOUSLY and never drops, so sustained draw is the safest case,
not the worst. every stall follows a QUIET spell - a level sitting in
ram, no disc access - and our prefetcher goes silent as soon as its
window is full. an idle usb optical drive spins down, and linux may
autosuspend the usb device underneath it; waking that is what costs
minutes, and the "reset high-speed USB device" lines are the host
trying to recover a device that did not answer in time - a
CONSEQUENCE of the sleep, not an independent power fault.

two fixes, independent, both shipped:
1. commit 5dbc855 - survive the re-enumeration. our fd was stale
   FOREVER after a disconnect, so every read would have failed with no
   recovery short of a remount. the prefetch thread now counts
   consecutive unreadable bursts, probes for ENODEV/ENXIO/EIO after 8,
   rescans /dev/sr0-7 and re-opens wherever the drive reappeared
   (rate-limited 5s). stats gained REATTACH + current device, which
   separates a bus problem from bad media at a glance.
2. idle keep-alive (KEEPALIVE_MS 15000): while a disc is MOUNTED and
   the prefetcher has been quiet for 15s, read one sector at the
   cursor and discard it. keeps the drive spun up, keeps the head
   near the next read, and incidentally defeats usb autosuspend since
   activity resets that timer too. deliberately NOT done at the menu -
   an idle drive there should be free to sleep.
3. tools/usb_nosleep.sh - pins the drive's usb device and its parent
   hub port to power/control=on, and prints autosuspend_delay_ms. run
   it standalone as the DIAGNOSTIC (does it report auto?) or add it to
   user-startup.sh as the config-level fix.

note the hardware is not the variable it looked like: the drive is a
slimline usb writer with a hard-attached short cable on a POWERED hub
already, and it is essentially the only model still sold. so a
software-side answer was needed rather than "buy a better drive".

#### 2026-07-20 (later): the bluetooth controller proves it is POWER

next session, mid-stall, the BLUETOOTH CONTROLLER on the same hub also
stopped responding - and started working again the instant the drive
spun back up. our software touches /dev/srN and nothing else; it
cannot stall a bt dongle. two unrelated devices on one hub failing and
recovering together is a shared-supply (or shared host-controller)
event, not anything the backend can cause or cure. the de10-nano has
a single dwc2 host controller with everything behind one hub, so a
sag or an error-recovery stall takes the whole tree down.

so: idle-wake made things much better but the residual stalls are
electrical. the useful mitigations are physical, and separating the
POWER DOMAINS matters more than raw psu amps - inrush is local, so
the fix is to stop the drive's spin-up transient sharing a rail with
anything you care about:
  - drive on its own powered hub, bt and input devices elsewhere
  - or bt moved to the de10-nano's own usb port
  - beefier 5v supply for whichever hub keeps the drive
what the software now does is degrade gracefully instead of dying: the
game kept running, audio and input resumed on their own, and the
re-attach path is there if the drive ever comes back renumbered.

### phase 5b: autoboot from the menu (PLANNED 2026-07-20, do before phase 6)

goal: mister sitting at the menu, disc goes in (or is already there at
power-on) -> identify it -> osd message "PSX disc detected, loading" ->
load the matching core -> mount, bios and region already handled by the
existing mount paths. build this BEFORE adding saturn/ngcd/etc so every
new core inherits the ux instead of retrofitting it three times.

#### decision: move it INTO main, retire physcdd

physcdd (phase 5) is written but never tested. do not ship it. reasons,
in order of weight:
1. CONTENTION. two processes polling /dev/srN is precisely the failure
   already documented above (cdrdao, zaparoo). main holds the drive
   open with a prefetch thread hammering it; a second process doing
   CDROM_DRIVE_STATUS on the same device fights it for the head. doing
   that to ourselves is self-inflicted.
2. only main can draw the osd. a daemon would need a new fifo command
   purely to display text, and would still be blind to menu state.
3. main knows is_menu(), the core identity and whether the mount
   succeeded directly. the daemon can only infer via /tmp/CORENAME
   polling and timeouts.
4. region/bios selection already lives inside the mount paths in main.

#### hard constraint: the poll CANNOT go in the main loop

main runs two cothreads cooperatively (scheduler.cpp: co_poll =
user_io_poll/frame_timer/input_poll/video_poll, co_ui = HandleUI/
OsdUpdate) with a ~1ms budget each - see SPIKE_SCOPE("co_poll", 1000).
CDROM_DRIVE_STATUS on a spinning-up drive blocks for 100ms+, which
would stall the ui outright. so media polling lives on a REAL
background thread (we already have one) and main only consumes an
event in O(1).

#### backend api to add (mister_physcd.h)

    typedef enum { PHYSCD_EV_NONE, PHYSCD_EV_DISC_IN, PHYSCD_EV_DISC_OUT } physcd_event_t;
    int  physcd_watch_start(void);   // LIGHTWEIGHT: fd only
    void physcd_watch_stop(void);
    physcd_event_t physcd_poll_event(physcd_disc_t *type, physcd_region_t *region);

watch mode must be CHEAP, and that means a second open mode. today
physcd_open allocates the 9.5MB slot cache and spins up the prefetch
thread; making the idle menu carry that just to notice a disc is
unreasonable. physcd_watch_start opens the fd only - no cache, no
prefetch thread - because physcd_media_changed/physcd_disc_present
both return 0 while pcd.fd < 0 and so require the drive open.
the full physcd_open stays exactly as it is for mount time.

watcher thread: every ~1-2s CDROM_DRIVE_STATUS + CDROM_MEDIA_CHANGED.
identification must happen ON THIS THREAD, once per media change, never
per tick: physcd_identify may trigger a full physcd_load_toc plus
several raw sector reads and blocks for SECONDS. prior art independently
reached the same conclusion and left a comment saying not to identify
in the poll. it must also not read at all while a mount is active - the
prefetcher owns the drive then, so the watcher degrades to a pure
status poll.

it must NEVER touch the OSD: Info/InfoMessage/OsdWrite are not
thread-safe. publish a flag, let the menu tick render it (the same
pattern scaler.cpp uses for screenshot results).

#### THE ARCHITECTURE-DEFINING FACT: fpga_load_rbf EXECS

`fpga_load_rbf` does not return. it calls app_restart(), which replaces
the process image. every variable, every fd (including the physcd
/dev/srN fd) and the prefetch pthread are destroyed. there is no
"after the load" in the same process, so

    detect -> load core -> mount        <-- IMPOSSIBLE as one sequence

any code written that way is dead after the second statement. this is
forced, not incidental: sidneivl's fork hit the same wall and its
AutoLoadCore never mounts at all - the NEW process re-detects the disc
from scratch and mounts it there. the feature is inherently TWO-PHASE.

the only things that cross the exec are argv[1] (rbf path), argv[2]
(xml/mgl path) and the filesystem.

#### phase A - the menu process: detect, announce, hand off, load

a poll function of its own, gated on is_menu(), called from the top of
HandleUI (NOT hung off a menu-drawing branch the way prior art did it,
which only ran when one specific page was being drawn):
1. consume an event from the watcher thread (O(1), no ioctls here).
2. draw the banner and set a timer. do NOT use Info() - see below.
3. on a LATER tick (>=100ms, so the banner is actually on screen):
   resolve the rbf, write the handoff marker, physcd_close(), then
   fpga_load_rbf(). the process ends inside that call.
4. FAILED/unknown disc: draw the reason, then LATCH until the next
   media change, or a bad disc reloads the core forever.

#### Info() DOES NOT WORK AT THE MENU - use OsdWrite

`Info()` is guarded by `if (menustate <= MENU_INFO)` (menu.cpp:7962),
and the menu core at rest sits ABOVE that with its browser open. so
the obvious `Info("PSX disc detected...")` silently does nothing -
this is the single biggest trap in the whole feature. the bootcore
countdown (menu.cpp:7628-7723) is the working precedent: it writes
directly to OSD lines 12-15 with OsdWrite on a 100ms tick, then loads
a core on a later tick, and it deliberately bypasses Info() for
exactly this reason. copy that.
(MenuHide() then Info() also works but tears down whatever the user
was doing - acceptable only because we are about to load a core.)
Info() DOES work in phase B, where no menu is open - which is why the
existing mcd_set_image Info() calls already work.

#### handoff marker: /tmp/physcd_autoboot

phase A writes it just before the exec; phase B consumes it. contents:
the expected core name and the device path. this is what distinguishes
"the disc made me load this core" from "the user loaded this core by
hand", and it is the one improvement over prior art worth making:
sidneivl's re-detect fires on ANY core start, so loading psx manually
to play a chd while a disc happens to sit in the drive would hijack
it. with a marker, phase B only auto-mounts when phase A asked it to,
and deletes the marker immediately so it is strictly one-shot.
(MGL was considered and rejected: it needs a browsable path, and
PHYSCD_SENTINEL is not one - menu.cpp:2736 prefixes anything not
starting with '/' with HomeDir(). it would need a new action type.)

#### phase B - the new core process: settle, mount, report

at startup, after user_io_init returns:
1. no marker -> do nothing at all. manual core loads stay untouched.
2. marker present but naming a different core -> delete it and do
   nothing (stale).
3. match -> wait a settle delay before mounting. "user_io_init
   returned" is HPS-side readiness only; it says nothing about the
   core's own cpu/bios being up. there is no handshake to wait on -
   MGL exists precisely because cores need extra wall-clock time and
   expresses it as a hand-tuned per-item delay in SECONDS. budget
   ~2s, make it configurable, tune per core.
4. mount via the shared dispatcher, delete the marker, then Info()
   naming the game (psx has the game id, megacd the region). Info()
   works here.

#### trigger policy (important - do not hijack an active game)

- is_menu() -> full autoboot.
- a MATCHING cd core already running -> mount only. this is disc swap
  (phase 7) almost for free.
- any other core running -> ignore entirely.

#### reuse, do not reinvent

- rbf resolution: bootcore.cpp already has findCore(), which recurses
  the _* dirs, prefers an exact name match and otherwise takes the
  NEWEST by date - exactly what the daemon reimplemented. two gotchas:
  it is `static` (not linkable), AND bootcore.h:12 declares a stale
  3-arg signature matching no definition in the tree. add a narrow
  wrapper (e.g. `bool find_core_rbf(const char *coreName, char *out,
  size_t len)`) in bootcore.cpp rather than trusting that header.
  the caller owns CoreMatch::path and must delete[] it.
- findCore is CASE-SENSITIVE, so the disc-type -> core-name table must
  carry the exact rbf basename casing, NOT the uppercase is_*()
  identity strings: MEGACD->"MegaCD", PSX->"PSX", SATURN->"Saturn",
  PCECD->"TurboGrafx16", NEOGEO->"NeoGeo". feeding it "MEGACD" finds
  nothing.
- mount dispatch: factor the core-type switch currently inside the
  mount_phys fifo handler (input.cpp) into one shared function so the
  fifo command, phase B and any future core cannot drift apart.
- worth copying from prior art: the blink-while-identifying icon on
  the menu (good ux, costs nothing), and eject_cdrom's door handling
  (CDROM_LOCKDOOR 0, 100ms, CDROMEJECT, retry once after 500ms on
  EBUSY) given our documented door-lock quirk.
- worth NOT copying: their fingerprinting (ours is strictly better -
  raw 2352 reads offset from first_data_lba, mode2-form1 aware, and
  it detects pcecd and neogeo too), their hardcoded four-path bios
  list (ours is region-matched), and their unanchored rbf prefix match.

#### config (cfg.cpp/cfg.h + MiSTer.ini)

    physcd_autoboot=1     ; 0 off, 1 menu only (default), 2 also swap
    physcd_device=        ; optional pin, e.g. /dev/sr1
    physcd_mount_delay=2  ; phase B settle seconds

adding an option is just two edits: a field in cfg_t (cfg.h) and a row
in the ini_vars table (cfg.cpp) - the table IS the registry, no other
registration step. use min/max 0/1 for booleans since ini_parse_numeric
clamps. GOTCHA: ini section matching is against the RUNNING core's
name, so at the menu only `[MiSTer]` applies - a `[PSX]` section is
never read there. the autoboot option must live in `[MiSTer]`.

#### edge cases that must be handled

1. disc already in at power-on: do not race bootcore_init. bootcore
   wins at startup; autoboot only acts once the menu is settled.
   (bootcore also re-multiplies cfg.bootcore_timeout by 10 in place at
   bootcore.cpp:265, so never call bootcore_init twice.)
2. failure latch, per above.
3. audio cd / unknown disc: message, load nothing.
4. menu-time watching holds the drive fd for the whole idle session,
   so external tools (physcd_probe over ssh, zaparoo) must not touch
   it while mister runs. document loudly. the fd-only watch mode keeps
   the cost to one fd rather than fd + thread + 9.5MB.
5. autoboot=0 must leave `mount_phys` working exactly as today.
6. phase A must physcd_close() BEFORE fpga_load_rbf: the exec would
   otherwise tear down the process with the prefetch thread possibly
   mid-SG_IO, leaving the drive busy for the next process.

#### phasing and acceptance

- 5b.1 lightweight watch api + event queue, no behaviour change;
      prove it by logging insert/eject events at the menu
- 5b.2 phase A: menu tick, OsdWrite banner, handoff marker, load
      ACCEPT: insert a disc at the menu -> banner is READ-able ->
      correct core loads
- 5b.3 phase B: consume marker, settle, mount, Info
      ACCEPT: sonic cd and x-files each boot hands-free end to end;
      loading psx by hand with a disc in the drive does NOT hijack
- 5b.4 config + the bootcore findCore wrapper
      ACCEPT: physcd_autoboot=0 disables it; mount_phys unaffected
- 5b.5 megacd hot-swap (see below)
- 5b.6 delete physcdd.c and its docs references

#### 5b.5 hot-swap: megacd yes, psx no

megacd has a complete tray model and mcd_set_image already does
OPEN -> (load) -> STOP. a swap path needs a NEW entry point (e.g.
mcd_swap_disc) that keeps the drive open, calls physcd_load_toc into
cdd.toc in place (it already invalidates the sector cache), sets
cdd.loaded, forces CD_STAT_OPEN with latency 0, HOLDS ~500ms, then
CD_STAT_STOP with latency 10. the 500ms dwell is the whole trick - the
bios polls status and must observe OPEN long enough to believe the
tray moved before it re-reads the toc. today that dwell only happens
by accident because Load() blocks for seconds in between.
do NOT get there by relaxing `if (phys) same_game = 0;` in
mcd_set_image - that line is correct for first mount (and is what lets
a failed mount recover); hot-swap needs its own path.

psx has no lid, no tray and no status bit for either. re-mounting sets
the reset bit in the metadata block, i.e. a swap resets the machine.
real lid emulation would need a new core-side status bit in the PSX
RTL - it does not exist to be wired up, so psx multi-disc swap is OUT
OF SCOPE and should be documented as such. two cheap partial wins
worth testing: mount_cd(0, ...) is the closest thing to "tray open"
for signalling disc removal without a reset, and psx.cpp's
`if (phys) same_game = 0;` currently re-mounts the memory card on
every swap - for a multi-disc game the card should follow the GAME,
not the disc.

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

#### 2026-07-20: first psx hardware test - froze the mister

probe on the psx disc (12 tracks, 681MB): fingerprint psx correct,
1072 KB/s sequential, raw subchannel supported, and the flags matrix
FINALLY captured - `data 0xF8 ok / data CDROMREADRAW ok / audio 0xF8
ok / audio 0x10 ok`. this drive honours everything, so the flags-byte
risk from section 6 is closed for it (the per-track-type split still
matters for other drives).

but: `seek+read avg 585ms, worst 2479ms`, versus 194/248ms on the
megacd disc. same drive, so that is disc-dependent - and it was enough
to expose a real defect.

`mount_phys` locked the mister hard: no osd, no input, hard reboot to
recover. /tmp/physcd_stats.log did not exist, and since the counters
only increment AFTER fill_cache returns, that places the freeze inside
the very first fill_cache.

root cause: a cache miss is serviced on the MAIN thread - the same one
answering the fpga - and that path was unbounded. a failed burst fell
into per-sector retries of 3 attempts x 8s, so one bad region could
block for ~6 minutes; and the io mutex was held around the whole retry
loop, so a consumer miss could also queue behind a background storm.
fixed in commit e8cd010 (see the commit message for the full list).

the dmesg errors during this are NOT ours and are a red herring:
opcode 0x28 is READ(10), the cooked block read, which we never issue -
we only use 0xBE via SG_IO. sense key 0x5 ASC 0x64 is "illegal mode
for this track", the expected answer when the kernel/udev probes a
mode 2 psx disc as a filesystem on media change. harmless noise, but
it does mean something else is touching the block device: worth
checking whether mister companion or zaparoo polls the optical drive,
since that contends with us for the head.

still unverified after the fix: whether the disc boots, the 150-sector
bias, and psx region detection (the core defaulted to US on a disc the
user believes is EU - but the freeze happened before region detection
could run, so that proves nothing yet).

#### 2026-07-20: psx crash ROOT-CAUSED - upstream NULL deref, our name

the fault handler's "SIGSEGV at address (nil)" plus elimination
(megacd phys boots; psx chd boots; crash fires the instant mount_phys
lands with the psx core loaded; companion disconnected changes
nothing) led to file_io.cpp FileGenerateSavestatePath:

    char *e = strrchr(fname, '.');
    if (e) e[0] = 0;                      // NULL check guards this...
    if(sufx) sprintf(e, "_%d.ss", sufx);  // ...but not these
    else strcat(e, ".ss");

every filename mister ever fed it had an extension, so e was never
NULL - until the psx phys path substituted the disc's bare game id
("SCES-01565", no slash, no dot) as the pseudo-filename. psx has
savestates, so psx_mount_cd -> process_ss -> FileGenerateSavestatePath
-> sprintf to NULL. megacd never takes the savestate path, which is
why sonic cd boots from the same backend. chd survives because ".chd"
supplies the dot. timing fits: the crash lands right after the ~26
sector reads that extract the game id, a split second after the echo.

fixed in file_io.cpp (append when no extension). this is a LATENT
UPSTREAM BUG - any extensionless rom name crashes any savestate
core - worth offering upstream independently of the fork.

lesson, again: the "crash during startup, before the fifo read" theory
from the truncated run-C log was wrong - that log was ambiguous
(companion held the fifo; sequencing unclear) and I over-read it. the
handler data (si_addr == exactly 0) + the elimination matrix the user
ran (megacd-phys ok / psx-chd ok / psx-phys crash) was what actually
localized it.

#### 2026-07-20: X-FILES BOOTS FROM PHYSICAL DISC - phase 6.1 met

SCES-01565 (pal) boots first try on the psx core once the savestate
NULL deref is fixed. the 150-sector bias, the inclusive-end toc
transform and the game-id-as-filename substitution are all validated
by that boot. still to check on this disc: cdda audio and the BAD
count in /tmp/physcd_stats.log.

#### 2026-07-20: adversarial review of the whole fork diff

22 agents over 4 dimensions (psx path, backend concurrency, megacd
regressions, name-shape sweep), every finding independently attacked
before being believed: 12 confirmed, 3 refuted. the important one was
found by FOUR reviewers independently and is a defect I introduced in
the very fix that stopped the freeze:

  the bounded sync-miss path stamped s->lba on its zero-filled slots.
  a stamped slot is indistinguishable from a good one - the prefetch
  scan only targets slots whose lba does NOT match, and s->bad was
  written but never read by anything. so one transient 3s timeout
  condemned 8 sectors to serve zeros for the REST OF THE MOUNT, on a
  perfectly readable disc, counted as cache HITS so the stats hid it.
  my comment promising the prefetcher would retry them described code
  that did not exist. worse on a bridge that passes the one-sector
  subchannel probe but rejects multi-sector-with-sub: the downgrade
  that would have caught it sat AFTER the sync early-return, so every
  sync fill would have failed and poisoned its slots.

fixed in a54360d (sync path claims nothing; zeros served once,
uncached; cursor left on the failed lba so the background ladder
re-reads it) and d035c1f (the rest):
  - physcd_load_toc n==0 guard: a drive answering trk0 > trk1 wrote
    tracks[-1], ~450 bytes before the caller's toc_t
  - physcd_read_sector always defines dst now; megacd ReadCDDA ignores
    the return value and shipped uninitialised stack to the fpga at
    leadout
  - physcd_read_sector_sub: sub validity is PER SECTOR, not per disc -
    retry/cooked-fallback sectors carry no subchannel, and ReadSubcode
    was interleaving those zeros as if they were real subcode
  - failed phys mounts leaked the fd, prefetch thread and 9.5MB cache
    in three places (toc.phys unset, so Unload never released it)
  - mount_phys needed a terminator check
accepted as-is: telemetry counters race between threads (diagnostic
only). refuted and NOT changed: three load_toc/prefetch race theories.

lesson worth keeping: the bug was in the fix for the previous bug, in
code I had reasoned about carefully and written a confident comment
about. the comment was the tell - it asserted behaviour no code
implemented. cheap to check, and nobody had checked.

#### 2026-07-20: drive speed capped at 4x (was: maximum)

`physcd_open` used to send CDROM_SELECT_SPEED 0 = MAXIMUM. wrong call
for this project's actual media:
- the requirement is 172 KB/s sustained (1x raw cdda), ~353 KB/s for
  the worst realistic case (data + cdda together, or psx 2x mode).
  4x = ~708 KB/s leaves ~2x headroom.
- tracking errors scale with rpm on warped, dirty or 30-year-old
  discs, and the OUTER edge - where rot starts and where cdrdao
  stalled for a minute per minute of audio - is exactly where a drive
  spins fastest.
- our prefetcher already works a drive far harder than a real console
  ever did; this drive class has wedged into a no-media state twice
  under sustained load. less rpm = less heat, less noise, less wear.
- costs little: the measured "maximum" was only 741-1072 KB/s (~4-6x,
  CAV), so the cap mostly reins in the outer edge.
re-applied per disc, because a media change resets it on many drives,
and best-effort because plenty of drives ignore SET SPEED entirely.

`physcd_probe <dev> <Nx>` now takes an optional speed so the
throughput/seek tradeoff can be MEASURED on a troublesome disc rather
than argued about - e.g. compare `physcd_probe /dev/sr0 0` (maximum)
against `physcd_probe /dev/sr0 4` and `2` on the pal sonic cd.

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
| drive worked far harder than a real console works it | the prefetcher holds the device open and pulls 96 sectors ahead of both streams for a whole session, where a real console reads on demand and parks the head. mitigated by capping speed at 4x (see below); if a long session still wedges the drive, consider idling the prefetcher when the readahead window has been full for a while |
| aging/rotting discs read marginally, esp. outer edge | backend zero-fills after retries so the core never hangs, and COUNTS it (stats `BAD`) so it can't masquerade as a clean read. DONE 2026-07-20: speed capped at 4x instead of requesting maximum (see below). further idea if BAD is still nonzero: drop to 2x adaptively once it goes up |
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
