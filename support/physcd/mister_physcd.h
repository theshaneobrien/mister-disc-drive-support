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

#define PHYSCD_DEV_DEFAULT "/dev/sr0"
#define PHYSCD_SENTINEL "*PHYSCD*"
#define PHYSCD_RAW  2352
#define PHYSCD_SUB  96

// pick the drive for the next physcd_open(). pass a path ("/dev/sr1")
// to pin one, or NULL/"" to autodetect. usb enumeration order is not
// stable across reboots, so autodetect is the default.
void physcd_set_device(const char *dev);

// the device actually in use, "" when closed (for logging/osd)
const char *physcd_device_name();

// open the drive and spin up the cache thread. dev NULL honors
// physcd_set_device() and otherwise scans /dev/sr0../dev/sr7,
// preferring a drive with media. 0 on success.
int physcd_open(const char *dev);

// true if a readable disc is present
int physcd_disc_present();

// media-change poll (wraps CDROM_MEDIA_CHANGED); 1 if disc changed/ejected
int physcd_media_changed();

// build a mister toc_t from the drive TOC. sets toc->phys = 1,
// invalidates the sector cache and probes subchannel capability.
// data track sector size is always reported as 2352 (raw reads).
// 0 on success.
int physcd_load_toc(toc_t *toc);

// true if the drive answered a raw P-W subchannel read during the
// probe in physcd_load_toc. when 0, sub96 out params come back zeroed
// and cores should take their "no sub file" path instead.
int physcd_sub_supported();

// read one full raw sector (2352 bytes) into dst, served from cache
// when possible. if sub96 is non-null also return the raw P-W
// subchannel for that sector. blocking, with internal retry.
// 0 on success.
int physcd_read_sector(int lba, uint8_t *dst, uint8_t *sub96);

// as above, but returns 1 only when sub96 received REAL subchannel
// data. sectors recovered by the single-sector retry or the cooked
// fallback carry none, so a per-disc physcd_sub_supported() check is
// not enough - a caller that treats zeros as valid subcode would feed
// the core fabricated data. returns 0 on failure or when absent.
int physcd_read_sector_sub(int lba, uint8_t *dst, uint8_t *sub96);

// convenience: read only the 2048 user bytes of a mode1/mode2 data
// sector (what cdd ReadData() wants). handles the 16/24 byte header
// skip based on the mode byte. 0 on success.
int physcd_read_data2048(int lba, uint8_t *dst);

// hint from the cdd layer: playback/reading repositioned to this lba.
// resets the prefetch cursor so the cache thread starts pulling ahead
// of the new position immediately (call from cdd seek handling).
void physcd_seek_hint(int lba);

// disc fingerprint for the autodetect daemon and menu display
typedef enum {
	PHYSCD_DISC_NONE = 0,
	PHYSCD_DISC_MEGACD,
	PHYSCD_DISC_SATURN,
	PHYSCD_DISC_PSX,
	PHYSCD_DISC_PCECD,
	PHYSCD_DISC_NEOGEO,
	PHYSCD_DISC_AUDIO,
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
physcd_event_t physcd_poll_event(physcd_disc_t *type, physcd_region_t *region);

void physcd_close();

#endif
