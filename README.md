# MiSTer USB CDROM Loader

Load almost any CD game on a MiSTer straight from a USB CD or DVD drive, no ripping. Put a disc in, play it. And if you want, earn RetroAchievements off the physical disc while you are at it.

This is a fork of Main_MiSTer, the ARM/Linux side of MiSTer. All the work is in userspace and the FPGA never touches the drive. On the plain disc build the cores stay stock too. The RetroAchievements build uses RA's patched cores, but the disc side is identical. Either way the ARM binary just answers the core's sector requests from the real disc instead of from a file on the SD card.

## Heads up, this was written with AI

Most of the code here was written with an AI assistant (Claude), with me steering it, reviewing every change, and testing on real hardware. I am not hiding that. It boots real games, it has earned achievements off a spinning disc, and it has been through adversarial code review. Still, treat it like any hobby fork: keep your working MiSTer binary as a backup before you swap this one in.

## Two versions, pick one

There are two builds on the releases page:

* **Disc.** Plain physical disc support on stock cores. Nothing but the loader.
* **Disc plus RetroAchievements.** The same disc support, merged into the MiSTer RetroAchievements build (odelot's fork and its patched cores). Achievements fire from the physical disc. Grab this one if you use RA.

Both do the disc side exactly the same. The only difference is whether RetroAchievements comes along for the ride.

## What works

* MegaCD, PlayStation, Saturn, NeoGeo CD, and 3DO games boot and play from disc, CD audio tracks included where the core supports them.
* The disc is identified automatically, so the right core loads for whatever you put in.
* RetroAchievements straight from the disc, on the RA build, for the cores RA supports (MegaCD, PlayStation, Saturn, PC Engine CD, NeoGeo CD). The disc is read once at load, hashed, and matched to the RA database exactly like an image would be. I have earned achievements on PS1 games with nothing but the disc in the drive.
* Region is handled per core, see the Region section below.
* Autoboot. Drop a disc in at the menu and it loads the right core and mounts the disc, hands free.
* Manual mode. A Play row at the bottom of the core list, so you load a disc when you want instead of on insert.

PC Engine CD and CD-i can be added the same way, I just have no discs to test with. Swapping discs mid game works, but it is untested for the games that actually need it.

## What you need

* A MiSTer. I have only tested on a DE10-nano. No kernel changes, no FPGA changes.
* A USB CD or DVD drive. Most drives that show up as /dev/sr0 will work. There is a small probe tool (physcd_probe) you can run on the MiSTer to check a drive before you trust it. The two I have tested and can vouch for:
  * Hitachi LG GP60NB60, https://www.amazon.co.uk/dp/B01G33IRYS
  * LIUAN External CD DVD Drive, model B0260, https://www.amazon.co.uk/dp/B0BB6YCHK4
* A decent power supply for the drive. This one really matters. A CD drive pulls a big gulp of current when it spins up for a seek, and the DE10-nano's own USB or a cheap unpowered hub will sag under it. When that happens the drive falls off the USB bus mid game, and everything else on that hub, controllers included, goes quiet for a minute until it recovers. Use a powered hub, and ideally give the drive its own supply. If you ever see a game freeze for a minute and then carry on by itself, that is power, not the software.

## Setup

1. Grab the binary from the releases page, Disc or Disc plus RetroAchievements. Back up your current /media/fat/MiSTer first, then drop the new one in its place and reboot.
2. Set each CD core's Region to Auto where it has the option, see Region below.

On BIOS files, I did not touch any of mine. It all just worked, set up through MiSTer Companion. If your CD cores already run ripped games then the BIOS files are already where they need to be, so stick with what you have. People running a MiSTer tend to know their own setup best.

## Region

A disc and the console BIOS have to agree on region or a game either refuses to boot or plays at the wrong speed. Each core handles this differently, so:

* Saturn: the core has its own Region option. Set it to Auto in the core's OSD menu and it reads the region off the disc. One boot.rom is all you need.
* PlayStation: set the core Region to Auto, same as Saturn. The fork reads the region off the disc and sends it to the core, so on Auto it boots PAL, NTSC-U, and NTSC-J discs with nothing to set. Forcing the core to a fixed region overrides that, which is what makes a PAL disc stutter on a US setting, so leave it on Auto.
* NeoGeo CD: use a Unibios and it is region free, so nothing to set. Otherwise the core's system type (CD or CDZ) has to match your BIOS.
* MegaCD: the BIOS is the region. Drop boot_EU.rom, boot_US.rom, and boot_JP.rom into the MegaCD home folder and the matching one loads automatically. A plain boot.rom still works as a catch all. Some MegaCD setups also have an Auto region option in the core, so try that first if you have it.

Short version: if a core has an Auto region setting, use it. Otherwise provide the region named BIOS files above.

Each CD core needs its own BIOS in that core's home folder, same as you would for a ripped image: MegaCD boot.rom, PlayStation BIOS, Saturn boot.rom, and a NeoGeo CD BIOS (uni-bioscd.rom, or top-sp1.bin and neocd.bin).

## Using it

Drive plugged in, disc inside:

* Autoboot on, the default: insert a disc at the menu, it works out the game, loads the core, and mounts the disc. Nothing to press. A disc left in the drive also relaunches on the next power on, so if you leave a game in, the MiSTer boots straight into it.
* Manual: scroll to the bottom of the core list. There is a Play row showing the game and console, for example "Play: SONIC CD - Mega CD". Select it to boot. With no disc in the drive it reads "Insert Disc". PlayStation discs with no readable title show their serial instead, like SLES-01234.

For scripts there is a command on the fifo:

```
echo mount_phys > /dev/MiSTer_cmd
```

That mounts the drive into whatever CD core is already running.

## Settings

In MiSTer.ini, under [MiSTer]:

```
PHYSCD_AUTOBOOT=1     ; 1 loads a disc automatically at the menu (default), 0 is manual, Play row only
PHYSCD_MOUNT_DELAY=2  ; seconds to let a core settle before mounting, raise it if a game does not see the disc
PHYSCD_DEVICE=        ; optional, pin one drive such as /dev/sr1, blank means autodetect
```

## RetroAchievements

The RA build is the disc loader merged into the MiSTer RetroAchievements setup, so it wants the RA patched cores, not stock ones. That whole setup is [odelot's RetroAchievements fork](https://github.com/odelot/Main_MiSTer) of Main_MiSTer, and our RA build sits right on top of it. All the credit for the RetroAchievements side belongs there.

The easy way to get it onto your MiSTer is [MiSTer Companion](https://mistercompanion.org), which has a RetroAchievements Cores option that installs odelot's binary and the patched cores for you. That is how I set mine up and it just worked. If you would rather use a script there is also [mister-fpga-retroachievements](https://github.com/manyhats-mike/mister-fpga-retroachievements), which installs the same odelot build. Either way, get RetroAchievements working with a ripped game first so you know the stack is set up, then drop the Disc plus RetroAchievements binary over /media/fat/MiSTer. When it boots a disc the loader prefers the patched core, so achievements can actually fire.

One real limit worth knowing. RetroAchievements matches a game by hashing the disc, and that hash has to match the set in the RA database, which is built against the unmodified retail disc. Retail discs that RA already supports work. Anything that needs a patch to get its achievements will not match from a physical disc, because we read the disc exactly as pressed and cannot change it. Europe Sonic CD is one of those, its set needs a patch, so it will not identify off the disc.

Worth checking before you count on a game. On the RetroAchievements site, search for the game, open its page, and click Supported Game Hashes. That lists the exact discs the set was built against, usually with the region and revision noted, so you can see if your copy is likely to match. It is not a cast iron guarantee, games have several pressings and revisions and only the listed ones will identify, but it is the quickest way to know before you burn or insert a disc. If yours is not listed it will still boot and play, it just will not start an achievement session.

Set the core Region to Auto, same as always. Softcore is the easy way to confirm a game identifies. Hardcore is stricter but works fine once the game is recognised.

## It is a fork, and it stays one

Upstream MiSTer has said they will not support USB CD drives, so this is probably never getting folded back in. It is a fork and it will stay one. It follows Main_MiSTer loosely and will lag behind at times. If you need a newer main feature you may have to rebase it yourself. The changes are kept small and tucked behind a physcd flag on purpose, so rebasing is not too painful.

## Building

The build runs in Docker with the same ARM toolchain Main_MiSTer uses, so you do not need a cross compiler installed.

On Windows:

```
.\tools\build.ps1
```

The binary lands in Main_MiSTer\bin\MiSTer. The disc build is on the physcd branch, the RetroAchievements build on physcd-ra.

## How it fits together

The backend lives in support/physcd. It reads raw 2352 byte sectors from the drive over SG_IO, keeps a prefetch cache split into two streams (game data and CD audio) so mixed mode discs do not stall, and hides real seek latency behind the timing the cores already model for themselves. Per core the change is tiny: one branch in the read path that pulls from the drive instead of a file, plus the region and BIOS handling. It is all guarded, so normal cue and chd loading behaves exactly as it did before. On the RA build one more branch points the RetroAchievements hash reader at the same physical sectors, so a disc identifies with no image file anywhere.

## License

physcd is a fork of [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer), combined with code from [odelot's Main_MiSTer](https://github.com/odelot/Main_MiSTer) for the RetroAchievements build. Both are under the GNU General Public License, version 3 or, at your option, any later version. So this is too: GNU GPL v3 or later. The full text is in [LICENSE](LICENSE).

Copyright (C) 2026 Shane O'Brien. Portions Copyright (C) 2005 to 2012 Dennis van Weeren, Jakub Bednarski, Till Harbaum, and the MiSTer-devel contributors.

The MiSTer binary bundles a few third party libraries, each keeping its own license: rcheevos and miniz (MIT), libchdr and zstd (BSD, zstd also under GPLv2), imlib2 (permissive BSD style), the LZMA SDK, libco, and md5 (public domain), and the BlueZ bluetooth headers (GPL v2 family). Those stay governed by their own license files.
