# Architecture — what runs where, and why

The feature is deliberately split across four layers. Each piece lives
where it does for a hardware-verified reason, not by accident.

```
 [button]                [SD card]                    [cloud]
    │                        │
    ▼                        ▼
 Main binary ──"go"──▶ translate_daemon.py ──HTTPS──▶ ztranslate.net
 (hotkey watch,        (capture DDR3 frame,           (OCR + translate
  overlay verbs,        PNG, POST, route reply)        + render image)
  OSD, fb present)           │
    ▲                        │ overlay_show /tmp/translated_N.png
    └────── /dev/MiSTer_cmd ─┘
```

## 1. Main binary (fork branch `overlay-poc`, C++)

Everything that *must* be in Main because it owns the hardware paths:

| Piece | Where | Why it can't live anywhere else |
|---|---|---|
| `osd_msg` verb | `input.cpp` dispatch → `Info()` (`menu.cpp`) | the OSD is SPI-driven and single-writer, owned by Main |
| `overlay_show/hide/shot` verbs | `input.cpp` → `video.cpp` | the fb banks, scaler SPI (`UIO_SET_FBUF`) and imlib context are Main's |
| `overlay_present()` | `video.cpp` | letterboxes into the scaler's true output rect (read from the DDR3 header) so freeze-frames land exactly over the live picture; cached-shadow + single memcpy = 2.5× faster than direct writes to the uncached mapping |
| hotkey watcher | `input.cpp` `overlay_hotkey()` in `input_cb` | Main `EVIOCGRAB`s every input device during play (`grabbed = 1`); nothing else can see buttons. Fires by writing `go` to the daemon fifo. Gated: never fires with OSD open, in the menu core, or on a terminal fb (only over live gameplay or its own overlay = re-translate) |
| any-button dismiss | same | needs the same event stream + `video_overlay_state()` (bank-1-while-game = ours; excludes F9 console and menu backgrounds). EV_KEY + d-pad hats only — sticks/gyro can never dismiss |
| `perf_log` | `perf_log.{h,cpp}` | µs telemetry to `/tmp/overlay_perf.log`, shared timebase with the daemon (both CLOCK_MONOTONIC); boot line stamps the build (compile time, can't go stale) |
| screenshot fixes | `scaler.cpp` | stock latent race: `getFullPath`'s shared static buffer used on the offload worker — path now resolved on the main thread; `imlib_free_image_and_decache` (path-keyed cache) |

Config read by Main: `/media/fat/translate/hotkey.cfg` (falls back to the
old `/media/fat/overlay/` path), format `314+315` | `68` | `learn`.

## 2. Python daemon (`mister/translate_daemon.py`, stdlib-only)

Everything network- and policy-shaped, kept out of Main so iteration
never needs a reflash:

- **Capture**: its own passive mmap of the scaler buffer at `0x20000000`
  — opened **`O_SYNC`** (rule below), tear-checked via the frame counter,
  staleness-guarded (refuses rather than ship a frozen frame).
- **Aspect fix**: row-doubles anamorphic hi-res modes (SNES 512×224) so
  OCR sees sane glyph shapes.
- **PNG encode**: minimal stdlib writer (zlib level 1).
- **Backend**: the RetroArch AI-Service protocol, one URL swap between
  the hosted ztranslate.net (de-facto), a LAN vgtranslate/mock, or an
  eventual self-hosted server (milestone 11). A direct-Google backend
  existed briefly and was cut 2026-07-30 — one protocol, one path.
- **Reply routing**: image → unique `/tmp/translated_N.png` (unique
  *path* per round — see imlib rule) → `overlay_show`; text → wrapped
  30-col, OSD-placed opposite the OCR bounding box.
- **Policy**: `MIN_INTERVAL` quota guard, `pause/resume` (auto-resume so
  a crashed script can't wedge it), auto-hide-before-recapture, api_key
  redaction in logs.
- **Trigger**: fifo `/tmp/translate_cmd`
  (`go|image|text|hide|pause|resume|quit`) — used by both the human and
  Main's hotkey, identically.
- **Settings**: `translate.ini` (flat KEY=VALUE), precedence CLI > ini >
  defaults.

## 3. Bash (`mister/translate_start.sh`, `Scripts/SetTranslateHotkey.sh`)

Lifecycle and setup UX — things that are naturally shell:

- `translate_start.sh`: boot start (gated on `ENABLED=1`, pgrep-deduped),
  `install` (idempotent line into `/media/fat/linux/user-startup.sh`, the
  update-safe hook), `stop` (guarded — see fifo rule).
- `SetTranslateHotkey.sh` (OSD Scripts menu): while a script runs, Main
  *releases* its input grab — so the script reads evdev directly, detects
  a press or held-combo, writes `hotkey.cfg`. Pauses the daemon while
  listening (pressing the current hotkey would otherwise fire a real
  translation of the script terminal — happened on hardware).

## 4. Config files

| File | Consumer | Content |
|---|---|---|
| `/media/fat/translate/translate.ini` | daemon (+ `ENABLED` grepped by start script) | service, key, languages, mode, quota guard |
| `/media/fat/translate/hotkey.cfg` | Main binary | evdev codes / `learn` |

## File locations

| Repo | SD card |
|---|---|
| `mister/*` | `/media/fat/translate/` |
| `Scripts/SetTranslateHotkey.sh` | `/media/fat/Scripts/` |
| `dev/mock_server.py` | any PC (port 4404, PIL optional) |
| `dev/test.sh` | anywhere on the MiSTer (display milestones 0–2) |
| Main fork | branch `overlay-poc`, release asset `MiSTer-disc-overlaypoc` |

Runtime artifacts (all `/tmp`, RAM-only): `overlay_perf.log`,
`translate_cmd` (fifo), `translated_NNNNNN.png` (previous unlinked).

## Hardware-derived rules (each cost a debugging session)

1. **`O_SYNC` on `/dev/mem`, always.** The FPGA writes DDR behind the
   CPU; a cacheable mapping serves stale lines indefinitely, across
   process restarts. Main's `shmem_map` always did this; the daemon
   learned it the hard way.
2. **Never write a fifo unguarded.** Writes block forever without a
   reader (`stop` hung an ssh session); pgrep-guard + `timeout 2`.
   And never *create* trigger paths with `echo` before `mkfifo` — a
   regular file replays its content forever (`ensure_fifo`).
3. **imlib2 caches decoded images by file path.** Rewriting one filename
   showed the first decode forever. Decache on free (Main) *and* unique
   paths per round (daemon).
4. **Verify fixes at the symptom level.** Two stacked staleness bugs
   (CPU cache + imlib cache) made each single fix look failed.
5. **Quota guards are not optional** with API keys on disk: min-interval
   + pause + fifo hygiene all exist because a replay loop once burned
   ~40 ztranslate calls in a minute.
6. **The scaler mux replaces, never blends.** Full-color over live video
   is impossible without RTL changes (`ascal.vhd` discards fb alpha);
   freeze-frame + OSD text are the two honest modes.
7. **Assume busybox.** The MiSTer rootfs has no `pgrep`/`pkill` (and
   `timeout` is not guaranteed): scan `/proc/*/cmdline` for process
   checks, `kill` by pid, and `command -v` before optional tools.
