#ifndef PHYSCD_IDE_H
#define PHYSCD_IDE_H

/* physcd on the ide-family cores: ao486, cd32 (akiko), cdtv and amiga on
   gayle ide. all four share ide.h's drive_t/track_t and the two read
   functions in ide_cdrom.cpp (cdrom_read_raw_sector / cdrom_read_track_raw),
   so ONE toc adapter serves the lot - unlike the console cores, which each
   needed their own branch against cd.h's toc_t.

   the console path (support/physcd/mister_physcd.h) stays the backend; this
   file is only the translation layer into the ide structs. */

struct drive_t;

/* open the drive, read its toc and populate drv->track[] / track_cnt /
   data_num / cd / phys. returns 1 on success, 0 if there is no usable disc
   (and leaves the backend closed). */
int physcd_ide_attach(struct drive_t *drv);

/* release a physical mount. safe to call on a drive that was never phys. */
void physcd_ide_detach(struct drive_t *drv);

#endif
