# Analog overlay — why the framebuffer cannot reach a CRT, and what we can do about it

> Decision document. Written 2026-07-31 after a bug report: Sony
> Trinitron over analog RGB from a MiSTer IO board, `vga_scaler=0`.
> Translation ran, the daemon logged a successful round trip, and
> nothing appeared on screen. This is not a bug in our code. It is the
> gateware's output mux, and it is not fixable in the direction we were
> pushing.
>
> **Update 2026-09-25: resolved on hardware - see section 0. Section 4's
> recommendation is superseded; the analysis in sections 1-3 held.**

## 0. Outcome (hardware-tested 2026-09-25)

Tested on an OSSC fed analog RGB over SCART (Ultimate MiSTer VGA-to-SCART),
SNES core, the reporter's exact configuration (`vga_scaler=0`,
`direct_video=0`, `vga_mode=rgb`, `composite_sync=1`,
`forced_scandoubler=0`). The OSSC sits on the same pins a CRT does and reports
the input mode, which made it a better instrument than a tube.

**What held:** section 1, exactly. `vga_scaler=0` plus image mode shows no
overlay. The raw-path OSD - `osd_msg` and `MODE=text` - does reach analog RGB,
the first time either was seen on a 15 kHz signal.

**What changed:** Option A works for 15 kHz RGB, given the right mode.
`vga_scaler=1` with `video_mode=320,15,30,35,240,4,4,14,6293` read on the OSSC
as `RGBS 262p 15.73kHz 60.05Hz` - genuine 240p - carrying the colour overlay.
The width has to be 320: `ascal` assumes square output pixels, and 640x240 on
a 4:3 tube is 2:1, so a 640-wide mode draws a 4:3 picture only 320 pixels
wide. `vscale_mode=1` (lines 1:1) and scoping the block to a core section
(`[SNES]`) both worked.

**A Main bug, found and fixed (`9a8c513`).** With `vsync_adjust=1` the top of
the overlay went black about a frame after it appeared. Enabling the
framebuffer changes the FB parameters; `get_video_info` counts that as a video
change; `video_mode_adjust` answers with a full mode re-set - of an identical
mode, since `vtime` never moved - whose `video_fb_config` and Linux fb module
reconfig blanked the top. The perf log showed `fb: config after mode set`
24 ms after `overlay_show`, and `vsync_adjust=0` made it vanish. An overlay
toggle that leaves the core's `vtime`/width/height alone is no longer a video
change. The same re-set would have mis-fitted `vscale_mode` 4/5 (it reads
`fb_width/height` as the core size while `fb_en`), and it can hit HDMI users
with `vsync_adjust=1` too.

**Revised recommendation:** document Option A for RGB CRT users
(translate/README.md, "On a CRT"), with `MODE=text` as the keep-your-native-
picture alternative and the only route for S-Video/composite. Option B stays
rejected. Option D (detect + automatic text fallback) is still worth doing,
but it is no longer the only answer for RGB.

**Still unverified:** a real 15 kHz tube (the reporter's Trinitron is the
test; the test rig's own SCART TV sits in composite mode for want of a pin-16
voltage); `MODE=text` on real S-Video/composite; a 50 Hz 288-line variant for
PAL games.

## 1. The mechanism

Our full-colour overlay is written into the HPS framebuffer
(`Main_MiSTer/video.cpp`, `video_overlay_show` → `overlay_present` →
`video_fb_enable`). The framebuffer is composited by `ascal` — the
scaler — and by nothing else. That single fact decides everything below.

The FPGA carries two independent video chains from the core to the
outside world:

- The **raw path**: core video → `scanlines` (sys_top.v:1383) → `vga_osd`
  (sys_top.v:1403) → `yc_out` (sys_top.v:1447) or `vga_out`
  (sys_top.v:1494) → `vga_o`.
- The **scaler path**: core video → `ascal` (which is where the HPS
  framebuffer is mixed in) → `hdmi_osd` → `vgas_o`.

The analog pins choose between them at sys_top.v:1523-1525, and the
selector is one line above:

```verilog
// sys_top.v:1516
wire vgas_en = vga_fb | vga_scaler;
// sys_top.v:1523-1525
assign VGA_R = vgas_en ? vgas_o[23:18] : vga_o[23:18];
```

So `VGA_R/G/B` carry the scaler output — the only output that contains
our overlay — if and only if `vgas_en` is high. With `vga_scaler=0` and
`direct_video=0`, which is the normal 15 kHz RGB-to-CRT configuration,
`vgas_en` is low and the overlay physically cannot reach the analog
pins. There is no ARM-side pixel operation that changes this, because
the framebuffer's only consumer is `ascal` (`LFB_*` from
`UIO_SET_FBUF` feeds the `ascal` instance at sys_top.v:791 and nothing
else).

Sorgelig has stated the same thing directly, on atari-forum thread
37945: "Linux video output (frame buffer) is controlled by scaler."

Two consequences that are easy to miss and that shape the options:

**The overlay and Y/C output are mutually exclusive in gateware.**
sys_top.v:1511 makes `vga_o` the `yc_out` encoder output when `yc_en`,
and `yc_out` is fed from `vga_data_osd` — the raw path. The scaler never
reaches the Y/C encoder. Because the pin mux at 1523 *prefers* `vgas_o`
whenever `vgas_en`, raising `vgas_en` to get the overlay discards the
S-Video/composite encoding entirely. The official MiSTer CRT
documentation independently lists `vga_scaler=0` as a *required* setting
for Y/C, so this is not an accident we might route around; it is the
documented supported configuration. An S-Video user cannot be told to
"just turn the scaler on" — doing so costs them their picture.

**The OSD is a separate, second compositor, and it is on the right side
of the mux.** sys_top instantiates two `osd` modules: `vga_osd` in the
raw path and `hdmi_osd` in the scaler path. That is why MiSTer menus are
visible on a 15 kHz CRT today, and why `yc_out.din = vga_data_osd`
(sys_top.v:1447) means OSD content also survives into composite and
S-Video. The OSD is the only ARM-drivable pixel source on the raw path.
I enumerated the raw chain block by block and read the entire UIO
command decoder (sys_top.v:410-530) to check this: `scanlines` is
core-controlled (`.VGA_SL(scanlines)`, sys_top.v:1786), `shadowmask` and
`gamma` exist only in the scaler path, and not one UIO command carries
pixel data to the raw chain. The only raw-path command is 0x41, which
sets colourburst and subcarrier constants.

**There is one runtime switch, and it is not a compositor.**
sys_top.v:295 is `wire vga_fb = cfg[12] | vga_force_scaler;`. Config bit
12 is ARM-settable — `CONF_VGA_FB` at `user_io.h:153`, applied in
`user_io_send_buttons` at `user_io.cpp:3022`, with a setter already
written at `user_io.cpp:2986`. Asserting it forces `vgas_en` high
without the user editing MiSTer.ini. That is a *routing* change, not a
new drawing surface: it swaps the whole analog output over to scaler
timings. Upstream already calls it, but only for one class of user —
`video.cpp:3629` is `if (cfg.direct_video) set_vga_fb(enable);`. Our
reporter has `direct_video=0`, so that path never fires for them.

## 2. Where the overlay can and cannot go

`vga_fb` below means config bit 12 as asserted by `set_vga_fb`.

| Output configuration | `vgas_en` | Colour overlay reaches it? | What it costs the user |
|---|---|---|---|
| HDMI | n/a — HDMI is always fed by the scaler | Yes | Nothing. This is the configuration the PoC was built and measured on. |
| Analog RGB, `vga_scaler=1` | 1 (from `vga_scaler`) | Yes | The analog output now carries the scaler's video mode, not the core's. Verified working here on a Compaq MV740 with `video_mode=6` (640×480@60). A 15 kHz-only set needs a 15 kHz-capable `video_mode` on top, and we have **not** tested that combination. Also adds the scaler's latency and resampling to a path that was previously pixel-native. |
| Raw analog RGB, `vga_scaler=0`, `direct_video=0` | 0 | **No** | This is the bug. Picture is fine; the overlay simply never appears. This is the reporter's configuration. |
| S-Video (`vga_mode=svideo`, `vga_scaler=0`) | 0 | **No, and cannot be made to** | Raising `vgas_en` would route `vgas_o` to the pins and discard the `yc_out` encoding — the user loses colour and sync on their television, not just the overlay. `vga_scaler=0` is documented as required for Y/C. |
| Composite (`vga_mode=cvbs`, `vga_scaler=0`) | 0 | **No, and cannot be made to** | Same as S-Video. |
| `direct_video=1` (HDMI-to-analog dongle) | 0 → 1 when the overlay shows | Yes, via the existing `video.cpp:3629` call | The overlay appears, but sys_top.v:1317-1320 switches the DAC from the direct-video path to the scaler path at the same instant. A dongle user on plain RGB sees a mode change; a dongle user on Y/C (`dv_data_osd` carries the Y/C encoding at sys_top.v:1257) sees their S-Video picture collapse — colour loss, resolution change — for the duration of the overlay. If a tester reports this, do not diagnose it as a new bug. |

## 3. The options

### Option A — document it: tell affected users to set `vga_scaler=1`

**To build:** nothing. A README paragraph.

**Costs the player:** their analog output stops being pixel-native. They
adopted `vga_scaler=0` deliberately — that is the entire point of a
15 kHz CRT setup — and we would be asking them to give up the reason
they bought the IO board so that a translation overlay can appear.
They must also find a `video_mode` their set will sync to, which is
fiddly and which we have not tested on a 15 kHz set; the only
confirmation we have is a 640×480@60 VGA monitor.

**Does not help:** every S-Video and composite user, absolutely and
permanently. For them `vga_scaler=1` is not a trade-off, it is a
regression to no usable picture. It also does not help anyone whose CRT
will not sync to a mode the scaler can produce.

This is what upstream already does. `menu.cpp:847` is `vga_nag()`: when
`video_fb_state()` is true it draws "If you see this, then you need to
modify MiSTer.ini … or enable scaler on VGA: `vga_scaler=1`" into the
**VGA-side** OSD and deliberately leaves it enabled there while
disabling the HDMI one. Upstream detected exactly our condition,
concluded it could not be fixed in the framebuffer, and fell back to the
OSD to apologise. That is the strongest available evidence that we are
not missing a trick.

It also means `vga_nag()` is an active hazard for us: it runs on every
`MENU_NONE1` transition and fires whenever `video_fb_state()` is true,
which in a game core is simply `fb_enabled` (`video.cpp:3634-3641`) —
i.e. exactly while our image-mode overlay is up. The reporter is very
likely already being shown that nag page, and it will overwrite
anything we draw in the VGA OSD.

### Option B — assert `CONF_VGA_FB` only while an overlay is up

Drop or widen the `if (cfg.direct_video)` condition at
`video.cpp:3629` so that `set_vga_fb(1)` is called when the overlay
shows and `set_vga_fb(0)` when it hides, for analog users generally.

**To build:** small. One condition, plus a guard (see below). The
mechanism is already written and already exercised by direct_video
users. No gateware change, no core change, which is the only kind of
fix our constraint permits.

**Costs the player:** more than it first appears. The analog output
switches to scaler timings for the duration of the overlay and back
again afterwards — two mode changes per translation. If the user's
scaler mode is the usual 720p or 1080p, a 15 kHz set does not display a
worse picture, it loses sync entirely: the screen goes black, then
comes back when the overlay hides. We would have converted "the overlay
is invisible" into "the screen goes dark for four seconds and I never
see the translation", which is worse, not better. Even where the mode is
compatible, CRTs take a second or more to relock, and some 15 kHz sets
will not relock cleanly at all.

**Does not help, and actively harms:** S-Video and composite users.
Raising `vgas_en` discards the Y/C encoding, so their working picture
breaks every time a translation appears. Any implementation of this
option **must** refuse when `cfg.vga_mode_int >= 2` — Main knows this
(`cfg.cpp:654-660` maps `svideo`→2, `cvbs`→3, and `video.cpp:3314` already
branches on it), so the guard is cheap, but shipping without it would be
a regression for a whole class of user.

Honest summary: this option is only correct for a user whose scaler is
already configured to emit a mode their CRT accepts — and such a user
could equally have set `vga_scaler=1` themselves. It buys automation and
revertibility, not new capability. It is the option most likely to
generate bug reports from people it was meant to help.

### Option C — an OSD-based text overlay

**The code exists. It has never been used for this.** Be precise about
what that means, because the distinction decides how much of this
option is real and how much is inference:

* **Proven.** The `osd_msg` verb (input.cpp) → `Info()` (menu.cpp)
  works and is in live use today for the "AI: no text found" toast
  (translate_daemon.py:293). DEV-NOTES.md measures it at **1.8 ms**
  render plus SPI on hardware and documents a worked dialogue-box
  example. The mechanism is not speculative.
* **Inferred, not observed.** That the raw-path OSD reaches the analog
  pins with `vga_scaler=0` follows from `vga_osd` sitting in the raw
  chain (sys_top.v:1403) and `yc_out` being fed from `vga_data_osd`
  (sys_top.v:1447). It is the same OSD instance that draws the menu
  every CRT user already sees, so the architecture is not in doubt —
  but nobody here has watched a translation appear on a 15 kHz display.
* **Untested entirely.** The daemon's `MODE=text` path
  (translate_daemon.py:438, `osd_show` at :257) has never been run
  end-to-end with real translation output. The project settled on
  image mode early and text mode was left as a code path nobody
  exercised. Its legibility, line wrapping, how `sanitize_osd`'s ASCII
  fold treats real translated dialogue, and how a two-speaker screen
  reads at 32×16 cells are all unknown.

So this option is a *reopening of a closed decision*, not a switch
waiting to be flipped. It is only worth reopening because it is the
sole mechanism that reaches this class of user at all.

**What the hardware gives you, exactly:** the positionable info window
is capped ARM-side at 32×16 character cells (`INFO_MAXW` / `INFO_MAXH`,
`osd.cpp:422-423`), i.e. 256×128 pixels, one bit per pixel, column-major
(`osd.cpp:86-89`). Colour is a single compile-time `OSD_COLOR`
parameter baked into each core's gateware; `grep -rn "OSD_COLOR" Main_MiSTer/`
returns nothing, because the ARM has no colour command at all. The
OSD composites in the raw path at `clk_vid`, so OSD pixels are *core*
pixels and osd.v's multiscan factor keeps the box a constant fraction of
the picture — a higher-resolution core buys zero extra detail.
Main_MiSTer issues #386 and #761 both asked for OSD colour; neither
produced it.

**To build, if we want more than text:** roughly 150 lines, all
ARM-side. `osdbuf`, `osdbufpos` and `osd_start()` are file-static in
`osd.cpp`, so an arbitrary 1-bpp bitmap needs one new exported function
(`OsdWriteRaw(line, bytes, len, xoff)`), one `Info()`-shaped wrapper
that calls `InfoEnable` and reuses `MENU_INFO` for teardown, one
`osd_bmp` verb beside `osd_msg`, and a PIL `convert("1")` render in the
daemon. The nearest existing precedent is the star-field
`framebuffer_plot()` at `osd.cpp:88`, which is already this exact
layout.

**Costs the player:** monochrome, one tint, 8×8 font, no alpha, nothing
outside a single 256×128 rectangle, and Latin script only — which
constrains translations to English or another Latin-alphabet target.
A 256×128 raster of proportional text is measurably *worse* than the
hardware font at that size, so the bitmap work is likely not worth it;
patching `charfont[]` at runtime (it is non-const, `charrom.h:4`, and
`LoadFont()` already does the transposition, `charrom.cpp:197-216`) is
the cheaper route to better glyphs.

**Does not help:** anyone who wanted the translated *image* — menus with
scattered labels, text baked into artwork, anything where placement
must be exact. The FPGA tints the whole info window one colour, so
multiple simultaneous labels are not a thing. It is a genuinely
different feature, not a lower-fidelity version of the same one.

**Known conflicts, all in existing code:** an open menu suppresses
`Info()` outright (`menu.cpp:8009`, and our verb already logs
`SUPPRESSED (menu open)`); opening the menu while a translation is up
falls through `MENU_INFO` → `MENU_NONE2` and `OsdClear()` wipes our
content with no notification to the daemon; the `MENU_INFO` timeout
*is* the hide path, so anything persistent must re-arm before it fires
and must cope with `osd_size` dropping back to 8 (`OsdUpdate` transmits
exactly `osd_size` lines, `osd.cpp:666`); and volume toasts, controller
info, screenshot results and core-load errors all share the same single
buffer and will replace our content mid-sentence.

### Option D — detect and warn, change nothing else

Main knows `cfg.vga_scaler`, `cfg.direct_video` and `cfg.vga_mode_int`,
and `video_fb_state()` already exists. The condition "this user's
framebuffer overlay cannot reach their screen" is a one-line predicate.

**To build:** trivial in Main. The useful version is slightly more: have
the daemon read the same facts and fall back to `MODE=text`
automatically instead of silently succeeding into a void. The daemon
already has a working text path; `translate.ini:29` is `MODE=image`,
and that default is the whole reason the reporter saw nothing.

**Costs the player:** nothing, if we honour the house rule — a warning
that repeats is a nag, and per project convention any OSD tip ships
once ever via a persistent marker under `/media/fat/config` or it does
not ship. Upstream's `vga_nag()` fires on every menu transition, which
is exactly the behaviour we have decided not to have.

**Does not help:** nobody gets a colour overlay. It converts a silent
failure into an explained one, which is worth doing regardless of what
else we choose, but it is not a fix.

## 4. Recommendation

**Ship D plus the automatic text fallback now. Do not ship B. Treat C's
bitmap extension as optional and probably unnecessary.**

The reasoning: the framebuffer overlay cannot reach a raw analog path,
and that is a property of the gateware we are forbidden to change. Every
existence proof of ARM-originated full-colour video on a 15 kHz CRT —
Groovy_MiSTer, MiSTerCast, MiSTer_Frontier — ships its own core and in
Groovy's case its own kernel. Under "stock cores, alternative Main
only", none of those approaches transfers. So the honest framing is not
"how do we get the overlay to the CRT" but "what do we give a CRT user
instead, and how do we stop lying to them in the meantime".

The lie is the immediate harm. Today the daemon defaults to
`MODE=image`, completes a round trip, logs success, and displays
nothing. That costs the user an API call and their confidence in the
feature. Making Main expose "can the framebuffer reach this display"
and having the daemon fall back to `MODE=text` when it cannot is the
largest gain available for the smallest cost, and the only option that
helps Y/C users at all.

**But cost that honestly.** The `osd_msg` verb underneath it is
measured at 1.8 ms and in daily use; the text *mode* on top of it has
never been run end-to-end, so "hours of work" is the integration
estimate and not a claim that the feature is sitting there working.
Before any of this is promised to a user, somebody has to watch a real
translation render in a 32×16 monochrome cell grid and decide whether
it is worth having. That test costs an afternoon and it gates the whole
recommendation — if two-speaker dialogue is unreadable at that size,
Option D's warning is all that is left and the honest answer to a CRT
user becomes "this feature is not for your display".

Option B is the one I want to talk you out of. It is technically elegant
— the mechanism is already there, it is per-overlay and revertible, and
it needs no ini edit — but it only works for a user whose scaler is
already emitting a CRT-compatible mode, and such a user could have set
`vga_scaler=1` themselves. For everyone else it turns an invisible
overlay into a black screen and two sync losses per translation, and
without a `cfg.vga_mode_int >= 2` guard it breaks working S-Video
pictures outright. If we ever ship it, it should be opt-in, off by
default, documented as "only if your scaler mode already displays on
your CRT", and guarded against Y/C.

Option A stays in the README as the honest statement of what the
hardware supports, not as advice we push.

**The losers, named.** S-Video and composite users never get the colour
overlay; the gateware forbids it and no configuration change helps them.
15 kHz RGB users get it only by abandoning `vga_scaler=0`, which is the
reason they built their setup. Anyone whose translation *needs* to be an
image — scattered menu labels, text inside artwork, non-Latin target
languages — is not served by the text fallback and will remain an
HDMI-only case. And `direct_video` users on a Y/C dongle have a
different, currently unaddressed problem: their picture breaks when the
overlay appears, courtesy of the existing upstream call at
`video.cpp:3629`.

## 5. What is still unverified, and how to settle it

**Does the reporter see `vga_nag()` on their Trinitron?** `vga_nag()`
fires whenever `video_fb_state()` is true, which is true whenever our
image-mode overlay is up. If they are seeing MiSTer's own "modify
MiSTer.ini" page appear on their CRT during a translation, that
confirms the diagnosis end-to-end and confirms the raw-path OSD reaches
them. Ask them to look. One question, no code.

**Does the OSD actually reach composite and S-Video?**
Architecturally proven — `yc_out.din = vga_data_osd`, sys_top.v:1447 —
but never seen on hardware here. This is the strongest argument for the
whole text-fallback route, so it deserves a real test. The ini delta
from the reporter's current configuration is exactly one line,
`vga_mode=rgb` → `vga_mode=svideo`; everything else they already have
(`composite_sync=1`, `vga_scaler=0`, `forced_scandoubler=0`,
`direct_video=0`, `ntsc_mode=0`) is the documented Y/C configuration
character for character. It needs a Y/C adapter, or a bare four-wire
cable with sync-on-green enabled — `yc_out` drives luma to zero outside
active video and emits sync on a separate port, so a naive three-wire
cable has no sync at all and will look like a cable fault.

**Will a 15 kHz set sync to any mode the scaler can produce?** Our only
confirmation of the scaler carrying the overlay to analog is a Compaq
MV740 at 640×480@60, which is a VGA monitor. Option B's viability rests
entirely on this question and it is currently unanswered. Settle it by
setting `vga_scaler=1` with a 15 kHz `video_mode` on the reporter's
Trinitron before writing any code for B.

**Units of `InfoEnable`'s x/y** (`osd.cpp:508-517`). Width and height
are definitely character cells; x/y default to 20,10 with a
direct_video special case of y=30 (`menu.cpp:8013-8014`), which reads
more like pixels. `translate/dev/test.sh` already probes `-y 200`; one
hardware run settles it, and subtitle placement depends on the answer.

**Whether the gateware has `OSD_HDR` set.** The ARM writes 19 lines in
the menu core (`osd.cpp:666`) and 18 in a non-menu core
(`menu.cpp:3442`), implying yes, but `OsdClear()` only clears 16
(`osd.cpp:497`). Anything above 16 lines is inferred, not proven. Stay
at 16 until someone verifies it.

**Cost of pushing a full 16-line bitmap.** The measured 1.8 ms covers
`osd_msg`'s typical short window. Sixteen lines is 4096 byte-at-a-time
SPI writes (`spi.cpp:180-192`) on the single main-loop thread. Fine at
one update per second or two; anything approaching per-frame needs
measuring against the main-loop budget, and we already have one known
instance of a long main-loop operation starving input.