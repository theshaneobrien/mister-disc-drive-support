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

1. Flash the release binary to `/media/fat/MiSTer`, reboot.
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
| `BACKEND` / `GOOGLE_KEY` | `service` | `google` = direct Vision+Translate, OSD text only |

CLI args override the ini (`python3 translate_daemon.py --help`).

## Troubleshooting

Everything logs to **`/tmp/overlay_perf.log`** (`tail -f` it):

- First line after boot = which Main build is flashed (compile stamp).
- `hotkey: fired but no daemon` → daemon not running (`ENABLED=1`? then
  `translate_start.sh` or reboot).
- `HTTP 500` from the backend → check `SOURCE_LANG` is set (`ja`) and
  `ZT_MODE` is blank; sending `mode=normal` and/or auto-detect produced
  consistent 500s on hardware.
- `capture FAILED ... not updating` → overlay still shown or no core
  running.
- Hotkey does nothing in the menus/terminal — by design; it only fires
  over live gameplay (or over its own overlay = re-translate).

## Limitations

- The freeze-frame **replaces** the game picture while shown (the FPGA
  scaler can't alpha-blend the framebuffer over live video); the game
  keeps running underneath.
- OSD text mode is ASCII only (8×8 hardware font) — fine for English out.
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
