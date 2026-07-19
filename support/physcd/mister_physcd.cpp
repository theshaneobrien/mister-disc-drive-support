/*
 * mister_physcd.cpp - physical usb cd-rom backend for Main_MiSTer
 * see mister_physcd.h for the design notes.
 *
 * flags-byte strategy and the CDROMREADRAW fallback follow the two
 * public GPL implementations of this idea (sidneivl's feature/use-cdrom
 * branch and Anime0t4ku's Physical_Disc fork): READ CD wants 0xF8 on
 * data tracks but 0x10 on cd-da (both transfer 2352 bytes, some usb
 * bridges reject 0xF8 on audio), and bridges that reject raw MMC
 * packets entirely still honor the kernel's cooked CDROMREADRAW path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <time.h>
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

typedef struct {
	int start;
	int end;
	int audio;
} phys_trk_t;

static struct {
	int fd;
	int leadout;                  /* nonzero only when a toc is live */
	int first_data_lba;
	int sub_ok;                   /* -1 unknown, 0 no, 1 yes        */
	phys_trk_t trk[100];
	int ntrk;
	slot_t *cache;
	volatile int cursor;          /* prefetch position              */
	volatile int running;
	pthread_t thread;
	pthread_mutex_t lock;
} pcd = { -1, 0, -1, -1, {}, 0, NULL, 0, 0, 0, PTHREAD_MUTEX_INITIALIZER };

static inline slot_t *slot_for(int lba) { return &pcd.cache[lba % CACHE_SECTORS]; }

static int track_of(int lba)
{
	for (int i = 0; i < pcd.ntrk; i++)
		if (lba < pcd.trk[i].end) return i;
	return pcd.ntrk ? pcd.ntrk - 1 : -1;
}

// ---------------------------------------------------------------- reads

static int sg_read_cd(int lba, int count, uint8_t flags, int with_sub, uint8_t *dst)
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
	cdb[9] = flags;                         /* 0xF8 data, 0x10 cd-da    */
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

// cooked single-sector fallback through the kernel cdrom layer, for
// bridges that reject raw MMC packets. MSF-addressed, absolute (+150).
static int cooked_read_raw(int lba, uint8_t *dst)
{
	union {
		struct cdrom_msf msf;
		uint8_t raw[PHYSCD_RAW];
	} req;

	int f = lba + 150;
	memset(&req, 0, sizeof(req));
	req.msf.cdmsf_min0 = f / (75 * 60);
	req.msf.cdmsf_sec0 = (f / 75) % 60;
	req.msf.cdmsf_frame0 = f % 75;

	if (ioctl(pcd.fd, CDROMREADRAW, &req) < 0) return -1;
	memcpy(dst, req.raw, PHYSCD_RAW);
	return 0;
}

// burst-read into cache slots. flags byte follows the track type and
// bursts are clamped at track boundaries so one transaction never
// mixes data and cd-da. degrades to single-sector retries, then the
// cooked path, so one bad sector doesn't poison the whole burst.
static int fill_cache(int lba, int count)
{
	uint8_t burst[BURST * SLOT_SIZE];     /* stack: both threads call here */

	if (count > BURST) count = BURST;
	if (lba < 0) lba = 0;
	if (lba + count > pcd.leadout) count = pcd.leadout - lba;
	if (count <= 0) return 0;

	int t = track_of(lba);
	if (t >= 0 && lba + count > pcd.trk[t].end) count = pcd.trk[t].end - lba;
	uint8_t flags = (t >= 0 && pcd.trk[t].audio) ? 0x10 : 0xF8;

	int with_sub = (pcd.sub_ok == 1);
	int r = sg_read_cd(lba, count, flags, with_sub, burst);

	if (r) {
		for (int i = 0; i < count; i++) {
			uint8_t one[SLOT_SIZE];
			int rr = -1;
			for (int n = 0; n < 3 && rr; n++)
				rr = sg_read_cd(lba + i, 1, flags, 0, one);
			if (rr) rr = cooked_read_raw(lba + i, one);
			pthread_mutex_lock(&pcd.lock);
			slot_t *s = slot_for(lba + i);
			if (!rr) {
				memcpy(s->data, one, PHYSCD_RAW);
			} else {
				/* unreadable: serve zeros, don't stall core */
				memset(s->data, 0, PHYSCD_RAW);
				printf("physcd: unreadable sector lba=%d\n", lba + i);
			}
			memset(s->data + PHYSCD_RAW, 0, PHYSCD_SUB);
			s->has_sub = 0;
			s->lba = lba + i;
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
			/* window full (or no toc yet): check again shortly */
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
	if (pcd.fd < 0) {
		printf("physcd: cannot open %s: %s\n", dev ? dev : PHYSCD_DEV_DEFAULT, strerror(errno));
		return -1;
	}

	pcd.cache = (slot_t *)malloc(sizeof(slot_t) * CACHE_SECTORS);
	if (!pcd.cache) { close(pcd.fd); pcd.fd = -1; return -1; }
	for (int i = 0; i < CACHE_SECTORS; i++) pcd.cache[i].lba = -1;

	/* best effort: spin the drive at full speed for prefetch headroom */
	ioctl(pcd.fd, CDROM_SELECT_SPEED, 0);

	pcd.leadout = 0;
	pcd.ntrk = 0;
	pcd.sub_ok = -1;
	pcd.running = 1;
	pthread_create(&pcd.thread, NULL, prefetch_thread, NULL);

	/* main() pins the process to cpu1 for fpga latency; let the
	   prefetch thread run on either core so it doesn't compete */
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(0, &set);
	CPU_SET(1, &set);
	pthread_setaffinity_np(pcd.thread, sizeof(set), &set);

	printf("\x1b[32mphyscd: opened %s\n\x1b[0m", dev ? dev : PHYSCD_DEV_DEFAULT);
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

// probe raw P-W subchannel support once per disc: only conclude "no"
// when a plain read of the same sector succeeds where the sub read
// failed (a scratched sector must not disable subchannel for good).
static void probe_subchannel(int lba)
{
	uint8_t buf[SLOT_SIZE];
	int t = track_of(lba);
	uint8_t flags = (t >= 0 && pcd.trk[t].audio) ? 0x10 : 0xF8;

	if (!sg_read_cd(lba, 1, flags, 1, buf)) { pcd.sub_ok = 1; return; }
	if (!sg_read_cd(lba, 1, flags, 0, buf)) { pcd.sub_ok = 0; return; }
	pcd.sub_ok = -1;                  /* couldn't tell, retry later */
}

int physcd_sub_supported()
{
	return pcd.sub_ok == 1;
}

int physcd_load_toc(toc_t *toc)
{
	struct cdrom_tochdr hdr;
	if (pcd.fd < 0 || !physcd_disc_present()) return -1;
	if (ioctl(pcd.fd, CDROMREADTOCHDR, &hdr) < 0) return -1;

	memset(toc, 0, sizeof(toc_t));
	pcd.leadout = 0;              /* stall prefetch while we swap tocs */
	pcd.first_data_lba = -1;
	pcd.ntrk = 0;

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

		pcd.trk[n].start = trk->start;
		pcd.trk[n].audio = (trk->type == TT_CDDA);

		if (trk->type != TT_CDDA && pcd.first_data_lba < 0)
			pcd.first_data_lba = trk->start;
		if (n > 0) {
			toc->tracks[n - 1].end = trk->start;
			pcd.trk[n - 1].end = trk->start;
		}
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

	pcd.trk[n - 1].end = lead.cdte_addr.lba;
	pcd.ntrk = n;

	/* new (or re-read) disc: drop every cached sector */
	pthread_mutex_lock(&pcd.lock);
	for (int i = 0; i < CACHE_SECTORS; i++) pcd.cache[i].lba = -1;
	pthread_mutex_unlock(&pcd.lock);

	pcd.sub_ok = -1;
	probe_subchannel(toc->tracks[0].start + 16);

	pcd.cursor = toc->tracks[0].start;
	pcd.leadout = lead.cdte_addr.lba;         /* unblocks prefetch    */

	printf("\x1b[32mphyscd: toc loaded, %d tracks, leadout %d, subchannel %s\n\x1b[0m",
		n, pcd.leadout,
		pcd.sub_ok == 1 ? "yes" : pcd.sub_ok == 0 ? "no" : "unknown");
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
	pcd.leadout = 0;
	pcd.ntrk = 0;
	pcd.first_data_lba = -1;
}
