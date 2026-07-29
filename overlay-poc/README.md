# Overlay PoC — on-the-fly translation, display milestones 0/1/2

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

## Next milestones (not in this branch)

5. controller hotkey (evdev grab question) replacing the SSH/FIFO trigger
6. real server: vgtranslate (Google Vision+Translate keys) or an
   Interpreter-style offline stack (manga-ocr + Sugoi) on the LAN PC
