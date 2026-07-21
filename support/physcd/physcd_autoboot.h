#ifndef PHYSCD_AUTOBOOT_H
#define PHYSCD_AUTOBOOT_H

/*
 * disc autodetect and autoboot.
 *
 * TWO PHASES, and this is forced rather than chosen: fpga_load_rbf()
 * does not return - it execs, replacing the process image, so every
 * variable, every fd and the prefetch thread die with it. "detect ->
 * load core -> mount" cannot be one in-process sequence.
 *
 *   phase A (menu process)  physcd_autoboot_menu_tick()
 *       watch for a disc, show a banner, write a handoff marker,
 *       release the drive, load the core. process ends here.
 *
 *   phase B (new core process)  physcd_autoboot_startup()
 *       read the marker, confirm it names US, wait for the core to
 *       settle, mount, report. no marker means a human loaded this
 *       core by hand, so we leave them alone.
 */

// call once at startup, after user_io_init: consumes the marker (game
// core) or starts watching (menu core). never blocks on the mount -
// that is armed on a timer and completed by physcd_autoboot_poll.
void physcd_autoboot_startup(void);

// call every main-loop pass. completes a deferred phase B mount once
// the core has had time to settle. cheap no-op otherwise.
void physcd_autoboot_poll(void);

// call from the menu tick in HandleUI. returns 1 while something is
// in progress, so the caller can speed its timer up to 100ms.
int physcd_autoboot_menu_tick(void);

// true while a banner is up; the menu uses this to pick its tick rate
int physcd_autoboot_busy(void);

// user pressed a key: abandon this disc until it is ejected
void physcd_autoboot_cancel(void);

// menu "Load Disc" action: load + mount the disc currently in the drive.
// execs on success (never returns); returns 0 if there is nothing to load.
int physcd_autoboot_load_disc(void);

// synthetic d_name for the pinned "Play Disc" row injected at the top of
// the menu core list. not a real file; selecting it loads the disc.
#define PHYSCD_MENU_SENTINEL "\x01physcd"

// fill the cores-list row text for the disc in the drive. returns 1 if a
// mountable disc is present (row should be shown), else 0. O(1).
int physcd_menu_row(char *out, int outsz);

// true if a d_name is the pinned Play Disc row
int physcd_is_menu_row(const char *name);

// mount the physical drive into whatever cd core is running. shared by
// the mount_phys fifo command and phase B so they cannot drift apart.
// returns 1 if the core type was recognised.
int physcd_mount_current_core(void);

// manual swap fallback for the swap_phys fifo: re-reads the disc now in the
// drive and hands it to the running game without resetting (multi-disc games).
// every swap-capable core auto-detects a physical swap in its poll; only PSX
// is also wired to this manual command. returns 0 on any other core.
int physcd_swap_current_core(void);

#endif
