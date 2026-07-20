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

// call once from user_io_init, after the core name is known
void physcd_autoboot_startup(void);

// call from the menu tick in HandleUI. returns 1 while something is
// in progress, so the caller can speed its timer up to 100ms.
int physcd_autoboot_menu_tick(void);

// true when a banner is showing and the menu should not be redrawn over
int physcd_autoboot_busy(void);

// mount the physical drive into whatever cd core is running. shared by
// the mount_phys fifo command and phase B so they cannot drift apart.
// returns 1 if the core type was recognised.
int physcd_mount_current_core(void);

#endif
