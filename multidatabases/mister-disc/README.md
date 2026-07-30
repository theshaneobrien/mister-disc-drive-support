# MiSTer Disc database

- Database ID:
  [`MultiDatabases/mister-disc`](https://theypsilon.github.io/DB-Inspector_MiSTer/?database-url=https%3A%2F%2Fraw.githubusercontent.com%2Ftheypsilon%2FMultiDatabases_MiSTer%2Fdb%2Fmister-disc%2Fdb.json)
- Upstream: [`theshaneobrien/mister-disc-drive-support`](https://github.com/theshaneobrien/mister-disc-drive-support)
- Database URL:
  `https://raw.githubusercontent.com/theypsilon/MultiDatabases_MiSTer/db/mister-disc/db.json`

Play CD games on a MiSTer straight from a USB CD or DVD drive, no ripping.
Seven consoles from real discs (PlayStation, Saturn, Mega CD, Neo Geo CD, 3DO,
TurboGrafx-CD, and CD-i), plus audio CDs, Video CDs, and CD+G karaoke. Live
disc swapping (multi-disc RPGs, Monster Rancher, Vib Ribbon with any album),
per-disc memory cards and regional BIOS on PlayStation, disc integrity
telemetry, and RetroAchievements earned from the physical disc on the RA
build. And since v0.7.0: **on-the-fly translation** - press a button in a
game and the dialogue comes back in English, drawn inside the game's
own text boxes, on any core. ARM-side Main binary only: the FPGA cores stay
completely stock, nothing is forked or patched, and the official cores must
already be installed. The generator follows the latest upstream GitHub
release.

*The disc backend that powers other CD loaders.*

## Installation

Download
[`downloader_MultiDatabases_mister-disc.zip`](https://raw.githubusercontent.com/theypsilon/MultiDatabases_MiSTer/db/mister-disc/downloader_MultiDatabases_mister-disc.zip),
extract it to `/media/fat` on the MiSTer SD card, and run the MiSTer
updaters. The files land under their release names and nothing stock is
touched; one line in MiSTer.ini switches the features on. Two ways:

**Full experience (recommended).** Menu disc detection, autoboot, the Play
row, and translation on every core. Add this line inside the `[MiSTer]`
section of `/media/fat/MiSTer.ini`:

```ini
MAIN=MiSTer-disc
```

Every session (menu and all cores) now runs the disc binary, while
`/media/fat/MiSTer` stays bone stock and keeps taking official updates
harmlessly. Delete the line and you are stock again; if the binary is ever
missing, MiSTer quietly falls back to stock.

**Scoped.** Stock everywhere except the sessions you explicitly launch:
only consoles started from the `_Disc_Cores` menu folder run the disc
binary, and you pick the console yourself (no autoboot, no menu disc
detection, no translation outside those sessions). Add this section
instead:

```ini
[CD-*]
main=MiSTer-disc
```

Either way: use a USB optical drive that MiSTer exposes as `/dev/sr0`, and
each core needs its own BIOS files exactly as it would for ripped games
(PlayStation uses `games/PSX/boot.rom` = US, `boot1.rom` = JP, `boot2.rom`
= EU); the database includes no BIOS or game files.

## Translation setup (optional, off by default)

Nothing runs until you do this. You need a free
[ztranslate.net](https://ztranslate.net) account and the full-experience
install above.

1. Run `/media/fat/translate/translate_start.sh install` once (creates the
   settings file and hooks the boot start).
2. Put your ztranslate API key and `ENABLED=1` in
   `/media/fat/translate/translate.ini`.
3. OSD > Scripts > **SetTranslateHotkey**, press the button or combo you
   want.
4. Reboot. In game: press the hotkey, read, press anything to continue.

The full guide is on the card at `translate/README.md`.

## RetroAchievements

Point your RA launchers at the installed RA build by using this section in
MiSTer.ini:

```ini
[RA_*]
main=MiSTer-disc-RA
```

odelot/MiSTer Companion setups that route through `/media/fat/MiSTer_RA`
can instead copy the installed `MiSTer-disc-RA` over `MiSTer_RA` (back up
the original first) - both routes end with achievements earned from the
physical disc.

## Updating from the first release of this database

File names changed to match the upstream release names (they are
referenced by your ini lines, so they are now permanent): `MiSTer_Disc` is
now `MiSTer-disc`, `MiSTer_Disc_RA` is now `MiSTer-disc-RA`, and the
launcher folder `_Disc Cores` is now `_Disc_Cores`. The updater removes
the old files automatically - you only need to update your MiSTer.ini
lines to the new names (and delete the old empty `_Disc Cores` folder if
one lingers).
