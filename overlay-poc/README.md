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
| `screenshot <name>.png` | (existing verb) capture core-native frame | now instrumented |

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

## Next milestones (not in this branch)

3. daemon + lifecycle (timeout auto-hide, show-while-fresh)
4. POST frame to a vgtranslate/ztranslate-compatible server; text → `osd_msg`,
   image → `overlay_show` (skip PNG/SD: encode from RAM, decode to bank 1)
5. controller hotkey (evdev grab question) replacing the SSH trigger
