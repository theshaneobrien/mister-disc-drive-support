/*
 * mister_physcd.cpp - physical usb cd-rom backend for Main_MiSTer
 * see mister_physcd.h for the design notes.
 *
 * status: fork scaffolding, compiles standalone against linux headers,
 * untested against real cores. cache sizing and retry policy will need
 * tuning on hardware.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/cdrom.h>
#include <scsi/sg.h>
#include <limits.h>

#include "mister_physcd.h"

// ---------------------------------------------------------------- state

#define CACHE_SECTORS 4096            /* 4096 * 2448B ~= 9.5MB of ram   */
#define SLOT_SIZE (PHYSCD_RAW + PHYSCD_SUB)
#define READAHEAD 64                  /* sectors ahead of cursor        */
#define BURST 16                      /* sectors per drive transaction  */

typedef struct {
	int lba;                      /* -1 = empty                     */
	int has_sub;
	uint8_t data[SLOT_SIZE];
} slot_t;

static struct {
	int fd;
	int leadout;
	int first_data_lba;
	slot_t *cache;
	volatile int cursor;          /* prefetch position              */
	volatile int running;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t kick;
} pcd = { -1, 0, -1, NULL, 0, 0, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };

static inline slot_t *slot_for(int lba) { return &pcd.cache[lba % CACHE_SECTORS]; }

// ---------------------------------------------------------------- sg_io

static int sg_read_cd(int lba, int count, int with_sub, uint8_t *dst)
{
	uint8_t cdb[12] = { 0 };
	uint8_t sense[32];
	struct sg_io_hdr io;
	int sector_len = PHYSCD_RAW + (with_sub ? PHYSCD_SUB : 0);

	cdb[0] = 0xBE;                          /* READ CD                  */
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[6] = (count >> 16) & 0xFF;
	cdb[7] = (count >> 8) & 0xFF;
	cdb[8] = count & 0xFF;
	cdb[9] = 0xF8;                          /* full 2352 raw            */
	cdb[10] = with_sub ? 0x01 : 0x00;       /* raw P-W subchannel       */

	memset(&io, 0, sizeof(io));
	io.interface_id = 'S';
	io.cmd_len = 12;
	io.cmdp = cdb;
	io.dxfer_direction = SG_DXFER_FROM_DEV;
	io.dxfer_len = count * sector_len;
	io.dxferp = dst;
	io.sbp = sense;
	io.mx_sb_len = sizeof(sense);
	io.timeout = 8000;

	if (ioctl(pcd.fd, SG_IO, &io) < 0) return -1;
	if (io.status || io.host_status || io.driver_status) return -2;
	return 0;
}

// burst-read into cache slots. tries with subchannel first, falls back
// to plain raw if the drive rejects the combined transfer.
static int fill_cache(int lba, int count)
{
	static uint8_t burst[BURST * SLOT_SIZE];
	static int sub_ok = -1;               /* probe once per session   */

	if (count > BURST) count = BURST;
	if (lba < 0) lba = 0;
	if (lba + count > pcd.leadout) count = pcd.leadout - lba;
	if (count <= 0) return 0;

	int with_sub = (sub_ok != 0);
	int r = sg_read_cd(lba, count, with_sub, burst);
	if (r && with_sub && sub_ok < 0) {
		sub_ok = 0;
		with_sub = 0;
		r = sg_read_cd(lba, count, 0, burst);
	}
	if (!r && with_sub && sub_ok < 0) sub_ok = 1;

	if (r) {
		/* degrade to single-sector retries so one bad sector    */
		/* doesn't poison the whole burst                        */
		for (int i = 0; i < count; i++) {
			uint8_t one[SLOT_SIZE];
			int rr = -1;
			for (int t = 0; t < 3 && rr; t++)
				rr = sg_read_cd(lba + i, 1, 0, one);
			pthread_mutex_lock(&pcd.lock);
			slot_t *s = slot_for(lba + i);
			if (!rr) {
				memcpy(s->data, one, PHYSCD_RAW);
				memset(s->data + PHYSCD_RAW, 0, PHYSCD_SUB);
				s->has_sub = 0;
				s->lba = lba + i;
			} else {
				/* unreadable: serve zeros, don't stall core */
				memset(s->data, 0, SLOT_SIZE);
				s->has_sub = 0;
				s->lba = lba + i;
				printf("physcd: unreadable sector lba=%d\n", lba + i);
			}
			pthread_mutex_unlock(&pcd.lock);
		}
		return 0;
	}

	int sector_len = PHYSCD_RAW + (with_sub ? PHYSCD_SUB : 0);
	pthread_mutex_lock(&pcd.lock);
	for (int i = 0; i < count; i++) {
		slot_t *s = slot_for(lba + i);
		memcpy(s->data, burst + i * sector_len, sector_len);
		if (!with_sub) memset(s->data + PHYSCD_RAW, 0, PHYSCD_SUB);
		s->has_sub = with_sub;
		s->lba = lba + i;
	}
	pthread_mutex_unlock(&pcd.lock);
	return 0;
}

static void *prefetch_thread(void *arg)
{
	(void)arg;
	int pos = 0;
	while (pcd.running) {
		int cur = pcd.cursor;
		if (cur != pos) pos = cur;         /* seek hint moved us   */

		/* find first uncached sector in the readahead window     */
		int target = -1;
		pthread_mutex_lock(&pcd.lock);
		for (int i = 0; i < READAHEAD; i++) {
			int lba = pos + i;
			if (lba >= pcd.leadout) break;
			if (slot_for(lba)->lba != lba) { target = lba; break; }
		}
		pthread_mutex_unlock(&pcd.lock);

		if (target < 0) {
			/* window full: wait for a kick                   */
			struct timespec ts = { 0, 20 * 1000 * 1000 };
			nanosleep(&ts, NULL);
			continue;
		}
		fill_cache(target, BURST);
	}
	return NULL;
}

// ---------------------------------------------------------------- api

int physcd_open(const char *dev)
{
	if (pcd.fd >= 0) return 0;
	pcd.fd = open(dev ? dev : PHYSCD_DEV_DEFAULT, O_RDONLY | O_NONBLOCK);
	if (pcd.fd < 0) return -1;

	pcd.cache = (slot_t *)malloc(sizeof(slot_t) * CACHE_SECTORS);
	if (!pcd.cache) { close(pcd.fd); pcd.fd = -1; return -1; }
	for (int i = 0; i < CACHE_SECTORS; i++) pcd.cache[i].lba = -1;

	pcd.running = 1;
	pthread_create(&pcd.thread, NULL, prefetch_thread, NULL);
	printf("physcd: opened %s\n", dev ? dev : PHYSCD_DEV_DEFAULT);
	return 0;
}

int physcd_disc_present()
{
	if (pcd.fd < 0) return 0;
	return ioctl(pcd.fd, CDROM_DRIVE_STATUS, CDSL_CURRENT) == CDS_DISC_OK;
}

int physcd_media_changed()
{
	if (pcd.fd < 0) return 0;
	return ioctl(pcd.fd, CDROM_MEDIA_CHANGED, CDSL_CURRENT) > 0;
}

int physcd_load_toc(toc_t *toc)
{
	struct cdrom_tochdr hdr;
	if (pcd.fd < 0 || !physcd_disc_present()) return -1;
	if (ioctl(pcd.fd, CDROMREADTOCHDR, &hdr) < 0) return -1;

	memset(toc, 0, sizeof(toc_t));
	pcd.first_data_lba = -1;

	int n = 0;
	for (int t = hdr.cdth_trk0; t <= hdr.cdth_trk1 && n < 99; t++, n++) {
		struct cdrom_tocentry e;
		memset(&e, 0, sizeof(e));
		e.cdte_track = t;
		e.cdte_format = CDROM_LBA;
		if (ioctl(pcd.fd, CDROMREADTOCENTRY, &e) < 0) return -1;

		cd_track_t *trk = &toc->tracks[n];
		trk->start = e.cdte_addr.lba;
		trk->type = (e.cdte_ctrl & CDROM_DATA_TRACK) ? TT_MODE1 : TT_CDDA;
		trk->sector_size = PHYSCD_RAW;   /* we always read raw   */
		trk->offset = 0;
		trk->index_num = 2;
		trk->indexes[0] = 0;
		trk->indexes[1] = 0;             /* pregap folded into start */

		if (trk->type != TT_CDDA && pcd.first_data_lba < 0)
			pcd.first_data_lba = trk->start;
		if (n > 0) toc->tracks[n - 1].end = trk->start;
	}

	struct cdrom_tocentry lead;
	memset(&lead, 0, sizeof(lead));
	lead.cdte_track = CDROM_LEADOUT;
	lead.cdte_format = CDROM_LBA;
	if (ioctl(pcd.fd, CDROMREADTOCENTRY, &lead) < 0) return -1;

	toc->tracks[n - 1].end = lead.cdte_addr.lba;
	toc->last = n;
	toc->end = lead.cdte_addr.lba;
	toc->sectorSize = PHYSCD_RAW;
	toc->phys = 1;                            /* new cd.h field       */
	pcd.leadout = lead.cdte_addr.lba;
	pcd.cursor = toc->tracks[0].start;

	printf("physcd: toc loaded, %d tracks, leadout %d\n", n, pcd.leadout);
	return 0;
}

void physcd_seek_hint(int lba)
{
	pcd.cursor = lba;
}

int physcd_read_sector(int lba, uint8_t *dst, uint8_t *sub96)
{
	if (pcd.fd < 0 || lba < 0 || lba >= pcd.leadout) return -1;

	pcd.cursor = lba;                         /* keep prefetch ahead  */

	pthread_mutex_lock(&pcd.lock);
	slot_t *s = slot_for(lba);
	int hit = (s->lba == lba);
	if (hit) {
		memcpy(dst, s->data, PHYSCD_RAW);
		if (sub96) memcpy(sub96, s->data + PHYSCD_RAW, PHYSCD_SUB);
	}
	pthread_mutex_unlock(&pcd.lock);
	if (hit) return 0;

	/* cache miss: synchronous burst fill, then serve               */
	if (fill_cache(lba, BURST)) return -1;

	pthread_mutex_lock(&pcd.lock);
	s = slot_for(lba);
	hit = (s->lba == lba);
	if (hit) {
		memcpy(dst, s->data, PHYSCD_RAW);
		if (sub96) memcpy(sub96, s->data + PHYSCD_RAW, PHYSCD_SUB);
	}
	pthread_mutex_unlock(&pcd.lock);
	return hit ? 0 : -1;
}

int physcd_read_data2048(int lba, uint8_t *dst)
{
	uint8_t raw[PHYSCD_RAW];
	if (physcd_read_sector(lba, raw, NULL)) return -1;

	/* mode byte at offset 15: mode1 user data at 16, mode2/xa at 24 */
	int off = (raw[15] == 2) ? 24 : 16;
	memcpy(dst, raw + off, 2048);
	return 0;
}

// ---------------------------------------------------------------- ident

physcd_disc_t physcd_identify()
{
	uint8_t raw[PHYSCD_RAW * 2];

	if (!physcd_disc_present()) return PHYSCD_DISC_NONE;
	if (pcd.first_data_lba < 0) {
		/* toc not loaded yet or audio-only disc                  */
		toc_t tmp;
		if (physcd_load_toc(&tmp)) return PHYSCD_DISC_NONE;
		if (pcd.first_data_lba < 0) return PHYSCD_DISC_AUDIO;
	}

	int base = pcd.first_data_lba;

	if (!physcd_read_sector(base, raw, NULL)) {
		if (!memcmp(raw + 16, "SEGADISCSYSTEM", 14)) return PHYSCD_DISC_MEGACD;
		if (!memcmp(raw + 16, "SEGA SEGASATURN", 15)) return PHYSCD_DISC_SATURN;
	}

	if (!physcd_read_sector(base + 16, raw, NULL)) {
		uint8_t *iso = raw + 16;
		if (memcmp(iso + 1, "CD001", 5)) iso = raw + 24;   /* mode2 form1 */
		if (!memcmp(iso + 1, "CD001", 5)) {
			if (!memcmp(iso + 8, "PLAYSTATION", 11)) return PHYSCD_DISC_PSX;
			if (!memcmp(iso + 8, "NGCD", 4)) return PHYSCD_DISC_NEOGEO;
		}
	}

	if (!physcd_read_sector(base, raw, NULL) &&
	    !physcd_read_sector(base + 1, raw + PHYSCD_RAW, NULL)) {
		for (int off = 0; off < (int)sizeof(raw) - 24; off++)
			if (!memcmp(raw + off, "PC Engine CD-ROM SYSTEM", 23))
				return PHYSCD_DISC_PCECD;
	}

	return PHYSCD_DISC_UNKNOWN;
}

const char *physcd_disc_name(physcd_disc_t t)
{
	switch (t) {
	case PHYSCD_DISC_MEGACD: return "MegaCD";
	case PHYSCD_DISC_SATURN: return "Saturn";
	case PHYSCD_DISC_PSX:    return "PSX";
	case PHYSCD_DISC_PCECD:  return "TurboGrafx CD";
	case PHYSCD_DISC_NEOGEO: return "NeoGeo CD";
	case PHYSCD_DISC_AUDIO:  return "Audio CD";
	case PHYSCD_DISC_NONE:   return "No Disc";
	default:                 return "Unknown";
	}
}

void physcd_close()
{
	if (pcd.fd < 0) return;
	pcd.running = 0;
	pthread_join(pcd.thread, NULL);
	free(pcd.cache);
	pcd.cache = NULL;
	close(pcd.fd);
	pcd.fd = -1;
}
