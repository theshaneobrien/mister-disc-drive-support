#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../../ide.h"
#include "../../cd.h"
#include "mister_physcd.h"
#include "physcd_log.h"
#include "physcd_ide.h"

/* ---- toc_t -> drive_t/track_t ----------------------------------------
   this mirrors load_chd_file() in ide_cdrom.cpp, which already does the
   same translation for CHDs ("borrow the cd.h toc_t ... then translate the
   toc_t to drive_t+track_t"). two conventions matter and they are easy to
   get backwards:

   * physcd_load_toc() reports DRIVE-ABSOLUTE lbas with EXCLUSIVE ends, so
     length = end - start, exactly as the chd loader computes it. do NOT
     apply the +150 pregap bias the console cores use - ide track_t.start is
     unbiased and the msf conversions add REDBOOK_FRAME_PADDING themselves.
     (apply_phys_bias() in the console path does the opposite; they are not
     interchangeable.)

   * track_cnt INCLUDES a trailing lead-out entry. get_track_from_lba(),
     disc_info() and cdtv's own "real_tracks = track_cnt - 1" all assume it
     is there. forgetting it loses the last real track.            */

int physcd_ide_attach(drive_t *drv)
{
	if (!drv) return 0;

	if (physcd_open(NULL))
	{
		physcd_log("ide: no drive opened - nothing to mount");
		physcd_close();
		return 0;
	}

	toc_t toc = {};
	if (physcd_load_toc(&toc) || !toc.last)
	{
		physcd_log("ide: drive opened but no readable toc - disc missing, "
		           "unfinalized, or still spinning up");
		physcd_close();
		return 0;
	}

	/* toc_t carries up to 99 tracks, drive_t only has track[50] - and one of
	   those must be left for the lead-out. a disc with more tracks than we
	   can hold is truncated rather than allowed to run off the array. */
	int n = toc.last;
	int max = (int)(sizeof(drv->track) / sizeof(drv->track[0])) - 1;
	if (n > max)
	{
		printf("physcd-ide: disc has %d tracks, only %d fit - truncating\n", n, max);
		physcd_log("ide: disc has %d tracks, only %d fit - truncating", n, max);
		n = max;
	}

	memset(drv->track, 0, sizeof(drv->track));
	drv->track_cnt = 0;
	drv->cd = 1;
	drv->phys = 1;

	uint32_t total_sector_size = 0;
	for (int i = 0; i < n; i++)
	{
		cd_track_t *src = &toc.tracks[i];
		track_t *trk = &drv->track[i];

		trk->number     = i + 1;
		trk->sectorSize = src->sector_size;	// PHYSCD_RAW (2352): we always read raw
		trk->phys       = 1;
		trk->skip       = 0;
		trk->chd_offset = 0;
		trk->start      = src->start;
		trk->length     = src->end - src->start;

		if (src->type != TT_CDDA) trk->attr = 0x40;	// data track

		drv->track_cnt++;
		total_sector_size += trk->length * trk->sectorSize;
	}

	/* lead-out, same shape as the chd loader's */
	track_t *lead_out = &drv->track[drv->track_cnt];
	lead_out->number = drv->track_cnt + 1;
	lead_out->attr   = 0;
	lead_out->phys   = 1;
	lead_out->start  = toc.tracks[n - 1].end;
	lead_out->length = 0;
	drv->track_cnt++;

	drv->total_sectors = total_sector_size / 512;

	drv->data_num = 0;
	for (uint8_t i = 0; i < drv->track_cnt; i++)
	{
		if (drv->track[i].attr == 0x40)
		{
			drv->data_num = i;
			break;
		}
	}

	/* mode2 is reported to the guest by track_info() and picks the 24-vs-16
	   byte header in the image read paths. the physcd read path does not
	   need it (physcd_read_data2048 reads the mode byte per sector), but the
	   guest-visible answer should still be right, so sniff the first data
	   track once. a cold drive that cannot serve this read just leaves the
	   mode1 default - it is metadata, not worth failing the mount over. */
	track_t *dt = &drv->track[drv->data_num];
	if (dt->attr == 0x40)
	{
		uint8_t raw[PHYSCD_RAW];
		if (!physcd_read_sector(dt->start, raw, NULL)) dt->mode2 = (raw[15] == 2);
	}

	printf("physcd-ide: %d track(s) + lead-out, data track %d, mode2 = %d, lead-out lba %u\n",
	       drv->track_cnt - 1, drv->data_num, dt->mode2, lead_out->start);
	physcd_log("ide: toc adapted - %d track(s) + lead-out, data track %d, mode2 %d, lead-out lba %u",
	           drv->track_cnt - 1, drv->data_num, dt->mode2, lead_out->start);

	/* spin a cold drive up before the guest's first read: dos/mscdex and the
	   atapi layer enforce a not-ready timeout the console cores never had. */
	physcd_prewarm_blocking();
	physcd_log("ide: mounted, drive prewarmed");
	return 1;
}

void physcd_ide_detach(drive_t *drv)
{
	if (!drv || !drv->phys) return;

	/* clear phys only. drv->cd is owned by ide_img_set() and by
	   minimig_cd_drive_open(), which sets it BEFORE calling the parser that
	   lands here - clearing it would break a phys -> image swap on cd32. */
	drv->phys = 0;
	for (uint8_t i = 0; i < sizeof(drv->track) / sizeof(drv->track[0]); i++) drv->track[i].phys = 0;
	drv->track_cnt = 0;
	drv->data_num = 0;

	physcd_log("ide: physical mount released");
	physcd_close();
}
