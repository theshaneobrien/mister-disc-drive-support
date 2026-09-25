# MiSTer On-The-Fly Translation

Press a button mid-game; a second later the screen freezes with the
Japanese text replaced by English, rendered inside the game's own dialogue
boxes. Press anything to keep playing. No PC, no self-hosted server — the
MiSTer talks directly to a cloud translation service.

Works on any core the HPS framebuffer reaches (the big consoles all do).
Powered by a patched Main binary + a small Python daemon speaking the
RetroArch AI-Service protocol to [ztranslate.net](https://ztranslate.net).

## What you need

- The `MiSTer-disc-overlaypoc` build of Main (release page) — stock Main
  lacks the overlay/hotkey support
- A free [ztranslate.net](https://ztranslate.net) account → API key
  (~20,000 calls/month free; one translation ≈ 2)
- Network on the MiSTer, python3 (stock rootfs has it)

## Install

| Repo file | Goes to |
|---|---|
| `mister/translate_daemon.py` | `/media/fat/translate/` |
| `mister/translate_start.sh` | `/media/fat/translate/` (`chmod +x`) |
| `mister/translate.ini` | `/media/fat/translate/` |
| `Scripts/SetTranslateHotkey.sh` | `/media/fat/Scripts/` (`chmod +x`) |

1. Install the release binary — for translation you want it running on
   EVERY core, which means **method 1** from the disc project README
   (recommended): put `MiSTer-disc` on the card at
   `/media/fat/MiSTer-disc` (keep the download name, no renaming) and
   add one line inside the `[MiSTer]` section of `MiSTer.ini`:

   ```
   MAIN=MiSTer-disc
   ```

   Every session (menu and all cores) then runs it; `/media/fat/MiSTer`
   stays bone stock and keeps getting official updates harmlessly.
   Delete the line to revert; if the binary is missing, MiSTer silently
   stays stock. First line of `/tmp/overlay_perf.log` confirms the
   routing (the stock binary writes no stamp).

   Method 3 (replace `/media/fat/MiSTer` outright) works identically
   for translation; method 2 (scoped `[CD-*]` + `_Disc_Cores` MGLs)
   runs the binary only in disc sessions, so translation would exist
   on the CD cores only — fine if that's what you want, but not the
   translate-everywhere experience.
2. Copy the files as above.
3. Edit `/media/fat/translate/translate.ini`: paste your `API_KEY`,
   set `ENABLED=1`.
4. `/media/fat/translate/translate_start.sh install` — hooks the daemon
   into boot (one line in the update-safe `/media/fat/linux/user-startup.sh`).
5. OSD → Scripts → **SetTranslateHotkey** → press your button (or hold
   one and press a second for a combo).
6. Reboot. Done — load a game and press the hotkey.

## Using it

- **Hotkey** → freeze-frame with the translation rendered in place
  (~1.5s round trip).
- **Any button / d-pad** → back to the game (the press also reaches the
  game — "A to continue" advances the dialogue *and* clears the text).
  Analog sticks and gyro never dismiss.
- **OSD button** also dismisses.
- The game keeps running underneath the frozen frame — don't hold
  directions while reading.

Manual triggers (SSH), all optional:

```
echo image  > /tmp/translate_cmd     # same as the hotkey
echo text   > /tmp/translate_cmd     # OSD toast instead of freeze-frame
echo hide   > /tmp/translate_cmd
echo pause  > /tmp/translate_cmd     # suspend triggers (auto-resumes in 120s)
echo resume > /tmp/translate_cmd
echo quit   > /tmp/translate_cmd
/media/fat/translate/translate_start.sh stop
```

## On a CRT (analog RGB)

The translated screen is drawn by MiSTer's scaler, and a normal CRT setup
(`vga_scaler=0`) sends the analog output around the scaler entirely - so the
translation runs, gets logged, and never reaches the tube. The fix is to put
the scaler on the analog output and give it a 15 kHz mode a TV can show. In
MiSTer.ini:

```ini
[MiSTer]
vga_mode=rgb
composite_sync=1
vga_scaler=1
video_mode=320,15,30,35,240,4,4,14,6293
vscale_mode=1
```

Reboot after editing - MiSTer only reads the ini when a core loads.

- That mode is standard 240p: 15.73 kHz, 262 lines, 60 Hz. It is 320 wide
  on purpose: the scaler assumes square pixels, so a 640-wide 240-line mode
  comes out squashed into half the screen.
- `vscale_mode=1` keeps the game's lines 1:1 on the tube (a 224-line game
  gets thin borders instead of being stretched).
- The trade: the analog picture now goes through the scaler instead of being
  the core's native output, and HDMI will not display this mode (the pixel
  clock is below the HDMI minimum).
- Just one core? Put those lines under its own section (`[SNES]`, `[3DO]`...)
  instead of `[MiSTer]`, and the menu and everything else keep their native
  output.
- It is 60 Hz, which suits Japanese games. PAL 50 Hz games need a different
  mode.
- `vsync_adjust=1` is fine from v0.8.0. Older builds blanked the top of the
  translated screen a moment after it appeared - use `vsync_adjust=0` there.

Would rather keep your native picture? Leave `vga_scaler=0` and set
`MODE=text` in translate.ini: the translation appears as text in the OSD box
instead (English only - the OSD font is ASCII). That is also the only route
on S-Video and composite, which are built from the core's raw output and can
never carry the translated screen (text mode is untested there so far).

## Settings (`/media/fat/translate/translate.ini`)

| Key | Default | Meaning |
|---|---|---|
| `ENABLED` | `0` | boot autostart on/off (manual runs ignore it) |
| `API_KEY` | | your ztranslate key (auto-appended to SERVER) |
| `SERVER` | ztranslate.net/service | any AI-Service-protocol endpoint |
| `SOURCE_LANG` | `ja` | source language; blank = service auto-detect (less reliable) |
| `TARGET_LANG` | `en` | output language |
| `ZT_MODE` | *(blank)* | blank = service default. `fast` = quicker, lower quality |
| `MODE` | `image` | `image` freeze-frame / `text` OSD toast |
| `MIN_INTERVAL` | `2.0` | min seconds between translations (quota guard) |
| `OSD_MS` | `8000` | text-mode display time |
| `LABEL` | auto | what to call the game in server logs; blank means read it from MiSTer |

CLI args override the ini (`python3 translate_daemon.py --help`).

## Telling the server what you are playing

Every frame is sent with a label, which a self-hosted server can use to
report timings and translation quality per game rather than in one
average. The daemon works it out from what MiSTer already publishes: the
core name from `/tmp/CORENAME`, plus a disc serial or the loaded
filename, giving labels like `PSX__SLES-01370` or `SNES__Fire Emblem`.

Disc serials and CRC32s need one line under `[MiSTer]` in `MiSTer.ini`:

```
log_file_entry=1
```

Set `LABEL=` in `translate.ini` to override it with a fixed string.

## Troubleshooting

Everything logs to **`/tmp/overlay_perf.log`** (`tail -f` it):

- First line after boot = which Main build is flashed (compile stamp).
- `hotkey: fired but no daemon` → daemon not running (`ENABLED=1`? then
  `translate_start.sh` or reboot).
- `HTTP 500` from the backend → a server-side error, usually transient
  (one hardware outbreak resolved on its own; every `ZT_MODE` /
  `SOURCE_LANG` permutation later tested clean). Wait and retry; to
  prove the MiSTer side is healthy, point `--server` at the mock
  server for one round.
- `capture FAILED ... not updating` → overlay still shown or no core
  running.
- Hotkey does nothing in the menus/terminal — by design; it only fires
  over live gameplay (or over its own overlay = re-translate).

## Limitations

- The freeze-frame **replaces** the game picture while shown (the FPGA
  scaler can't alpha-blend the framebuffer over live video); the game
  keeps running underneath.
- OSD text mode is ASCII only (8×8 hardware font) — fine for English out.
- On analog RGB the translated screen needs the scaler on the analog
  output — see [On a CRT](#on-a-crt-analog-rgb). S-Video and composite
  can only use `MODE=text`.
- The hotkey press also reaches the game — pick buttons your game ignores.
- Ornate/stylized fonts (brush-style title screens) may defeat the OCR;
  regular dialogue boxes work well.
- Needs the core to include HPS-framebuffer support (logged as `enable
  REFUSED` if missing).

## Migrating from the PoC path (`/media/fat/overlay`)

```bash
mv /media/fat/overlay /media/fat/translate
sed -i 's|/media/fat/overlay|/media/fat/translate|g' \
    /media/fat/translate/translate_start.sh \
    /media/fat/translate/translate_daemon.py \
    /media/fat/Scripts/SetTranslateHotkey.sh \
    /media/fat/linux/user-startup.sh
```

The release binary reads `hotkey.cfg` from the new path first and falls
back to the old one, so a half-migrated setup still works.
