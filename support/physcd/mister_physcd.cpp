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
#define NWIN 2                        /* one window per stream          */
#define WIN_SECTORS (CACHE_SECTORS / NWIN)
#define SLOT_SIZE (PHYSCD_RAW + PHYSCD_SUB)
#define READAHEAD 96                  /* sectors ahead, per window      */
#define BURST 16                      /* sectors per drive transaction  */
#define STATS_MS 5000

/*
 * a cache miss is serviced ON THE MAIN THREAD, which is also the thread
 * answering the fpga. so every path a miss can take must be bounded:
 * an 8s timeout with 3 retries over a 16-sector burst is ~6 MINUTES of
 * frozen main loop on a disc that reads badly, which presents as the
 * whole mister locking up with no osd and no input.
 *
 * so the consumer path gets ONE attempt, a small burst and a timeout
 * just above the worst seek seen on real media (2.5s on a marginal psx
 * disc), and gives up to zeros instead of retrying. retries and the
 * cooked fallback belong to the prefetch thread, where blocking is
 * free. sync_pending lets that thread yield rather than make the
 * consumer queue behind a slow background read.
 */
#define SYNC_BURST 8
#define SYNC_TIMEOUT_MS 3000
#define BG_TIMEOUT_MS 3000

/*
 * mixed-mode discs read two streams at once: the core pulls animation
 * or game data from a data track while cdda plays from an audio track
 * thousands of sectors away (sonic cd's intro is the canonical case).
 * a single prefetch cursor ping-pongs between them and never gets
 * ahead of either, and a cache mapped `lba % CACHE_SECTORS` lets the
 * two streams evict each other wherever they happen to be congruent.
 * so the cache is split into one window per track type - window 0 for
 * data, window 1 for cdda - each with its own cursor and its own slice
 * of slots. the two streams can no longer collide or fight.
 */
typedef struct {
	int lba;                      /* -1 = empty                     */
	int has_sub;
	int bad;                      /* zero-filled, sector unreadable */
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
	volatile int cursor[NWIN];    /* prefetch position, per window  */
	volatile int wactive[NWIN];
	volatile int running;
	volatile int sync_pending;    /* consumer waiting: prefetch yields */
	pthread_t thread;
	pthread_mutex_t lock;         /* guards cache slots             */
	pthread_mutex_t io;           /* one drive transaction at a time */
	uint32_t st_hit, st_miss;     /* telemetry, see stats_report()  */
	uint32_t st_bad;              /* sectors served as zeros        */
	uint32_t st_bad_logged;
	double st_worst_ms;           /* worst consumer-visible miss    */
	double st_worst_io_ms;        /* worst drive transaction        */
} pcd = { -1, 0, -1, -1, {}, 0, NULL, {0,0}, {0,0}, 0, 0, 0,
	  PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
	  0, 0, 0, 0, 0.0, 0.0 };

static int track_of(int lba)
{
	for (int i = 0; i < pcd.ntrk; i++)
		if (lba < pcd.trk[i].end) return i;
	return pcd.ntrk ? pcd.ntrk - 1 : -1;
}

static inline int win_of(int lba)
{
	int t = track_of(lba);
	return (t >= 0 && pcd.trk[t].audio) ? 1 : 0;
}

static inline slot_t *slot_for(int lba)
{
	return &pcd.cache[win_of(lba) * WIN_SECTORS + (lba % WIN_SECTORS)];
}

static double now_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ---------------------------------------------------------------- reads

static int sg_read_cd(int lba, int count, uint8_t flags, int with_sub, uint8_t *dst, int timeout_ms)
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
	io.timeout = timeout_ms;

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
static int fill_cache(int lba, int count, int sync)
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

	/* one transaction at a time: a synchronous miss and the prefetch
	   thread issuing concurrent reads just makes the head seesaw and
	   both take longer than they would serialized */
	double io0 = now_ms();
	pthread_mutex_lock(&pcd.io);
	int r = sg_read_cd(lba, count, flags, with_sub, burst,
		sync ? SYNC_TIMEOUT_MS : BG_TIMEOUT_MS);
	pthread_mutex_unlock(&pcd.io);

	if (r && with_sub && count > 1) {
		/* the subchannel probe only ever reads ONE sector, and some
		   usb bridges accept that but reject a multi-sector transfer
		   with subchannel appended - which would fail every burst.
		   re-try the same burst plain before condemning any sectors,
		   and if that works, drop subchannel for the rest of the
		   session. this must run BEFORE the sync bail below: on such
		   a bridge every sync fill would otherwise fail forever, and
		   the cost here is one extra attempt exactly once, because
		   success disables subchannel session-wide. */
		pthread_mutex_lock(&pcd.io);
		int r2 = sg_read_cd(lba, count, flags, 0, burst,
			sync ? SYNC_TIMEOUT_MS : BG_TIMEOUT_MS);
		pthread_mutex_unlock(&pcd.io);
		if (!r2) {
			printf("physcd: drive rejects multi-sector subchannel reads, disabling subchannel\n");
			pcd.sub_ok = 0;
			with_sub = 0;
			r = 0;
		}
	}

	if (r && sync) {
		/* bounded: give up rather than block the thread that is also
		   answering the fpga - but do NOT claim these slots. stamping
		   s->lba on zero-filled slots would make them permanent cache
		   HITS: the prefetch scan only targets slots whose lba does
		   not match, so nothing would ever re-read them and one 3s
		   hiccup would serve zeros for the rest of the mount. leaving
		   them missing lets the caller serve zeros once while the
		   prefetch thread fetches them properly, with the retry ladder
		   and the cooked fallback. */
		double dt = now_ms() - io0;
		if (dt > pcd.st_worst_io_ms) pcd.st_worst_io_ms = dt;
		return -1;
	}

	if (r) {
		for (int i = 0; i < count; i++) {
			uint8_t one[SLOT_SIZE];
			int rr = -1;
			/* lock per ATTEMPT, not around the whole retry loop: a
			   consumer miss must never queue behind a retry storm */
			for (int n = 0; n < 3 && rr; n++) {
				pthread_mutex_lock(&pcd.io);
				rr = sg_read_cd(lba + i, 1, flags, 0, one, BG_TIMEOUT_MS);
				pthread_mutex_unlock(&pcd.io);
			}
			if (rr) {
				pthread_mutex_lock(&pcd.io);
				rr = cooked_read_raw(lba + i, one);
				pthread_mutex_unlock(&pcd.io);
			}
			pthread_mutex_lock(&pcd.lock);
			slot_t *s = slot_for(lba + i);
			if (!rr) {
				memcpy(s->data, one, PHYSCD_RAW);
				s->bad = 0;
			} else {
				/* unreadable: serve zeros so the core never hangs.
				   this is silent corruption by design, so COUNT it -
				   a zero-filled slot is still a cache hit later and
				   would otherwise hide a rotting disc behind a 100%
				   hit rate. */
				memset(s->data, 0, PHYSCD_RAW);
				s->bad = 1;
				pcd.st_bad++;
				/* never log per sector in the hot path: with output
				   on /dev/ttyS0 each line blocks on serial for ms and
				   a bad region would stall the core by logging. */
				if (pcd.st_bad_logged < 8) {
					pcd.st_bad_logged++;
					printf("physcd: unreadable sector lba=%d%s\n", lba + i,
						pcd.st_bad_logged == 8 ? " (further ones counted silently)" : "");
				}
			}
			memset(s->data + PHYSCD_RAW, 0, PHYSCD_SUB);
			s->has_sub = 0;
			s->lba = lba + i;
			pthread_mutex_unlock(&pcd.lock);
		}
		double d = now_ms() - io0;
		if (d > pcd.st_worst_io_ms) pcd.st_worst_io_ms = d;
		return 0;
	}

	double d = now_ms() - io0;
	if (d > pcd.st_worst_io_ms) pcd.st_worst_io_ms = d;

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

/* dump counters so a stutter can be pinned on the cache instead of
   guessed at. cleared each interval; only written while streaming. */
static void stats_report()
{
	FILE *f = fopen("/tmp/physcd_stats.log", "w");
	if (f) {
		fprintf(f, "hit %u miss %u  hitrate %.1f%%  worst miss %.0f ms\n",
			pcd.st_hit, pcd.st_miss,
			(pcd.st_hit + pcd.st_miss) ? 100.0 * pcd.st_hit / (pcd.st_hit + pcd.st_miss) : 0.0,
			pcd.st_worst_ms);
		/* BAD is the number that matters on an aging disc: those
		   sectors were served to the core as zeros and counted as
		   hits. nonzero here means the disc, not the cache. */
		fprintf(f, "BAD %u sectors served as zeros  worst drive io %.0f ms\n",
			pcd.st_bad, pcd.st_worst_io_ms);
		fprintf(f, "data  window: active %d cursor %d\n", pcd.wactive[0], pcd.cursor[0]);
		fprintf(f, "cdda  window: active %d cursor %d\n", pcd.wactive[1], pcd.cursor[1]);
		fclose(f);
	}
	pcd.st_hit = pcd.st_miss = 0;
	pcd.st_worst_ms = 0.0;
	pcd.st_worst_io_ms = 0.0;
	/* st_bad is cumulative for the mount - a running total is what
	   you want when hunting an intermittent read problem */
}

static void *prefetch_thread(void *arg)
{
	(void)arg;
	int rr = 0;                                /* round-robin start   */
	double last_stats = now_ms();

	while (pcd.running) {
		int target = -1;

		/* serve the neediest active window, alternating which one
		   gets looked at first so neither stream starves */
		pthread_mutex_lock(&pcd.lock);
		for (int n = 0; n < NWIN && target < 0; n++) {
			int w = (rr + n) % NWIN;
			if (!pcd.wactive[w]) continue;
			int pos = pcd.cursor[w];
			for (int i = 0; i < READAHEAD; i++) {
				int lba = pos + i;
				if (lba >= pcd.leadout) break;
				if (win_of(lba) != w) break;   /* left the stream */
				if (slot_for(lba)->lba != lba) { target = lba; break; }
			}
		}
		pthread_mutex_unlock(&pcd.lock);
		rr = (rr + 1) % NWIN;

		if (now_ms() - last_stats >= STATS_MS) {
			if (pcd.st_hit || pcd.st_miss) stats_report();
			last_stats = now_ms();
		}

		/* a consumer read is waiting: do not start a new transaction
		   and make it queue behind us */
		if (pcd.sync_pending) {
			struct timespec ts = { 0, 2 * 1000 * 1000 };
			nanosleep(&ts, NULL);
			continue;
		}

		if (target < 0) {
			/* both windows full (or no toc yet): check again soon */
			struct timespec ts = { 0, 20 * 1000 * 1000 };
			nanosleep(&ts, NULL);
			continue;
		}
		fill_cache(target, BURST, 0);
	}
	return NULL;
}

// ---------------------------------------------------------------- api

static char pref_dev[64] = "";       /* "" = autodetect */
static char cur_dev[64] = "";        /* what we actually opened */

void physcd_set_device(const char *dev)
{
	if (dev && *dev) snprintf(pref_dev, sizeof(pref_dev), "%s", dev);
	else pref_dev[0] = 0;
}

const char *physcd_device_name()
{
	return cur_dev;
}

/*
 * usb enumeration order is not stable: the same drive comes up as
 * /dev/sr0 one boot and /dev/sr1 the next (a card reader or a second
 * usb storage device claiming the earlier minor is enough to shift
 * it). so never assume a name - scan, and prefer a drive with media.
 */
static int open_drive(char *out, int outsz)
{
	char path[64];
	int spare = -1;

	if (pref_dev[0]) {
		int fd = open(pref_dev, O_RDONLY | O_NONBLOCK);
		if (fd >= 0) { snprintf(out, outsz, "%s", pref_dev); return fd; }
		printf("physcd: %s not available (%s), scanning\n", pref_dev, strerror(errno));
	}

	for (int i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "/dev/sr%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0) continue;

		if (ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT) == CDS_DISC_OK) {
			snprintf(out, outsz, "%s", path);
			if (spare >= 0) close(spare);
			return fd;
		}
		/* opens but empty: remember it in case nothing has a disc */
		if (spare < 0) { spare = fd; snprintf(out, outsz, "%s", path); }
		else close(fd);
	}
	return spare;
}

int physcd_open(const char *dev)
{
	if (dev && *dev) physcd_set_device(dev);

	if (pcd.fd >= 0) {
		/* already open on the drive we want? */
		if (!pref_dev[0] || !strcmp(pref_dev, cur_dev)) return 0;
		physcd_close();
	}

	pcd.fd = open_drive(cur_dev, sizeof(cur_dev));
	if (pcd.fd < 0) {
		cur_dev[0] = 0;
		printf("physcd: no cd-rom drive found (looked at /dev/sr0../dev/sr7)\n");
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
	for (int w = 0; w < NWIN; w++) { pcd.cursor[w] = 0; pcd.wactive[w] = 0; }
	pcd.st_hit = pcd.st_miss = 0;
	pcd.st_worst_ms = 0.0;
	pcd.running = 1;
	pthread_create(&pcd.thread, NULL, prefetch_thread, NULL);

	/* main() pins the process to cpu1 for fpga latency; let the
	   prefetch thread run on either core so it doesn't compete */
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(0, &set);
	CPU_SET(1, &set);
	pthread_setaffinity_np(pcd.thread, sizeof(set), &set);

	printf("\x1b[32mphyscd: opened %s\n\x1b[0m", cur_dev);
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

	if (!sg_read_cd(lba, 1, flags, 1, buf, BG_TIMEOUT_MS)) { pcd.sub_ok = 1; return; }
	if (!sg_read_cd(lba, 1, flags, 0, buf, BG_TIMEOUT_MS)) { pcd.sub_ok = 0; return; }
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

	/* cdth_trk0/trk1 come straight from the drive: a blank, unfinalized
	   or flaky-bridge answer with trk0 > trk1 skips the loop entirely,
	   and tracks[n-1] would then write ~450 bytes BEFORE the caller's
	   toc_t (and pcd.trk[-1].end aliases first_data_lba). */
	if (n < 1) {
		printf("physcd: drive reported no tracks (%d-%d)\n", hdr.cdth_trk0, hdr.cdth_trk1);
		return -1;
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

	/* prime the data window at track 1; the cdda window activates on
	   its first read so we don't spin the head over audio nobody
	   asked for yet */
	for (int w = 0; w < NWIN; w++) { pcd.cursor[w] = 0; pcd.wactive[w] = 0; }
	pcd.cursor[0] = toc->tracks[0].start;
	pcd.wactive[0] = 1;
	pcd.st_hit = pcd.st_miss = pcd.st_bad = pcd.st_bad_logged = 0;
	pcd.st_worst_ms = pcd.st_worst_io_ms = 0.0;

	pcd.leadout = lead.cdte_addr.lba;         /* unblocks prefetch    */

	printf("\x1b[32mphyscd: toc loaded, %d tracks, leadout %d, subchannel %s\n\x1b[0m",
		n, pcd.leadout,
		pcd.sub_ok == 1 ? "yes" : pcd.sub_ok == 0 ? "no" : "unknown");
	return 0;
}

void physcd_seek_hint(int lba)
{
	if (lba < 0 || !pcd.ntrk) return;

	/* retarget only the stream that actually moved */
	int w = win_of(lba);
	pcd.cursor[w] = lba;
	pcd.wactive[w] = 1;
}

/* shared implementation; sub_valid (optional) reports whether sub96
   actually received subchannel data rather than zeros */
static int read_sector_impl(int lba, uint8_t *dst, uint8_t *sub96, int *sub_valid)
{
	if (sub_valid) *sub_valid = 0;

	if (pcd.fd < 0 || lba < 0 || lba >= pcd.leadout) {
		/* always leave dst defined: megacd's ReadCDDA ignores the
		   return value and would otherwise ship an uninitialized
		   stack buffer to the fpga */
		memset(dst, 0, PHYSCD_RAW);
		if (sub96) memset(sub96, 0, PHYSCD_SUB);
		return -1;
	}

	int w = win_of(lba);
	pcd.wactive[w] = 1;

	pthread_mutex_lock(&pcd.lock);
	slot_t *s = slot_for(lba);
	int hit = (s->lba == lba);
	if (hit) {
		memcpy(dst, s->data, PHYSCD_RAW);
		if (sub96) memcpy(sub96, s->data + PHYSCD_RAW, PHYSCD_SUB);
		if (sub_valid) *sub_valid = s->has_sub;
	}
	pthread_mutex_unlock(&pcd.lock);

	if (hit) {
		/* advance this stream's prefetch, leave the other alone */
		if (lba >= pcd.cursor[w]) pcd.cursor[w] = lba + 1;
		pcd.st_hit++;
		return 0;
	}

	/* cache miss: bounded synchronous fill, then serve             */
	double t0 = now_ms();
	pcd.sync_pending++;
	int fr = fill_cache(lba, SYNC_BURST, 1);
	pcd.sync_pending--;

	pcd.st_miss++;
	double d = now_ms() - t0;
	if (d > pcd.st_worst_ms) pcd.st_worst_ms = d;

	if (!fr) {
		pthread_mutex_lock(&pcd.lock);
		s = slot_for(lba);
		hit = (s->lba == lba);
		if (hit) {
			memcpy(dst, s->data, PHYSCD_RAW);
			if (sub96) memcpy(sub96, s->data + PHYSCD_RAW, PHYSCD_SUB);
			if (sub_valid) *sub_valid = s->has_sub;
		}
		pthread_mutex_unlock(&pcd.lock);

		if (hit) {
			pcd.cursor[w] = lba + 1;
			return 0;
		}
	}

	/* couldn't serve it in time: hand back zeros THIS ONCE (never
	   cached, see fill_cache) so the core gets defined data instead of
	   a stale buffer, and leave the prefetch cursor pointing here so
	   the background path re-reads it properly. */
	memset(dst, 0, PHYSCD_RAW);
	if (sub96) memset(sub96, 0, PHYSCD_SUB);
	pcd.st_bad++;
	pcd.cursor[w] = lba;
	return 0;
}

int physcd_read_sector(int lba, uint8_t *dst, uint8_t *sub96)
{
	return read_sector_impl(lba, dst, sub96, NULL);
}

int physcd_read_sector_sub(int lba, uint8_t *dst, uint8_t *sub96)
{
	int valid = 0;
	if (read_sector_impl(lba, dst, sub96, &valid)) return 0;
	return valid;
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

physcd_region_t physcd_region_from_md_header(const uint8_t *hdr, int len)
{
	if (!hdr || len < 0x1F3) return PHYSCD_REGION_UNKNOWN;

	/* mega drive style header: "SEGA..." at 0x100, region at 0x1F0.
	   mega cd discs mirror this in their first data sector and mega cd
	   bios roms carry it too, so one parse serves both. */
	if (memcmp(hdr + 0x100, "SEGA", 4)) return PHYSCD_REGION_UNKNOWN;

	const char *f = (const char *)hdr + 0x1F0;
	int has_j = 0, has_u = 0, has_e = 0, junk = 0;
	for (int i = 0; i < 3; i++) {
		char c = f[i];
		if (c == 'J') has_j = 1;
		else if (c == 'U') has_u = 1;
		else if (c == 'E') has_e = 1;
		else if (c != ' ' && c != 0) junk = 1;
	}

	/* older discs spell the region with J/U/E letters. a bare 'E' is
	   ambiguous with the newer hex bitfield (where E = 14 = europe +
	   americas + japan-pal), but reading it as europe is right for
	   european releases and still picks a region the disc supports
	   either way. multi-region discs prefer US, then EU, then JP -
	   only reachable when every choice is valid for that disc. */
	if (!junk && (has_j || has_u || has_e)) {
		if (has_u) return PHYSCD_REGION_US;
		if (has_e) return PHYSCD_REGION_EU;
		return PHYSCD_REGION_JP;
	}

	/* newer style: hex bitfield, bit0 japan / bit2 americas / bit3 europe */
	int v = -1;
	if (f[0] >= '0' && f[0] <= '9') v = f[0] - '0';
	else if (f[0] >= 'A' && f[0] <= 'F') v = f[0] - 'A' + 10;
	if (v > 0) {
		if (v & 4) return PHYSCD_REGION_US;
		if (v & 8) return PHYSCD_REGION_EU;
		if (v & 1) return PHYSCD_REGION_JP;
	}

	return PHYSCD_REGION_UNKNOWN;
}

physcd_region_t physcd_region()
{
	uint8_t user[2048];

	if (!physcd_disc_present()) return PHYSCD_REGION_UNKNOWN;
	if (pcd.first_data_lba < 0) {
		toc_t tmp;
		if (physcd_load_toc(&tmp)) return PHYSCD_REGION_UNKNOWN;
		if (pcd.first_data_lba < 0) return PHYSCD_REGION_UNKNOWN;
	}

	if (physcd_read_data2048(pcd.first_data_lba, user)) return PHYSCD_REGION_UNKNOWN;
	return physcd_region_from_md_header(user, sizeof(user));
}

const char *physcd_region_name(physcd_region_t r)
{
	switch (r) {
	case PHYSCD_REGION_JP: return "JP";
	case PHYSCD_REGION_US: return "US";
	case PHYSCD_REGION_EU: return "EU";
	default:               return "";
	}
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
	cur_dev[0] = 0;
}
