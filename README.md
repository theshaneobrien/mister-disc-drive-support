# MiSTer USB CDROM Loader

Load almost any CD game on a MiSTer straight from a USB CD or DVD drive, no ripping. Put a disc in, play it, Music CDs too! If you want, earn RetroAchievements off the physical disc while you are at it.

This is a fork of Main_MiSTer, the ARM/Linux side of MiSTer. All the work is in userspace, the FPGA never touches the drive, and the disc build leaves the cores stock. The ARM binary just answers the core's sector requests from the real disc instead of from a file on the SD card.

*The disc backend that powers other CD loaders.*

## Features

* Play CD games straight from a USB drive, no ripping: MegaCD, PlayStation, Saturn, NeoGeo CD, 3DO, TurboGrafx-CD, and CD-i.
* Disc swapping. A multi disc PlayStation game asks for the next disc, you swap it, it keeps playing. ([Video](https://youtu.be/J477S55O5DE))
* Vib Ribbon with your own music. Boot the game, swap in any album off your shelf. ([Video](https://youtu.be/msHR5iKsleY))
* Audio CDs. Put a music disc in and the MiSTer boots a console's built in CD player. Swap albums whenever. Choose your default console as a player. ([Video](https://youtu.be/r3Z2uLo_iXA))
* Video CDs. Pop a VCD in and it plays through the CD-i core, which turns out to emulate the CD-i digital video hardware. The MiSTer as a Video CD player.
* Karaoke. A CD+G disc draws its lyrics and the colour highlight in time with the music, through the Mega CD or PC Engine, straight off the disc. Your MiSTer does karaoke now.
* CD-i disc swapping. Multi disc CD-i titles and two disc Video CD movies swap disc 1 for disc 2 mid play, no reset.
* The disc is detected automatically and the right core loads.
* Autoboot when you drop a disc in, or a manual Play row in the menu.
* RetroAchievements earned straight off the physical disc, on the RA build (Really depends on your disc and supported hashes).
* Region sorted per core, usually just set it to Auto.
* Acoustic Disc Mirror: pop a spare disc in and the drive spins and seeks along with CHD games you play off the SD card, for the sound of a real console. ([Video](https://youtu.be/H7zMJVK5tPI))

## Two versions, pick one

Two builds on the releases page:

* **Disc.** Plain physical disc support on stock cores. Nothing but the loader.
* **Disc plus RetroAchievements.** The same disc support merged into the MiSTer RetroAchievements build (odelot's fork and its patched cores), so achievements fire from the physical disc. Grab this one if you use RA.

The disc side is identical in both.

## What works

Everything in the above features section! This is still early though so any and all testing and confirmation will be helpful.

For core requests, I've added everything I own discs for, I'm not sure about testing on burnt discs or the word of an AI.

## What you need

* A MiSTer. I have only tested on a DE10-nano. Other users have tested it on a SuperStation and it works for them. No kernel changes, no FPGA changes. Let me know if it works on other devices!
* A USB CD or DVD drive. Most drives that show up as /dev/sr0 work. There is a probe tool (physcd_probe) you can run on the MiSTer to check one first. The two I have tested:
  * Hitachi LG GP60NB60, https://www.amazon.co.uk/dp/B01G33IRYS (This thing works amazingly)
  * LIUAN External CD DVD Drive, model B0260, https://www.amazon.co.uk/dp/B0BB6YCHK4 (This one kinda sucks, the disc tray won't open if its too warm)
* Tested by others
  * SuperDock (Tested by Long-Marsupial3422)
  * LG BP50NB40 CD/DVD/BD (Tested by indigo)
* A decent power supply for the drive. This one matters. A CD drive pulls a big gulp of current spinning up for a seek, and the DE10-nano's USB or a cheap hub will sag under it, drop the drive off the bus, and take your controllers with it for a minute. Use a powered hub, ideally give the drive its own supply. A game that freezes for a minute then carries on by itself is power, not the software.

## Install, three ways

Nothing here renames or replaces a stock file until method 3, and every
method uses the release files under their exact download names.

**Method 1 (recommended) - the full experience, stock files untouched.**
Menu disc detection, autoboot, the Play row, and on-the-fly translation
on every core. One binary on the card, one ini line.

1. From the releases page, put MiSTer-disc on your card at /media/fat/MiSTer-disc (keep the name). Do not touch /media/fat/MiSTer.
2. Add this line inside the `[MiSTer]` section of /media/fat/MiSTer.ini:

```
MAIN=MiSTer-disc
```

Every session (menu and all cores) now runs the disc binary. Official
updates keep updating the stock file harmlessly (it is no longer what
runs); delete the line and you are bone stock again; if the binary goes
missing, MiSTer quietly falls back to stock. RA users: put MiSTer-disc-RA
on the card too (again, keep the name) and route your RA launchers at it:

```
[RA_*]
main=MiSTer-disc-RA
```

(Per-core sections win over the global line, so RA sessions get the RA
build and everything else gets the disc build. An existing MiSTer_RA
setup keeps working unchanged if you'd rather leave it be.)

**Method 2 - scoped: stock everywhere except the sessions you choose.**
Only consoles launched from the _Disc_Cores folder run the disc binary.
You pick the console, the disc mounts itself. No autoboot, no menu disc
detection, and translation only exists inside those disc sessions - by
design: nothing alternate ever runs unless you explicitly launched it.

1. Put MiSTer-disc on your card at /media/fat/MiSTer-disc. Do not touch /media/fat/MiSTer.
2. Unzip Disc-Cores-MGLs.zip from the release to /media/fat (it adds a _Disc_Cores folder).
3. Add this to /media/fat/MiSTer.ini:

```
[CD-*]
main=MiSTer-disc
```

4. Launch a console from _Disc_Cores with your disc in the drive, it mounts on its own. RA users: same [RA_*] section as method 1.

**Method 3 - replace Main wholesale (you know what you're doing).**
The original full install: back up /media/fat/MiSTer, copy MiSTer-disc
over it (RA users: MiSTer-disc-RA over /media/fat/MiSTer and
/media/fat/MiSTer_RA). Same experience as method 1; the difference is
an official Main update overwrites your copy, so you re-copy after
updates. Method 1 exists so you never have to.

All ways: set each CD core's Region to Auto where it has one (see Region), and BIOS files are exactly the ones ripped games use, so if your CD cores already run rips you are done. For the RetroAchievements setup itself use MiSTer Companion or odelot's instructions.

## Region

A disc and the console BIOS have to agree on region or a game refuses to boot or runs at the wrong speed. Per core:

* Saturn: set the core's Region option to Auto in its OSD menu, it reads the region off the disc. One boot.rom.
* PlayStation: name the BIOS files the PSX core's own way, all in games/PSX/: boot.rom = US, boot1.rom = JP, boot2.rom = EU, and keep the core's Region on Auto. The core holds one BIOS per region and the fork points it at the one the disc in the drive asks for, so US, JP, and EU discs each boot on their own BIOS with nothing to set. Missing one? The closest BIOS you do have stands in for it (noted in /tmp/physcd_psx.log) and the game still boots, just on a foreign BIOS. A fixed Region forces both the timing and the BIOS and makes a PAL disc stutter on a US setting, so leave it on Auto.
* NeoGeo CD: a Unibios is region free, so nothing to set. Otherwise the core's system type (CD or CDZ) has to match your BIOS.
* MegaCD: the BIOS is the region. Drop boot_EU.rom, boot_US.rom, and boot_JP.rom in the MegaCD home folder and the matching one loads. A plain boot.rom is the catch all. Some setups also have an Auto region option, try that first.

Short version: use Auto where a core has it, and provide the region named BIOS files using each core's OWN naming (PlayStation and MegaCD name theirs differently, see above). Each CD core needs its own BIOS in its home folder, same as for a ripped image.

## Using it

Drive plugged in, disc inside:

* Autoboot on, the default: insert a disc at the menu and it works out the game, loads the core, mounts the disc. A disc left in also relaunches on the next power on.
* Manual: scroll to the bottom of the core list, there is a Play row showing the game and console, like "Play: SONIC CD - Mega CD". No disc reads "Insert Disc". PlayStation discs with no title show their serial, like SLES-01234.

For scripts there is a fifo command:

```
echo mount_phys > /dev/MiSTer_cmd
```

That mounts the drive into whatever CD core is already running.

Cores I've tested with:
* 3DO_20260717.rbf
* CDi_20260502.rbf
* MegaCD_20260603.rbf
* NeoGeo_20260603.rbf
* PSX_20260411.rbf
* Saturn_20251003.rbf
* TurboGrafx16_20260603.rbf
* All current RA Cores

## Disc swapping

Multi disc games work with real discs. On PlayStation you play until the game asks for the next disc, eject, put the next one in, and the game carries on by itself. Tested with Final Fantasy VII on retail discs, it just notices, no buttons, no menus.

The one I'm most happy with is Vib Ribbon. Boot the game disc, then swap in any music CD you own and it builds levels from your album. Exactly like the real PlayStation, shelf of CDs and all.

Saturn multi disc games take one extra step. The Saturn runs a disc change through its BIOS, so after you swap it checks the new disc and offers Start Application. Select that and the game carries on. Saturn multi disc games save before a swap as part of their normal flow, so nothing is lost. The console even reports Drive Door Open while the tray is out, which is a nice touch. Not sure how D plays like this tbh.

Swaps are detected automatically from the physical eject and insert. On PlayStation there is also a fifo fallback if a game ever misses one:

```
echo swap_phys > /dev/MiSTer_cmd
```

## Audio CDs

The MiSTer is now a CD player. Put a music CD in at the menu and it boots into a console's built in CD player, the same player the real hardware shipped with. PlayStation by default, and PHYSCD_AUDIO_CORE in MiSTer.ini picks the Saturn, Mega CD, NeoGeo CD, or TurboGrafx-CD player instead if you prefer one of those.

Swap albums live and the player picks up the new disc and its track list. The start of an audio disc is buffered while the drive spins up, so track one comes in clean.

## Karaoke (CD+G)

A CD+G disc is an ordinary music CD with karaoke graphics hidden in its subchannel. Drop one in and it boots the Mega CD player by default, which reads those graphics off the disc and draws the lyrics with the colour highlight sweeping along in time with the song. Set PHYSCD_CDG_CORE=TurboGrafx16 for the PC Engine player instead. Both need their core (and BIOS) installed, same as playing a game on them.

The Philips CD-i played CD+G too, and the fork feeds it the same data, but the released CD-i core does not draw it yet (the support is in the CD-i source, just not in a built core as of writing). A newer CD-i core will light it up with no change here.

## Settings

In MiSTer.ini, in their own [physcd] section:

```
[physcd]
PHYSCD_AUTOBOOT=1     ; 1 auto-loads a disc at the menu (default), 0 is manual, Play row only
PHYSCD_MOUNT_DELAY=2  ; seconds to let a core settle before mounting, raise it if a game misses the disc
PHYSCD_DEVICE=        ; optional, pin one drive such as /dev/sr1, blank means autodetect
PHYSCD_ACOUSTIC=0     ; 1 = a spare disc in the drive spins and seeks along with image games, see below
PHYSCD_AUDIO_CORE=PSX ; which console's CD player an audio disc boots: PSX, MegaCD, Saturn, NeoGeo, or TurboGrafx16
PHYSCD_VCD_CORE=CDI   ; which core a Video CD boots: CDI plays them, others are there to experiment with
PHYSCD_CDG_CORE=MegaCD ; which core a CD+G karaoke disc boots: MegaCD or TurboGrafx16 draw the graphics
```

They also work under [MiSTer], but keeping them in their own section means any other MiSTer binary on your card (like MiSTer Companion's MiSTer_RA) skips them quietly instead of popping unknown option warnings at boot.

## Acoustic seek / Disc Mirroring

This is really stupid, but I love it. When you play a game from the SD card (a chd), the binary already knows what part of the disc the game is reading. With PHYSCD_ACOUSTIC=1 it mirrors that onto a real drive: pop any spare disc in and it spins up, seeks, and changes speed in step with the game, so an emulated game gets the sound of a real console.

It only makes noise, it does not read anything the game needs, so the disc is throwaway. For the fullest sound use a full data CD-R (Mode 1, closed session), one data track burned to the edge, so the drive can read and seek across the whole platter. It pauses at the menu and never touches a disc you are actually playing.

## RetroAchievements

The RA build is the disc loader merged into the MiSTer RetroAchievements setup, so it wants the RA patched cores, not stock ones. That setup is [odelot's RetroAchievements fork](https://github.com/odelot/Main_MiSTer) of Main_MiSTer, and our RA build sits on top of it. All credit for the RA side belongs there.

Easiest way in is [MiSTer Companion](https://mistercompanion.org), which has a RetroAchievements Cores option that installs odelot's binary and the patched cores for you, which is how I set mine up. There is also a script installer, [mister-fpga-retroachievements](https://github.com/manyhats-mike/mister-fpga-retroachievements). Either way, get RA working with a ripped game first, then drop the Disc plus RetroAchievements binary over /media/fat/MiSTer. It prefers the patched core when it boots a disc, so achievements fire.

One real limit: RA matches a game by hashing the disc, and the hash has to match a set in the RA database, which is built against the unmodified retail disc. Retail discs RA already supports work. Anything that needs a patch to earn its achievements will not match, because we read the disc exactly as pressed. Europe Sonic CD is one of those.

To check a game first, search it on the RetroAchievements site and open Supported Game Hashes, which lists the discs the set was built against with their region and revision. Not a guarantee, games have several pressings, but it is the quickest way to know. An unlisted disc still plays, it just starts no session.

Set the core Region to Auto. Softcore confirms a game identifies, hardcore is stricter but fine once it is recognised.

## It is a fork, and it stays one

Upstream MiSTer has said they will not support USB CD drives, so this is probably never getting folded back in. It follows Main_MiSTer loosely and will lag at times, so a newer main feature might mean rebasing it yourself. The changes are kept small and behind a physcd flag on purpose, so that is not too painful. I'll be updating once a month or if something really cool happens.

## Building

The build runs in Docker with the same ARM toolchain Main_MiSTer uses, so you do not need a cross compiler installed. On Windows:

```
.\tools\build.ps1
```

The binary lands in Main_MiSTer\bin\MiSTer. The disc build is on the physcd branch, the RetroAchievements build on physcd-ra.

## How it fits together

The backend lives in support/physcd. It reads raw 2352 byte sectors over SG_IO, keeps a prefetch cache split into game data and CD audio streams so mixed mode discs do not stall, and hides seek latency behind the timing the cores already model. Per core the change is tiny: one branch in the read path that pulls from the drive instead of a file, plus region and BIOS handling, all guarded so normal cue and chd loading is unchanged. On the RA build one more branch points the achievement hash reader at the same physical sectors.

## License

physcd is a fork of [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer), combined with code from [odelot's Main_MiSTer](https://github.com/odelot/Main_MiSTer) for the RetroAchievements build. Both are under the GNU General Public License, version 3 or, at your option, any later version. So this is too: GNU GPL v3 or later. The full text is in [LICENSE](LICENSE).

Copyright (C) 2026 Shane O'Brien. Portions Copyright (C) 2005 to 2012 Dennis van Weeren, Jakub Bednarski, Till Harbaum, and the MiSTer-devel contributors.

The MiSTer binary bundles a few third party libraries, each keeping its own license: rcheevos and miniz (MIT), libchdr and zstd (BSD, zstd also under GPLv2), imlib2 (permissive BSD style), the LZMA SDK, libco, and md5 (public domain), and the BlueZ bluetooth headers (GPL v2 family). Those stay governed by their own license files.
