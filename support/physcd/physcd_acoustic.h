#ifndef PHYSCD_ACOUSTIC_INCLUDED
#define PHYSCD_ACOUSTIC_INCLUDED

/*
 * acoustic seek.
 *
 * give image-backed (chd/cue) games the SOUND of a real drive. we already
 * know the lba a game is reading - the chd read path hands it to us - so
 * mirror those positions onto a real usb cd drive holding a throwaway "prop"
 * disc. the drive physically seeks and spins in step with the game: the
 * motor-and-head sound a real console makes and an emulator never can.
 *
 * gated behind PHYSCD_ACOUSTIC in the ini, off by default. it only ever runs
 * for image games; a physcd (physical game disc) mount owns the same drive,
 * so acoustic pauses whenever physcd wants it.
 */

// set from cfg at startup; starts the background seek thread when enabled.
void physcd_acoustic_config(int enabled);

// called from the chd read path with the lba being read. CHEAP and
// non-blocking - it runs on the fpga-answering thread, so it only stashes the
// position; the background thread does the actual (blocking) seeking.
void physcd_acoustic_hint(int lba);

// the menu owns the drive for disc-watching, so pause acoustic there and
// resume it when a core loads. paired with physcd_watch_start / _stop.
void physcd_acoustic_pause(void);
void physcd_acoustic_resume(void);

#endif
