#ifndef MISTER_PHYSCD_INCLUDED
#define MISTER_PHYSCD_INCLUDED

#include <stdint.h>
#include "../../cd.h"

/*
 * physical usb cd-rom backend for Main_MiSTer cd cores.
 * third data source alongside cue/bin (fileTYPE) and chd.
 *
 * design:
 *  - one global drive instance (/dev/sr0), one disc at a time
 *  - toc_t is populated from the drive TOC so per-core cdd state
 *    machines run unmodified
 *  - all reads are raw 2352-byte READ CD (0xBE) via SG_IO with an
 *    optional 96-byte raw P-W subchannel
 *  - a background thread prefetches sequentially from the last
 *    requested lba into a ram sector cache to hide real seek latency
 *    (cores simulate their own seek timing and expect data on schedule)
 *
 * integration contract:
 *  - cd.h toc_t gains `int phys;` (zero-init keeps existing paths intact)
 *  - cores check toc.phys BEFORE toc.chd_f in their read functions
 *  - set_image functions receive PHYSCD_SENTINEL instead of a file path
 *    to mean "mount the physical drive"
 */

#define PHYSCD_SENTINEL "*PHYSCD*"
#define PHYSCD_RAW  2352
#define PHYSCD_SUB  96

// how long (ms) the cores hold the tray OPEN after a physical swap's new toc
// is loaded, before closing it so the bios/game re-scans. megacd/neogeo pulse
// OPEN->STOP for exactly this long; saturn's lid has been open in real time
// since the eject, so for it this is only the close delay. a hardware-tuning
// knob: long enough for a slow-polling bios to observe the lid move.
#define PHYSCD_SWAP_DWELL_MS 500

// pick the drive for the next physcd_open(). pass a path ("/dev/sr1")
// to pin one, or NULL/"" to autodetect. usb enumeration order is not
// stable across reboots, so autodetect is the default.
void physcd_set_device(const char *dev);

// open the drive and spin up the cache thread. dev NULL honors
// physcd_set_device() and otherwise scans /dev/sr0../dev/sr7,
// preferring a drive with media. 0 on success.
int physcd_open(const char *dev);

// true if a readable disc is present
int physcd_disc_present();

// media-change poll (wraps CDROM_MEDIA_CHANGED); 1 if disc changed/ejected
int physcd_media_changed();

// true while physcd holds the drive (watching or a game disc mounted), so the
// acoustic seek can stay off the drive whenever physcd wants it.
int physcd_drive_busy();

// mid-mount physical disc swap (multi-disc games). the core arms detection on
// a physical mount; physcd_swap_consume() returns 1 once after an eject/insert
// has been seen and the new toc loaded, so the core re-announces the disc.
// physcd_swap_ejected() is 1 during the swap window itself (disc out, or back
// in but not yet read) so a core can show the guest the lid open in REAL TIME.
void physcd_swap_enable(int enable);
int physcd_swap_consume(void);
int physcd_swap_ejected(void);

// did a swap complete during the last mount session? read-and-clear, and
// file-backed so it survives the core-exit exec (the menu is a fresh
// process). the menu's autoboot uses it so a disc swapped in MID-GAME is
// treated as already-played on core exit, not auto-launched as a fresh insert.
int physcd_swap_happened(void);

// build a mister toc_t from the drive TOC. sets toc->phys = 1,
// invalidates the sector cache and probes subchannel capability.
// data track sector size is always reported as 2352 (raw reads).
// 0 on success.
int physcd_load_toc(toc_t *toc);

// fill a toc_t from the ALREADY-mounted disc without touching the drive or
// cache (unlike physcd_load_toc). for mid-session callers such as the RA
// hash reader. 0 on success, -1 if nothing is mounted.
int physcd_current_toc(toc_t *toc);

// read one full raw sector (2352 bytes) into dst, served from cache
// when possible. if sub96 is non-null also return the raw P-W
// subchannel for that sector. blocking, with internal retry.
// 0 on success.
int physcd_read_sector(int lba, uint8_t *dst, uint8_t *sub96);

// as above, but returns 1 only when sub96 received REAL subchannel
// data. sectors recovered by the single-sector retry or the cooked
// fallback carry none, so a per-disc capability flag is not enough - a
// caller that treats zeros as valid subcode would feed the core
// fabricated data. returns 0 on failure or when absent.
int physcd_read_sector_sub(int lba, uint8_t *dst, uint8_t *sub96);

// convenience: read only the 2048 user bytes of a mode1/mode2 data
// sector (what cdd ReadData() wants). handles the 16/24 byte header
// skip based on the mode byte. 0 on success.
int physcd_read_data2048(int lba, uint8_t *dst);

// hint from the cdd layer: playback/reading repositioned to this lba.
// resets the prefetch cursor so the cache thread starts pulling ahead
// of the new position immediately (call from cdd seek handling).
void physcd_seek_hint(int lba);

// spin a cold drive up to read speed and prime the start of the disc BEFORE the
// core begins reading. blocks the caller (up to ~8s on a stone-cold drive,
// near-instant on a warm one). call from a phys mount, after physcd_load_toc,
// for a core that reads the disc the instant it is mounted (cd-i's bios does),
// so its initial load and real-time fmv are not fed by a still-spinning drive.
void physcd_prewarm_blocking(void);

// startup environment fix: installs a persistent udev rules file exempting cd
// drives from blkid superblock probing (the boot-coldplug head-seesaw behind
// the cd-i cold-load choppiness - see install_udev_rule in the .cpp for the
// full story). idempotent; call once per process start.
void physcd_quiet_udev(void);

// opt this mount into running the drive UNCAPPED on data-only discs (native
// CAV speed management = fast long-throw seeks). for cores that stream live
// and long-throw mid-stream against a hard deadline (cd-i voice clips); the
// proven-at-4x cores keep their exact drive profile by not calling this.
// call between physcd_open and physcd_load_toc; cleared by physcd_close.
// discs with audio tracks stay capped even when opted in.
void physcd_speed_uncap(int enable);

// disc fingerprint for the autodetect daemon and menu display
typedef enum {
	PHYSCD_DISC_NONE = 0,
	PHYSCD_DISC_MEGACD,
	PHYSCD_DISC_SATURN,
	PHYSCD_DISC_PSX,
	PHYSCD_DISC_PCECD,
	PHYSCD_DISC_NEOGEO,
	PHYSCD_DISC_3DO,
	/* new types go AFTER AUDIO: the /tmp autoboot markers persist these as
	   raw ints across the core-exit exec, and a no-reboot binary swap would
	   misread a shifted AUDIO (cdi was briefly inserted before it and an
	   old marker's 7 then read as cdi = one spurious relaunch). UNKNOWN is
	   never persisted, so appending before it is always safe. */
	PHYSCD_DISC_AUDIO,
	PHYSCD_DISC_CDI,
	PHYSCD_DISC_VCD,     /* video cd / cd-bridge; boots the PHYSCD_VCD_CORE core (default CD-i) */
	PHYSCD_DISC_CDG,     /* audio cd carrying cd+g karaoke graphics in the R-W subchannel;
	                        boots PHYSCD_CDG_CORE (default CD-i, the only core that draws them) */
	PHYSCD_DISC_UNKNOWN,
} physcd_disc_t;

physcd_disc_t physcd_identify();
const char *physcd_disc_name(physcd_disc_t t);

// disc region. a pal disc booted on a us bios runs at the wrong
// timing - it plays, but stutters - so the mount path uses this to
// pick a matching bios.
typedef enum {
	PHYSCD_REGION_UNKNOWN = 0,
	PHYSCD_REGION_JP,
	PHYSCD_REGION_US,
	PHYSCD_REGION_EU,
} physcd_region_t;

// region of the mounted disc. mega cd only for now: it reads the
// mega drive style header the disc mirrors in its first data sector.
physcd_region_t physcd_region();

// parse a mega drive style header block (>= 0x1F3 bytes, "SEGA" at
// 0x100, region field at 0x1F0). exposed because mega cd BIOS roms
// carry the same header, so callers can cross-check disc against bios.
physcd_region_t physcd_region_from_md_header(const uint8_t *hdr, int len);

// "JP" / "US" / "EU", or "" when unknown
const char *physcd_region_name(physcd_region_t r);

// ---------------------------------------------------------------- watch
//
// menu-side disc watching. the drive is opened and the existing
// prefetch thread - idle anyway while nothing is mounted - polls media
// status and identifies a new disc on ITS OWN thread, because
// physcd_identify can block for seconds and the ui runs cooperatively
// on a ~1ms budget. the menu then consumes an event in O(1).
typedef enum {
	PHYSCD_EV_NONE = 0,
	PHYSCD_EV_DISC_IN,      // new disc, identified
	PHYSCD_EV_DISC_OUT,     // tray opened / disc gone
} physcd_event_t;

int physcd_watch_start(void);
void physcd_watch_stop(void);
int physcd_watching(void);

// non-blocking; returns the pending event and clears it. never call
// any osd function from the watcher - this is the handoff.
// *initial is 1 when the disc was ALREADY in the drive when watching
// began, rather than newly inserted - the caller needs that to avoid
// re-booting the same disc every time the user returns to the menu.
physcd_event_t physcd_poll_event(physcd_disc_t *type, physcd_region_t *region, int *initial);

// forget the current disc so the next identify re-reads the toc. the
// watcher calls this on media change; without it a second disc is
// identified from the first one's toc and cached sectors.
void physcd_forget_disc(void);

// human-readable name from the iso9660 volume label ("SONIC CD"), for
// the menu. blocking (reads the disc); returns strlen, 0 if none or if
// the label is the generic "PLAYSTATION".
int physcd_disc_label(char *out, int outsz);

// psx game serial ("SLES-01234") as a name fallback when the iso label
// is blank. blocking (reads the disc); returns strlen, 0 if none.
int physcd_disc_serial(char *out, int outsz);

// friendly console name for the menu ("Mega CD", "PlayStation")
const char *physcd_console_name(physcd_disc_t t);

// O(1) snapshot for the menu row, safe from the ui thread: 1 if a disc
// is present, filling *type and a display name (label or console name).
int physcd_menu_status(char *name, int namesz, physcd_disc_t *type);

// edge-triggered: returns 1 once after the disc presence changed, so the
// menu can rebuild the core list to add/remove the Play Disc row.
int physcd_menu_dirty(void);

void physcd_close();

#endif
