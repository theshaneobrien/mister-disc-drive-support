# Development notes (PoC history)

> Historical record of the overlay PoC: milestones 0–6, hardware
> findings, telemetry numbers, and the incident log. Paths in here refer
> to the PoC-era `/media/fat/overlay`. Current user docs: [README](../README.md);
> current design: [ARCHITECTURE](ARCHITECTURE.md).

# Overlay PoC — on-the-fly translation for MiSTer

## User setup (the whole thing, post-PoC)

1. Flash the `MiSTer-disc-overlaypoc` binary from the latest release, reboot.
2. Copy to the SD card: `translate_daemon.py`, `translate_start.sh`,
   `translate.ini` → `/media/fat/overlay/`; `Scripts/TranslateHotkey.sh`
   → `/media/fat/Scripts/`.
3. Edit `/media/fat/overlay/translate.ini`: paste your ztranslate.net
   `API_KEY`, set `SOURCE_LANG`/`TARGET_LANG` (`ZT_MODE=fast` trades
   quality for speed), set `ENABLED=1`.
4. One-time boot hook: `/media/fat/overlay/translate_start.sh install`
   (appends one line to the update-safe `/media/fat/linux/user-startup.sh`).
5. Set your hotkey from the OSD: Scripts → **TranslateHotkey** → press the
   button (or hold one + press a second for a combo). Gyro/sticks ignored.
6. Reboot. Load a Japanese game. Press the hotkey. Read. Press anything
   to continue playing.

Settings precedence: CLI args > `translate.ini` > defaults, so manual
`python3 translate_daemon.py --once ...` testing still works unchanged.

---

## Development history — display milestones 0/1/2

Spike for a RetroArch-AI-Service-style translation feature on MiSTer. This
branch (`overlay-poc` in `Main_MiSTer/`) proves the **display half** — the
hard part — before any OCR/network code exists. Everything is driven over
SSH through the existing `/dev/MiSTer_cmd` FIFO, so no hotkey/evdev work is
needed to test.

## What was added (branch `overlay-poc`, built on `physcd` tip)

| Verb | Does | Mechanism |
|---|---|---|
| `osd_msg [-t ms] [-x n] [-y n] [-f 1\|2] <text>` | Text **over the live game** — game keeps running | OSD info window (`Info()` → `InfoEnable`), FPGA-composited, works on every core |
| `overlay_show [testpat\|<image>]` | Full-color image — **replaces** game video until hidden | HPS framebuffer bank 1 + `video_fb_enable(1,1)`; input keeps routing to the core |
| `overlay_hide` | Back to core video | `video_fb_enable(0)` |
| `overlay_shot` | Freeze-frame: capture current frame → show it, **all in RAM** (v2) | scaler read → imlib scale into cached shadow → one memcpy into bank 1 |
| `screenshot <name>.png` | (existing verb) capture core-native frame | now instrumented |

## Hardware findings (2026-07-29, PSX core, 1080p output, DE10-Nano)

First run on real hardware confirmed the research predictions almost exactly —
and found the real wall:

| Stage | Measured | Verdict |
|---|---|---|
| `osd_msg` render+SPI | **1.8 ms** | text-over-game is essentially free |
| scaler mmap init | 0.2 ms | free |
| DDR3 frame read (512×224) | **3.8–4.3 ms** | matches Screenshot_MiSTer's 3–5 ms |
| `UIO_SET_FBUF` scaler switch | **39 µs** | free |
| vsync latency | 8–11 ms | one frame, as expected |
| testpat full-1080p draw | **196 ms** | ⚠ the wall: ~42 MB/s into the uncached fb mapping |
| PNG encode + SD write | 27–169 ms | placeholder cost — eliminated by `overlay_shot` |

Bar order was correct → the ARGB/RxB pixel-format assumption is validated.

Two bugs found by the first run, fixed in v2:
- **PNG save raced `getFullPath`'s shared static buffer** (stock latent bug —
  `write_screenshot` ran on the offload worker while the main loop rewrote
  the same buffer; seen as `ok=0`, no file). Path is now resolved on the
  main thread; save failures now log `imlib_err` to the perf log.
- test.sh waited on a stale log line from a previous run and displayed a
  half-written PNG (`err=14`); the wait is now count-based and checks `ok=1`.

Open performance question the v2 `overlay_shot` A/B answers: is the 42 MB/s
fb write rate the *mapping* (O_SYNC/strongly-ordered `/dev/mem`) or the
*access pattern*? `blit(cached)` vs `copy(ddr)` in its log line splits it.
Quick mitigation to try regardless: `fb_size=2` in MiSTer.ini quarters the
fb to 960×540 (~4× fewer bytes; the low-res game art loses nothing).

- `Info()` gained optional `x, y` position args (defaults unchanged) and
  returns whether it displayed — `menu.cpp` / `menu.h`, all existing callers
  untouched.
- `perf_log.{h,cpp}`: µs-precision telemetry → **`/tmp/overlay_perf.log`**
  (tmpfs). Always on, one line per event, `tail -f`-able. Stages timed:
  OSD render+SPI, testpat draw, PNG load, blit, scaler switch, vsync,
  screenshot mmap/DDR3-read/PNG-encode/SD-write, request→file total.
- `\n` in `osd_msg` text = line break; `-f 1` draws the dialogue-box frame.
- `osd_msg` x/y units are `InfoEnable` units (defaults 20,10) — the test
  script probes `-y 200` to map where a subtitle bar sits.

## Build & deploy

```powershell
.\tools\build.ps1 -Clean       # -Clean: branch switch invalidates objcache
```

```bash
scp Main_MiSTer/bin/MiSTer root@<mister>:/media/fat/MiSTer
ssh root@<mister> "sync; reboot"
```

## Run

Start a **game core** (not the menu), then:

```bash
ssh root@<mister> 'bash -s' < overlay-poc/test.sh
```

or by hand:

```bash
echo 'osd_msg -f 1 Translated:\nHERO: I found the sword!' > /dev/MiSTer_cmd
echo 'overlay_show testpat' > /dev/MiSTer_cmd
echo 'overlay_hide' > /dev/MiSTer_cmd
tail -f /tmp/overlay_perf.log
```

## What the numbers mean / expected ballparks

- `osd_msg render+spi` — full text-over-game cost. Expect ~1–2 ms.
- `screenshot read` — the DDR3 frame copy (NEON). Screenshot_MiSTer measured
  ~3–5 ms; this is the number the whole translation pipeline inherits.
- `screenshot total` — request → PNG on SD. Includes up-to-one-frame wait
  (capture runs on the vsync callback) + PNG encode on the worker thread.
  For the real feature the PNG/SD leg disappears (frame goes RAM → network).
- `overlay_show total` — draw + `UIO_SET_FBUF` SPI + one vsync ≈ upper bound
  for "translated image on glass".

## Known limits (by design of the stock gateware)

- The color framebuffer **replaces** core video (scaler source mux, alpha
  discarded in `ascal.vhd`) — a translucent color HUD over live gameplay
  needs RTL changes. Text-over-game is the OSD's job (1-bpp, hardcoded
  palette, 8×8 font, ASCII only — translations must be to English/Latin).
- `overlay_show` needs the core to include the HPS-framebuffer reader;
  refusal is logged (`enable REFUSED`). OSD path works everywhere.
- While the overlay is up the game **keeps running** and controller input
  still reaches it (bank 1 ↔ `input_switch(1)`) — blind inputs possible.
  Deliberate: bank 0 would steal input *and* belongs to the Linux console.
- `osd_msg` is suppressed (and logged) while the OSD menu is open.
- Test pattern doubles as a pixel-format check: bar order white → black as
  listed in the script; red/blue swap on screen = RxB assumption wrong.

## Milestone 4 — the server loop (v3)

`translate_daemon.py` (runs on the MiSTer, stdlib-only python3) captures the
frame **itself** from the scaler DDR3 buffer (no Main involvement, ~no PNG/SD),
POSTs it RetroArch-AI-Service-style, and routes the reply: `text` → `osd_msg`
over the live game, `image` → `overlay_show /tmp/translated.png` freeze-frame.
`mock_server.py` (runs on any PC, port 4404 = vgtranslate's default) answers
the protocol with a canned translation — swap the URL for a real
vgtranslate/ztranslate later and nothing else changes.

Quickstart:

```bash
# on the PC:
python3 mock_server.py                  # pip install pillow for drawn image mode

# on the MiSTer (game core running):
python3 /media/fat/overlay/translate_daemon.py --server http://<pc-ip>:4404 &
echo image > /tmp/translate_cmd         # freeze-frame with mock translation bar
echo text  > /tmp/translate_cmd         # OSD text over the live game
echo hide  > /tmp/translate_cmd
```

One-shot form (no daemon): `python3 translate_daemon.py --server http://<pc>:4404 --once --mode text`

Coordinate story (v3): `overlay_show`/`overlay_shot` now letterbox into the
scaler's TRUE output rect (from the same DDR3 header), so freeze-frames land
pixel-on-pixel over the live picture instead of stretching to full screen,
and OCR coords in capture space map linearly to fb space. Anamorphic hi-res
modes (SNES 512×224) get row-doubled before upload so OCR sees sane glyph
proportions. OSD `-x/-y` are pre-scale-domain units — calibratable, but
character-cell-approximate; when placement must be exact, image mode is the
tool. Multiple simultaneous OSD messages: stock = one info window (a second
`Info()` replaces the first); the window is a 32×16-char canvas that could
host multiple text blocks, but the FPGA tints the whole window blue — for
RPG-menu-style scattered labels, image mode is the right answer.

## Real translation with NO self-hosted server (milestone 6)

Most people won't run a server, so both cloud paths skip it entirely:

**Option A — ztranslate.net (full image mode, zero code changes).** The
hosted service by the RetroArch AI-Service author; does OCR + translation
+ renders the translated frame server-side. Free account at ztranslate.net
→ API key → run the daemon with their URL (paste it exactly as their docs
give it; our `output`/lang params append cleanly):

```bash
python3 translate_daemon.py --server "https://ztranslate.net/service?api_key=KEY" &
echo image > /tmp/translate_cmd
```

**Option B — `--backend google` (direct-to-cloud, OSD text mode).** The
daemon calls Google Vision (OCR) + Google Translate REST APIs straight
from the MiSTer — no middleman at all. console.cloud.google.com → enable
the Vision and Translation APIs → create an API key. Free tier: 1,000 OCR
calls + 500k translated chars/month. The reply arrives as an OSD toast
over the *running* game, word-wrapped for the 32-char window and
auto-placed opposite the detected text (OCR bounding box top-half →
toast at the bottom, and vice versa):

```bash
python3 translate_daemon.py --backend google --google-key AIza... &
echo go > /tmp/translate_cmd
```

TLS note: python's urllib falls back to the rootfs CA bundle
(/etc/ssl/certs/cacert.pem) if the default CA path is empty on the image.

## Milestone 5 — hotkey + any-button dismiss (v6, `overlaypoc6`)

The EVIOCGRAB question answered itself: Main grabs every input device
exclusively while a core runs (`input.cpp` `grabbed = 1`), so the hotkey
lives in Main's `input_cb` — the one point every event from every device
passes through, keyboard or pad alike.

**Config — `/media/fat/overlay/hotkey.cfg`** (one line; loaded at core
start, since core loads re-exec Main):

| Content | Meaning |
|---|---|
| `314+315` | combo: fires when both held (SELECT+START on most pads) |
| `68` | single key (68 = F10 on a keyboard) |
| `learn` | log every pressed button's evdev code to the perf log |
| *(no file)* | hotkey off; dismiss stays active |

Common codes: `BTN_SELECT=314` `BTN_START=315` `BTN_TL=310` `BTN_TR=311`
`BTN_THUMBL=317` `BTN_THUMBR=318` `KEY_F10=68` `KEY_PAUSE=119`.

Behavior, by design:
- Firing writes `go` to the daemon fifo — byte-identical to the manual
  `echo`, so translation stays **strictly on-demand** (no polling exists
  anywhere; the daemon's 2s min-interval still guards the API quota).
- **Any other button press or d-pad hat movement clears the shown
  translation** — and the same press still reaches the game, so "press A
  to continue" advances the dialogue and drops the old text in one go.
  All other `EV_ABS` (analog sticks, gyro/accelerometer streams — DS4
  etc.) are deliberately ignored and can never dismiss mid-read.
- The hotkey is *observed, not consumed*: the core still sees the
  presses. Pick codes your game ignores (or a keyboard key). True
  consumption needs delayed forwarding — future refinement.

## Next milestones

7. ~~image mode without a render server (freetype `overlay_text` +
   Vision boxes)~~ **CUT 2026-07-30**: it depended on the google
   backend for OCR box coordinates; ztranslate returns finished
   images (no boxes), and an own server (11) renders server-side
   anyway — client-side rendering solves a problem we no longer have
8. side-by-side install via MGL launchers instead of replacing
   /media/fat/MiSTer — the physcd sidecar already proved the mechanism
   (`main=` ini routing + alternate binary name + `routed_main()`
   basename detection); the overlay binary could ship the same way
9. ~~google backend A/B~~ **CUT 2026-07-30**: ztranslate is the
   de-facto backend (Shane); the google code path + ini keys were
   removed from the daemon the same day — one protocol, one path
10. MultiDatabases distribution (after 8): the disc project's entry
    (shipped with physcd v0.6.0) is the template — the downloader
    natively distributes file trees shaped like our install zip, and
    the binary ships as a side-by-side alternate main
11. **self-hosted, shareable ztranslate-compatible server**: a docker
    image anyone can run (unraid/Pi/VPS) speaking the AI-Service
    protocol — OCR (manga-ocr, game/manga-tuned) + local MT (Sugoi V4
    or NLLB-int8) + PIL in-place rendering. mock_server.py is the
    protocol seed; the research priced the stack at ~1.5GB models on
    Pi-4-class hardware. Bonus: it serves stock RetroArch users too
    (same protocol), which makes it worth sharing on its own
