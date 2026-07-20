# MiSTer USB CDROM Loader

Load MegaCD and PlayStation games on a MiSTer straight from a USB CD or DVD drive, no ripping. Put a disc in, play it.

This is a fork of Main_MiSTer, the ARM/Linux side of MiSTer. All the work is in userspace. Cores and RBF files stay stock, the FPGA never touches the drive. The ARM binary just answers the core's sector requests from the real disc instead of from a file on the SD card.

## Heads up, this was written with AI

Most of the code here was written with an AI assistant (Claude), with me steering it, reviewing every change, and testing on real hardware. I am not hiding that. It boots real games and it has been through adversarial code review, but treat it like any hobby fork: keep your working MiSTer binary as a backup before you swap this one in.

## What works

* MegaCD, PlayStation, Saturn, NeoGeo CD, and 3DO games boot and play from disc, CD audio tracks included where the core supports them.
* The disc is identified automatically, so the right core loads for whatever you put in.
* Region is handled per core, see the Region section below.
* Autoboot. Drop a disc in at the menu and it loads the right core and mounts the disc, hands free.
* Manual mode. A Play row at the bottom of the core list, so you load a disc when you want instead of on insert.

PC Engine CD and swapping discs mid game are not done yet. CD-i is another CD core that could be added the same way if there is interest. This does what the list above says and nothing more.

Some things are deliberately out of scope. Dreamcast uses GD-ROM, a proprietary high density format that normal CD and DVD drives cannot read, so it is not possible with this hardware. ao486 (DOS/Windows PC) is a different flow, you install a game to the hard drive from the disc rather than booting it, so it is more of an "attach the drive to the core" job than "put a disc in and play" and is left for later.

## What you need

* A MiSTer (DE10-nano) on current stock firmware. No kernel changes, no FPGA changes.
* A USB CD or DVD drive. Most drives that show up as /dev/sr0 will work. There is a small probe tool (physcd_probe) you can run on the MiSTer to check a drive before you trust it.
* A decent power supply for the drive. This one really matters. A CD drive pulls a big gulp of current when it spins up for a seek, and the DE10-nano's own USB or a cheap unpowered hub will sag under it. When that happens the drive falls off the USB bus mid game, and everything else on that hub, controllers included, goes quiet for a minute until it recovers. Use a powered hub, and ideally give the drive its own supply. If you ever see a game freeze for a minute and then carry on by itself, that is power, not the software.

## Setup

1. Build the binary (see Building) and copy it to /media/fat/MiSTer. Save your old one as MiSTer.bak first.
2. For MegaCD region matching, drop your BIOS files into the MegaCD home folder as boot_EU.rom, boot_US.rom, and boot_JP.rom. A plain boot.rom still works as a catch all. A PAL disc running on a US BIOS will play but stutter, so this is worth setting up.
3. Reboot.

## Region

A disc and the console BIOS have to agree on region or a game either refuses to boot or plays at the wrong speed. Each core handles this differently, so:

* Saturn: the core has its own Region option. Set it to Auto in the core's OSD menu and it reads the region off the disc. One boot.rom is all you need.
* PlayStation: handled for you. The region is detected and sent to the core.
* NeoGeo CD: use a Unibios and it is region free, so nothing to set. Otherwise the core's system type (CD or CDZ) has to match your BIOS.
* MegaCD: the BIOS is the region. Drop boot_EU.rom, boot_US.rom, and boot_JP.rom into the MegaCD home folder and the matching one loads automatically. A plain boot.rom still works as a catch all. Some MegaCD setups also have an Auto region option in the core, so try that first if you have it.

Short version: if a core has an Auto region setting, use it. Otherwise provide the region named BIOS files above.

Each CD core needs its own BIOS in that core's home folder, same as you would for a ripped image: MegaCD boot.rom, PlayStation BIOS, Saturn boot.rom, and a NeoGeo CD BIOS (uni-bioscd.rom, or top-sp1.bin / neocd.bin).

## Using it

Drive plugged in, disc inside:

* Autoboot on (the default): insert a disc at the menu, it works out the game, loads the core, and mounts the disc. Nothing to press.
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

## It is a fork, and it stays one

Upstream MiSTer has said they will not support USB CD drives, so this is never getting folded back in. It is a permanent fork. It follows Main_MiSTer loosely and will lag behind at times. If you need a newer main feature you may have to rebase it yourself. The changes are kept small and tucked behind a physcd flag on purpose, so rebasing is not too painful.

## Building

The build runs in Docker with the same ARM toolchain Main_MiSTer uses, so you do not need a cross compiler installed.

On Windows:

```
.\tools\build.ps1
```

The binary lands in Main_MiSTer\bin\MiSTer. The fork code is on the physcd branch.

## How it fits together

The backend lives in support/physcd. It reads raw 2352 byte sectors from the drive over SG_IO, keeps a prefetch cache split into two streams (game data and CD audio) so mixed mode discs do not stall, and hides real seek latency behind the timing the cores already model for themselves. Per core the change is tiny: one branch in the read path that pulls from the drive instead of a file, plus the region and BIOS handling. It is all guarded, so normal cue and chd loading behaves exactly as it did before.
