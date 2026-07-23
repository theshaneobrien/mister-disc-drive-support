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
#include "physcd_acoustic.h"

// ---------------------------------------------------------------- state

#define CACHE_SECTORS 4096            /* 4096 * 2448B ~= 9.5MB of ram   */
#define NWIN 2                        /* one window per stream          */
#define WIN_SECTORS (CACHE_SECTORS / NWIN)
#define SLOT_SIZE (PHYSCD_RAW + PHYSCD_SUB)
#define READAHEAD 96                  /* sectors ahead, per window      */
#define BURST 16                      /* sectors per drive transaction  */
#define PREWARM_SECTORS 768           /* cold-start spin-up read (~10s)  */
#define STATS_MS 5000
#define SWAP_CHECK_MS 500             /* eject/insert poll interval - NOT the tray dwell; equals PHYSCD_SWAP_DWELL_MS only by coincidence */

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
 * drive speed cap.
 *
 * the real requirement is 172 KB/s sustained (1x raw cdda), and about
 * 353 KB/s for the worst realistic case - a data stream and cdda
 * together, or psx's 2x mode. asking the drive for MAXIMUM speed buys
 * nothing above that and costs reliability: a dvd writer will spin a
 * cd as fast as it can, tracking errors scale with rpm on a warped,
 * dirty or 30-year-old disc, and the outer edge - where rot starts and
 * where it spins fastest - is precisely where our own discs read worst
 * (cdrdao stalled for a minute per minute of audio out there). it also
 * runs hotter and louder, and this drive class has already wedged into
 * a no-media state twice under sustained load; our prefetcher works a
 * drive far harder than a real console ever did, holding it open and
 * pulling 96 sectors ahead of both streams for a whole session.
 *
 * 4x leaves ~2x headroom over the worst case. note the measured rates
 * at "maximum" were only 741-1072 KB/s (~4-6x, CAV), so this mostly
 * reins in the outer edge and costs nothing at the inner one.
 */
#define PHYSCD_SPEED_NX 4

/*
 * idle keep-alive.
 *
 * once a level is in ram a game can go minutes without touching the
 * disc, and the prefetcher goes quiet as soon as its window is full.
 * an idle usb optical drive then spins down - and linux may also
 * autosuspend the usb device underneath it. waking that back up is
 * where this drive misbehaves: observed as a multi-minute stall with
 * the drive audibly spinning down, then back up, and dmesg full of
 * "reset high-speed USB device" (which is the host trying to recover a
 * device that did not answer in time). the giveaway was that x-files -
 * wall-to-wall fmv, i.e. never idle - ran clean for 15 minutes while
 * sonic cd's time travel and wipeout's race start both stalled: those
 * are exactly the moments AFTER a quiet spell.
 *
 * so keep the drive awake while a disc is mounted by touching it
 * every so often. one sector, at the current cursor, result discarded
 * - it also keeps the head near where the next read will want it.
 * only while mounted: at the menu the drive should be free to sleep.
 */
#define KEEPALIVE_MS 15000

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
	volatile int fd;              /* read lock-free by the acoustic thread via physcd_drive_busy */
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
	volatile int consec_fail;     /* consecutive unreadable bursts  */
	uint32_t st_reattach;         /* times the drive came back      */
	volatile int watch_mode;      /* menu is watching for a disc    */
	volatile int ev;              /* physcd_event_t, pending        */
	volatile int ev_type;
	volatile int ev_region;
	volatile int ev_initial;      /* disc was already in at watch start */
	int watch_present;            /* menu snapshot, guarded by pcd.lock */
	int watch_type;
	char watch_label[64];
	volatile int watch_dirty;     /* presence changed since last query  */
	volatile int swap_enable;     /* arm mid-mount physical disc-swap detection */
	volatile int swap_ready;      /* a swap happened; the new toc is loaded     */
	volatile int swapping;        /* reload in flight: the read path serves zeros */
	volatile int last_win;        /* window of the most recent real read; -1 = none yet */
	volatile int prewarm;         /* cold-start spin-up: next lba to pre-read; -1 = idle */
	volatile int prewarm_end;     /* stop pre-reading at this lba */
	volatile int swap_ejected;    /* mid-swap: disc physically out, new toc not loaded yet */
	int uncap;                    /* opt-in data-only speed uncap (physcd_speed_uncap) */
	/* edc integrity stats (stats-only, see the edc section). same
	   convention as st_hit/st_bad: plain counters, telemetry noise from
	   the rare cross-thread race is accepted. */
	uint32_t st_edc_checked;      /* data sectors whose edc was actually verified */
	uint32_t st_edc_bad;          /* ...that failed: the drive delivered a misread */
	uint32_t st_form_bad;         /* malformed structure (sync/mode/subheader) */
	uint32_t st_edc_logged;       /* log cap, mirrors st_bad_logged */
	int st_edc_last_lba;          /* -1 = none */
} pcd = { -1, 0, -1, -1, {}, 0, NULL, {0,0}, {0,0}, 0, 0, 0,
	  PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
	  0, 0, 0, 0, 0.0, 0.0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, 0, 0, 0, 0, -1, -1, 0, 0, 0,
	  0, 0, 0, 0, -1 };

/* "a swap happened during this mount" must survive the core-exit exec (the
   menu runs in a FRESH process - fpga_load_rbf execs; see physcd_autoboot.h,
   it is why the autoboot markers are files). same trick here. */
#define PHYSCD_SWAPPED_MARKER "/tmp/physcd_swapped"

static int track_of(int lba)
{
	for (int i = 0; i < pcd.ntrk; i++)
		if (lba < pcd.trk[i].end) return i;
	return pcd.ntrk ? pcd.ntrk - 1 : -1;
}

static char pref_dev[64] = "";       /* "" = autodetect */
static char cur_dev[64] = "";        /* what we actually opened */

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

/* linux turns Nx into kB/s (N*177) for GPCMD_SET_SPEED; 0 means maximum.
   plenty of drives ignore the command entirely - best effort, and
   re-applied per disc because a media change resets it on many.

   DATA-ONLY discs run UNCAPPED (drive native speed management, i.e. CAV -
   constant rpm): the cap puts the drive in a constant-data-rate mode where
   a long-throw seek pays a spindle speed-match on top of the sled move,
   and measured WARM random access was ~294ms - which loses the cd-i
   core's hardcoded 250ms simulated-seek grace on EVERY voice-clip seek
   (the fmv/voice "doubled syllables": the flushed core fifo starves a few
   75Hz ticks and the CDIC replays the last audio buffer). games that
   stream live from disc (cd-i) jump between level data and voice banks
   half a disc apart, so long-throw latency IS the product. this drive's
   native max is only ~4-6x anyway (comment at PHYSCD_SPEED_NX), so
   uncapping changes the speed-management mode, not really the speed.
   discs WITH audio tracks keep the cap: cdda is sequential (no long
   throws to hide) and 4x keeps old/warped albums readable and the drive
   quiet under music. */
static void set_speed_cap()
{
	if (pcd.fd < 0) return;

	/* the uncap is OPT-IN per core (physcd_speed_uncap), not blanket: only a
	   core that streams live and long-throws mid-stream against a hard
	   deadline (cd-i) benefits, and the proven-at-4x cores keep their exact
	   drive profile. even opted in, a disc WITH audio tracks stays capped. */
	int audio = 0;
	for (int i = 0; i < pcd.ntrk; i++)
		if (pcd.trk[i].audio) audio = 1;
	int nx = (pcd.uncap && pcd.ntrk > 0 && !audio) ? 0 : PHYSCD_SPEED_NX;

	if (ioctl(pcd.fd, CDROM_SELECT_SPEED, nx) < 0)
		printf("physcd: speed cap not supported, drive keeps its default\n");
	else if (nx)
		printf("physcd: speed capped at %dx (~%d KB/s, need 172)\n", nx, nx * 177);
	else
		printf("physcd: data-only disc - speed uncapped (native CAV, fast long seeks)\n");
}

/* opt this mount into the data-only speed uncap (see set_speed_cap). call
   between physcd_open and physcd_load_toc; cleared by physcd_close so no
   other core inherits it. */
void physcd_speed_uncap(int enable)
{
	pcd.uncap = enable ? 1 : 0;
	if (pcd.fd >= 0 && pcd.ntrk > 0) set_speed_cap();
}

/* defang the kernel's block-layer probing of our drive. after a media change,
   the first plain open of /dev/srN (udev and the partition rescan) reads the
   START and the END of the device through the page cache - the end because the
   GPT backup header lives there. on a disc that ends in AUDIO tracks, cooked
   READ(10) is illegal there ("illegal mode for this track"), and the big
   readahead bursts those probes generate (60-block, 10+ segment scatterlists)
   wedge some usb bridges hard: dwc2 scatterlist error, a 30-SECOND block-layer
   timeout, then a usb reset - observed twice per Policenauts session, which is
   why an audio-tailed disc "loads slow" while a data-tailed one flies. we
   cannot stop the probe, but with readahead at zero its reads shrink to single
   pages that fail in milliseconds instead of wedging the bridge. our own SG_IO
   reads bypass the page cache entirely, so this costs us nothing. */
static void quiet_block_probes(const char *dev)
{
	const char *name = strrchr(dev, '/');
	name = name ? name + 1 : dev;

	char path[128];
	FILE *f;

	/* kill big readahead so the kernel's probe reads are small */
	snprintf(path, sizeof(path), "/sys/block/%s/queue/read_ahead_kb", name);
	if ((f = fopen(path, "w"))) { fputs("0", f); fclose(f); }

	/* the kernel's disc-event poll must stay ON: udev deliberately LOCKS the
	   tray on insert ("lock tray to enable the receiving of media eject
	   button events", 60-cdrom_id.rules), which turns the eject button into
	   a soft REQUEST the drive merely queues - and this poll is the relay
	   that fetches it (GET_EVENT -> DISK_EJECT_REQUEST uevent -> cdrom_id
	   --eject-media unlocks and opens). v0.4.0 wrote 0 here (a leftover from
	   a FALSIFIED choppiness theory - the real fix was the blkid udev rule)
	   and that killed the eject button on DATA discs across ALL cores (psx,
	   saturn, megacd, pce, cd-i - hardware-confirmed; audio cds escaped,
	   most likely because the tray is only locked for data media). also
	   hardware-confirmed: with the button dead, echo 2000 >
	   events_poll_msecs popped the queued eject press at once.
	   write -1 (= follow the kernel default, 2000ms via 60-block.rules) so
	   a device a v0.4.0 binary already zeroed is HEALED, not just left be.
	   the poll is a status command - no media reads, no head movement - so
	   the cd-i seesaw fix (the blkid rule) is unaffected. */
	snprintf(path, sizeof(path), "/sys/block/%s/events_poll_msecs", name);
	if ((f = fopen(path, "w"))) { fputs("-1", f); fclose(f); }

	printf("physcd: block probes quieted for %s (readahead off, eject relay on)\n", name);
}

/* startup environment fix: exempt cd drives from udev's blkid superblock
 * probing, via a persistent rules file.
 *
 * THE root cause of the cd-i cold-load choppiness + fmv audio doubling
 * (found by reading the shipped mister rootfs + the 5.15 kernel source, and
 * consistent with every hardware falsification): 60-persistent-storage.rules
 * runs the blkid BUILTIN inside a udevd worker for any sr* device whose
 * media has a data track. blkid buffered-reads the superblock probe chain -
 * blocks 0/8/16/24/56/128, exactly the dmesg spew - and on a mode-2 disc
 * every read grinds the drive 1-3s then fails ASC 0x64. The worker runs to
 * udev's 180s event timeout (the storm "resolving itself"), seesawing the
 * head against the game's deep reads the whole time. The log fingerprint
 * proves it is blkid: "async page read" only comes from the bdev pagecache
 * path, and the kernel's own partition scan reads ONLY block 0 - blocks 8+
 * can only be a userspace read(). Why every runtime countermeasure failed:
 * the triggering event is the BOOT COLDPLUG (S10udev's `udevadm trigger`),
 * which fires long before Main starts, and stop-exec-queue cannot touch an
 * in-flight worker (falsified on hardware, cditest5/6). Only a rules file
 * already on disk at boot prevents it.
 *
 * The exemption uses the escape hatch upstream provides for exactly this
 * (60-persistent-storage.rules honors UDEV_DISABLE_PERSISTENT_STORAGE_RULES_
 * FLAG at its top); mister's own rootfs ships a defang rule of the same
 * class (60-jms583-phantom.rules). Nothing on mister consumes blkid's cd
 * fingerprints - usbmount matches only sd and ub devices - so the only loss
 * is ID_FS_* properties nobody reads. cdrom_id (SG_IO, harmless) still runs.
 * Idempotent: rewrites only when missing/stale; the current boot's storm is
 * already past by the time we run, so the rule pays off from the NEXT boot
 * and for every insert/swap event from now on. */
static void install_udev_rule(void)
{
	static const char *path = "/etc/udev/rules.d/59-physcd-cdrom.rules";
	static const char *rule =
		"# installed by physcd (MiSTer-Disc fork). stops udev's blkid builtin\n"
		"# from superblock-probing cd drives: a mode-2 disc (cd-i, psx) rejects\n"
		"# cooked reads, so each probe grinds the drive for seconds and fights\n"
		"# the running game for the drive head. safe: nothing on mister mounts\n"
		"# or fingerprints cds (usbmount matches sd*/ub* only). delete this file\n"
		"# and physcd will reinstall it on next start; remove the physcd binary\n"
		"# and this rule is inert but harmless.\n"
		"ACTION!=\"remove\", KERNEL==\"sr[0-9]*\", ENV{UDEV_DISABLE_PERSISTENT_STORAGE_RULES_FLAG}=\"1\"\n";

	/* length-aware compare: a truncating read would make strcmp never match
	   and silently rewrite + udevadm-reload on EVERY process start. size the
	   buffer from the rule and treat an oversized on-disk file as stale. */
	size_t rlen = strlen(rule);
	char cur[1024] = {};
	FILE *f = fopen(path, "r");
	if (f) {
		size_t got = fread(cur, 1, sizeof(cur) - 1, f);
		fclose(f);
		if (got == rlen && rlen < sizeof(cur) && !memcmp(cur, rule, rlen))
			return;   /* already installed, current text */
	}

	f = fopen(path, "w");
	if (!f) { printf("physcd: cannot write %s\n", path); return; }
	fputs(rule, f);
	fflush(f);
	fsync(fileno(f));
	fclose(f);

	/* eudev spells it --reload-rules, newer udevadm --reload; try both.
	   PATH must be explicit - system()'s shell lacks /sbin (bit us once). */
	system("export PATH=/usr/sbin:/sbin:/usr/bin:/bin:$PATH; "
	       "udevadm control --reload-rules 2>/dev/null || udevadm control --reload 2>/dev/null");
	printf("physcd: installed %s (cd drives exempt from udev blkid probing from next boot)\n", path);
}

void physcd_quiet_udev(void)
{
	install_udev_rule();
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
/* ------------------------------------------------------------------ edc
 *
 * error detection code of cd-rom data sectors, clean-room from ECMA-130
 * (2nd ed.) section 14.3: a 32-bit crc over the sector with generator
 * P(x) = (x^16+x^15+x^2+1)(x^16+x^2+x+1) = 0x8001801B, "least significant
 * bit of a data byte is used first" = a REFLECTED crc, so the table is
 * built against the bit-reversed constant 0xD8018001. seed 0, no final
 * xor, stored little-endian at the per-mode edc offset.
 *
 * coverage (cross-checked against ECMA-130 + public tooling docs):
 *   mode 1:        edc at 2064 over bytes 0..2063 (sync+header INcluded)
 *   mode 2 form 1: edc at 2072 over bytes 16..2071 (sync+header EXcluded)
 *   mode 2 form 2: edc at 2348 over bytes 16..2347, OPTIONAL - an all-zero
 *                  field means "not computed" and cannot be judged
 *
 * STATS-ONLY groundwork: sectors are counted, never rejected - the read
 * path behaves byte-identically to v0.4.0. enforcement (retry-on-bad) is
 * a separate future decision, made after these counters have described
 * what real discs look like. audio sectors carry no edc and are skipped
 * by the callers (track type gates the call). */
static uint32_t edc_lut[256];
static int edc_on = 0;                /* armed only if the self-test passes */

static void edc_init(void)
{
	static int done = 0;
	if (done) return;
	done = 1;

	for (uint32_t i = 0; i < 256; i++) {
		uint32_t e = i;
		for (int j = 0; j < 8; j++)
			e = (e >> 1) ^ ((e & 1) ? 0xD8018001u : 0);
		edc_lut[i] = e;
	}

	/* self-test vector: sync + header (msf 00:02:00, mode 1). the expected
	   crc 0x91FE029E was verified THREE independent ways against this exact
	   algorithm: a table-driven reproduction (lut[1]=0x90910101 matches the
	   canonical cd-rom edc table), a from-scratch bit-by-bit reflected crc,
	   and the append-edc-yields-zero property (appending 0x91FE029E LE and
	   re-checksumming gives 0, which only the true crc of the vector can do).
	   a bit-order/constant/table mistake in the build above cannot pass this.
	   on failure the whole feature disarms - stats must never affect reads. */
	static const uint8_t tv[16] = { 0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
	                                0xFF,0xFF,0xFF,0x00,0x00,0x02,0x00,0x01 };
	uint32_t e = 0;
	for (int i = 0; i < 16; i++) e = (e >> 8) ^ edc_lut[(e ^ tv[i]) & 0xFF];
	edc_on = (e == 0x91FE029Eu);
	printf(edc_on ? "physcd: edc self-test ok\n"
	              : "physcd: edc self-test FAILED - integrity stats disabled\n");
}

static uint32_t edc_compute(const uint8_t *p, int len)
{
	uint32_t e = 0;
	while (len--) e = (e >> 8) ^ edc_lut[(e ^ *p++) & 0xFF];
	return e;
}

static uint32_t edc_stored(const uint8_t *p)      /* little-endian on disc */
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const uint8_t edc_sync[12] = { 0x00,0xFF,0xFF,0xFF,0xFF,0xFF,
                                      0xFF,0xFF,0xFF,0xFF,0xFF,0x00 };

static void edc_check_sector(const uint8_t *sec, int lba)
{
	if (!edc_on) return;

	/* structure first: without a sane sync pattern + mode byte the layout
	   cannot be trusted, so no edc verdict either way */
	if (memcmp(sec, edc_sync, 12) || sec[15] > 2) {
		pcd.st_form_bad++;
		if (pcd.st_edc_logged < 8) {
			pcd.st_edc_logged++;
			printf("physcd: malformed data sector lba=%d (mode %02x)%s\n",
				lba, sec[15],
				pcd.st_edc_logged == 8 ? " (further ones counted silently)" : "");
		}
		return;
	}

	uint32_t want, got;
	if (sec[15] == 1) {
		want = edc_stored(sec + 2064);
		got = edc_compute(sec, 2064);
	}
	else if (sec[15] == 2) {
		/* only the XA forms carry an edc; the 4-byte subheader is stored
		   twice (16..19 and 20..23). disagreeing copies = either a plain
		   non-XA mode-2 sector (no edc exists) or corruption we cannot
		   attribute - count the anomaly, skip the edc claim. all our
		   mode-2 consoles (psx, cd-i) are XA, so agreement is the norm. */
		if (memcmp(sec + 16, sec + 20, 4)) {
			pcd.st_form_bad++;
			return;
		}
		if (sec[18] & 0x20) {
			/* form 2: edc optional - an all-zero field means "not computed" */
			want = edc_stored(sec + 2348);
			if (!want) return;
			got = edc_compute(sec + 16, 2332);
		} else {
			want = edc_stored(sec + 2072);
			got = edc_compute(sec + 16, 2056);
		}
	}
	else return;                      /* mode 0: zero-filled, no edc field */

	pcd.st_edc_checked++;
	if (got != want) {
		pcd.st_edc_bad++;
		pcd.st_edc_last_lba = lba;
		/* capped like the BAD logging: serial output in the hot path
		   stalls the core, so 8 lines then silent counting */
		if (pcd.st_edc_logged < 8) {
			pcd.st_edc_logged++;
			printf("physcd: edc mismatch lba=%d (misread served as-is)%s\n",
				lba, pcd.st_edc_logged == 8 ? " (further ones counted silently)" : "");
		}
	}
}

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
			/* feeds the re-attach probe: a drive that has dropped off
			   the bus fails every sector, a scratch fails a few */
			if (rr) pcd.consec_fail++; else pcd.consec_fail = 0;
			/* integrity pass (stats-only) before the lock; audio has no edc */
			if (!rr && flags == 0xF8) edc_check_sector(one, lba + i);
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

	pcd.consec_fail = 0;          /* a clean burst: the drive is there */

	int sector_len = PHYSCD_RAW + (with_sub ? PHYSCD_SUB : 0);

	/* integrity pass (stats-only) from the burst buffer, OUTSIDE the lock:
	   every physically read data sector is verified exactly once, here at
	   stamping (sync misses, prefetch, prewarm and swap-wake all funnel
	   through this path). audio tracks carry no edc; the track clamp above
	   means a burst never mixes types, so one flags test covers it. */
	if (flags == 0xF8)
		for (int i = 0; i < count; i++)
			edc_check_sector(burst + i * sector_len, lba + i);

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
		/* EDC bad = the drive returned bytes that fail the sector's own
		   checksum: a misread served AS-IS (unlike BAD, which is zeros).
		   cumulative per mount, like BAD. */
		fprintf(f, "EDC bad %u / %u checked  malformed %u  last bad lba %d\n",
			pcd.st_edc_bad, pcd.st_edc_checked, pcd.st_form_bad, pcd.st_edc_last_lba);
		/* nonzero REATTACH means the drive dropped off the usb bus and
		   was recovered - that is a power/cabling problem, not media */
		fprintf(f, "REATTACH %u  (device %s)\n", pcd.st_reattach, cur_dev);
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

static int open_drive(char *out, int outsz);   /* defined below */

/*
 * a usb drive can vanish mid-session and come back as a DIFFERENT
 * /dev/srN. observed on real hardware: heavy seeking (a level load)
 * draws peak current, the port browns out, and dmesg shows
 * "reset high-speed USB device" - sometimes a full disconnect and a
 * new device number, which is exactly how the drive moved sr0 -> sr1.
 * our fd is then stale FOREVER and every read fails, so the game gets
 * zeros with no way back short of a remount. rescan and re-open.
 *
 * prefetch thread only (it blocks), rate-limited, and the io mutex is
 * held so no read can be in flight across the swap.
 */
static int device_gone(void)
{
	if (pcd.fd < 0) return 1;
	if (ioctl(pcd.fd, CDROM_DRIVE_STATUS, CDSL_CURRENT) >= 0) return 0;
	return (errno == ENODEV || errno == ENXIO || errno == EIO || errno == ESHUTDOWN);
}

static void try_reattach(void)
{
	char newdev[64] = "";

	pthread_mutex_lock(&pcd.io);
	if (pcd.fd >= 0) { close(pcd.fd); pcd.fd = -1; }

	int fd = open_drive(newdev, sizeof(newdev));
	if (fd >= 0) {
		pcd.fd = fd;
		snprintf(cur_dev, 64, "%s", newdev);
		pcd.st_reattach++;
		pcd.consec_fail = 0;
		quiet_block_probes(cur_dev);   /* a re-enumerated device gets fresh queue defaults */
		set_speed_cap();
		printf("physcd: drive re-attached as %s (recovered from a usb reset)\n", newdev);
	}
	else {
		printf("physcd: drive still missing, will retry\n");
	}
	pthread_mutex_unlock(&pcd.io);
}

static void *prefetch_thread(void *arg)
{
	(void)arg;
	int rr = 0;                                /* round-robin start   */
	double last_stats = now_ms();
	double last_reattach = 0;
	double last_io = now_ms();

	double last_watch = 0;
	int was_present = -1;

	double last_swap_check = 0;
	int swap_was_present = -1;
	int swap_ejected = 0;

	while (pcd.running) {
		int target = -1;

		/* menu watch mode: nothing is mounted, so this thread's job is
		   to notice a disc instead of prefetching. identification runs
		   HERE because it can block for seconds - the ui cannot. */
		if (pcd.watch_mode) {
			if (now_ms() - last_watch >= 2000) {
				last_watch = now_ms();

				/* the drive can drop off the bus while we sit at the
				   menu too - the prefetch path's re-attach never runs
				   here, so check it ourselves or the fd stays stale
				   and autoboot is dead for the session */
				if (device_gone() && now_ms() - last_reattach > 5000) {
					last_reattach = now_ms();
					try_reattach();
					was_present = -1;
				}

				int present = physcd_disc_present();
				int changed = physcd_media_changed();

				if (present && (was_present != 1 || changed)) {
					/* MUST forget the previous disc first: identify
					   only loads a toc when first_data_lba < 0, so a
					   second disc in one menu session would otherwise
					   be identified from the FIRST disc's toc and its
					   stale cached sectors */
					physcd_forget_disc();

					physcd_disc_t t = physcd_identify();
					physcd_region_t r = physcd_region();

					if (t == PHYSCD_DISC_NONE) {
						/* could not read it at all - do not latch, let
						   the next poll try again (a drive still
						   spinning up reports exactly this) */
						printf("physcd: disc present but unreadable, retrying\n");
					}
					else {
						/* capture a display label now, on this thread,
						   so the menu query stays O(1) and non-blocking.
						   iso volume label first; if blank/generic, fall
						   back to the psx serial (X-Files etc have no
						   useful label). */
						char lbl[64];
						if (!physcd_disc_label(lbl, sizeof(lbl)))
							physcd_disc_serial(lbl, sizeof(lbl));
						pthread_mutex_lock(&pcd.lock);
						snprintf(pcd.watch_label, sizeof(pcd.watch_label), "%s", lbl);
						pcd.watch_type = (int)t;
						pcd.watch_present = 1;
						/* this branch only runs on a real change - a fresh
						   disc OR a swap (media changed) - so always flag
						   dirty, or a swap keeps the old row label */
						pcd.watch_dirty = 1;
						pthread_mutex_unlock(&pcd.lock);

						pcd.ev_type = (int)t;
						pcd.ev_region = (int)r;
						pcd.ev_initial = (was_present < 0) ? 1 : 0;
						pcd.ev = (int)PHYSCD_EV_DISC_IN;
						printf("physcd: disc detected: %s%s%s%s%s\n",
							physcd_disc_name(t),
							*physcd_region_name(r) ? " region " : "",
							physcd_region_name(r),
							lbl[0] ? " - " : "", lbl);
						was_present = 1;
					}
				}
				else if (!present && was_present == 1) {
					pthread_mutex_lock(&pcd.lock);
					if (pcd.watch_present) pcd.watch_dirty = 1;
					pcd.watch_present = 0;
					pcd.watch_label[0] = 0;
					pthread_mutex_unlock(&pcd.lock);

					pcd.ev = (int)PHYSCD_EV_DISC_OUT;
					printf("physcd: disc removed\n");
					was_present = 0;
				}
				else if (present) was_present = 1;
				else was_present = 0;
			}
			struct timespec ts = { 0, 50 * 1000 * 1000 };
			nanosleep(&ts, NULL);
			continue;
		}

		/* mid-mount physical disc swap (multi-disc games), armed by the core
		   for phys mounts. watch the present edge; on eject then insert,
		   reload the toc HERE - on this thread, so it serialises against
		   fill_cache, and pcd.swapping makes the consumer read path serve
		   zeros while trk[]/leadout are rewritten. then flag the core (via
		   physcd_swap_consume) to re-announce the disc without a reset. */
		/* (leadout>0 || swap_ejected): a failed reload leaves leadout at 0, so
		   keep the block armed while an eject is pending or we would never
		   retry and the read path would serve zeros forever. */
		if (pcd.swap_enable && (pcd.leadout > 0 || swap_ejected)
		    && now_ms() - last_swap_check >= SWAP_CHECK_MS) {
			last_swap_check = now_ms();
			int present = physcd_disc_present();
			if (swap_was_present == 1 && !present) {
				swap_ejected = 1;                 /* disc pulled */
				pcd.swap_ejected = 1;             /* let the core show the lid open in real time */
			}
			else if (swap_ejected && present) {
				/* new disc in and reading (present only goes true past spin-up,
				   but a first read can still fail on a marginal disc - keep
				   swap_ejected so we retry). the reload runs under pcd.io so its
				   toc ioctls cannot race a consumer fill_cache, and pcd.swapping
				   bails the read path while trk[]/leadout are rewritten. */
				toc_t scratch;
				pcd.swapping = 1;
				pthread_mutex_lock(&pcd.io);
				physcd_forget_disc();
				int ok = !physcd_load_toc(&scratch);
				pthread_mutex_unlock(&pcd.io);
				/* pcd.swapping stays SET through the wake below: the running
				   game still polls its OLD disc's lbas, and with swapping up
				   those reads return instant zeros instead of wrong-position
				   3s sync stalls against the new toc that would fight the
				   wake head-to-head. */
				if (ok) {
					/* wake the drive BEFORE the announce. a freshly inserted
					   disc spins for its firmware toc read then idles (the
					   kernel probe that kept it spinning by accident was
					   defanged in 28643ad), and the game's first read then
					   races the ~2.5s spin-up against the 3s sync timeout -
					   the flaky vib ribbon swap. SYNC fills only: they fail
					   fast and never stamp slots (the background ladder would
					   poison track 1 with permanent zero cache hits on a cold
					   NAK). loop until ONE burst genuinely succeeds - proof
					   the platter is at speed - or give up and let the gate
					   retry the whole reload. */
					double w0 = now_ms();
					int warm = 0;
					int wlba = scratch.tracks[0].start;
					while (now_ms() - w0 < 8000) {
						if (!fill_cache(wlba, SYNC_BURST, 1)) { warm = 1; break; }
					}
					if (warm) {
						/* at speed: prime a short lead (fast now), and for an
						   audio-first disc keep the lead growing until the
						   game's first read - the window prefetch yields to
						   consumers, so this cannot starve the announce. */
						for (int i = 1; i < 8; i++)
							if (fill_cache(wlba + i * SYNC_BURST, SYNC_BURST, 1)) break;
						if (pcd.trk[0].audio) { pcd.cursor[1] = wlba; pcd.wactive[1] = 1; }
						printf("physcd: disc swap - drive awake in %.0f ms\n", now_ms() - w0);
					}
					ok = warm;
				}
				pcd.swapping = 0;
				if (ok) {
					/* the menu must learn this disc was swapped in mid-game
					   (else its TYPE mismatch vs the booted game reads as a
					   fresh insert and core-exit auto-launches it). the fact
					   crosses the core-exit exec as a file, like BOOTED. */
					FILE *sf = fopen(PHYSCD_SWAPPED_MARKER, "w");
					if (sf) fclose(sf);

					/* publish the reloaded trk[]/leadout BEFORE the swap_ready
					   flag the poll thread gates on, so a weakly-ordered arm
					   core cannot observe the flag with a stale toc (matches the
					   prewarm arm; physcd_swap_consume pairs the acquire side) */
					pcd.swap_ejected = 0;
					__sync_synchronize();
					pcd.swap_ready = 1;
					swap_ejected = 0;
					printf("physcd: disc swap - new toc loaded\n");
				}
				/* load or wake failed (drive not settled / disc unreadable):
				   swap_ejected stays set, the gate above keeps retrying every
				   SWAP_CHECK_MS until the disc genuinely reads */
			}
			swap_was_present = present;
		}

		/* cold-start spin-up: on a fresh audio-first mount, aggressively read a
		   deep lead of the first audio track BEFORE the core plays, so the drive
		   reaches full read speed and the cdda cache is filled - otherwise the
		   drive's multi-second spin-up starves the first several seconds of
		   playback. stops the instant the core issues its first read (last_win
		   set) or once the lead is built, and yields to any waiting consumer. */
		if (pcd.prewarm >= 0) {
			if (pcd.last_win >= 0 || pcd.prewarm >= pcd.prewarm_end
			    || pcd.consec_fail >= 8 || pcd.leadout <= 0) {
				pcd.prewarm = -1;   /* core read, lead built, drive struggling, or toc swapping */
			} else if (!pcd.sync_pending) {
				int pw = pcd.prewarm;
				fill_cache(pw, BURST, 0);
				pcd.prewarm = pw + BURST;
				last_io = now_ms();
				continue;
			}
		}

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

		/* reads keep failing: the drive may have dropped off the bus
		   and re-enumerated elsewhere. probe and rescan, at most once
		   every 5s so a genuinely dead drive is not thrashed. */
		if (pcd.consec_fail >= 8 && now_ms() - last_reattach > 5000) {
			last_reattach = now_ms();
			if (device_gone()) try_reattach();
			else pcd.consec_fail = 0;   /* device is fine, just bad media */
		}

		if (target < 0) {
			/* nothing to fetch. if a disc is mounted and the drive has
			   been untouched for a while, poke it so it does not spin
			   down / get autosuspended - waking it is what stalls for
			   minutes. one sector, discarded, on the stream the core is
			   ACTUALLY reading (last_win) - never the data track of a
			   mixed disc an audio player is ignoring. skipped until a
			   first read tells us which stream is live. */
			int lw = pcd.last_win;
			/* before any read (mounted but nothing streamed yet - a paused cd
			   player, a slow core init, the moment after a swap) fall back to
			   the primed cursor[0] so the drive STILL gets its poke: a mount
			   that idles must never spin down into the multi-minute usb wake
			   stall. once a real read arms last_win the poke follows the actual
			   stream, so a mixed disc's data track is never chased mid-play. */
			int klba = (lw >= 0) ? pcd.cursor[lw] : (pcd.leadout > 0 ? pcd.cursor[0] : -1);
			if (klba >= pcd.leadout) klba = pcd.leadout - 1;  /* sat on leadout: poke the last real sector */
			if (pcd.leadout > 0 && klba >= 0
			    && now_ms() - last_io >= KEEPALIVE_MS && !pcd.sync_pending) {
				uint8_t sc[SLOT_SIZE];
				int t = track_of(klba);
				uint8_t fl = (t >= 0 && pcd.trk[t].audio) ? 0x10 : 0xF8;
				pthread_mutex_lock(&pcd.io);
				sg_read_cd(klba, 1, fl, 0, sc, BG_TIMEOUT_MS);
				pthread_mutex_unlock(&pcd.io);
				last_io = now_ms();
			}

			/* both windows full (or no toc yet): check again soon */
			struct timespec ts = { 0, 20 * 1000 * 1000 };
			nanosleep(&ts, NULL);
			continue;
		}
		fill_cache(target, BURST, 0);
		last_io = now_ms();
	}
	return NULL;
}

// ---------------------------------------------------------------- api


void physcd_set_device(const char *dev)
{
	if (dev && *dev) snprintf(pref_dev, sizeof(pref_dev), "%s", dev);
	else pref_dev[0] = 0;
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
		int fd = open(pref_dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0) { snprintf(out, outsz, "%s", pref_dev); return fd; }
		printf("physcd: %s not available (%s), scanning\n", pref_dev, strerror(errno));
	}

	for (int i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "/dev/sr%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
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

	quiet_block_probes(cur_dev);
	set_speed_cap();
	edc_init();                   /* one-shot lut build + self-test */

	pcd.leadout = 0;
	pcd.ntrk = 0;
	pcd.sub_ok = -1;
	for (int w = 0; w < NWIN; w++) { pcd.cursor[w] = 0; pcd.wactive[w] = 0; }
	pcd.last_win = -1;
	pcd.prewarm = -1;
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

// true while physcd holds the drive (watching at the menu, or a game disc
// mounted). the acoustic seek feature uses this to stay off the drive
// whenever physcd wants it.
int physcd_drive_busy()
{
	return pcd.fd >= 0;
}

int physcd_swap_ejected(void)
{
	/* mid-swap window: the disc is physically out (or back in but its toc not
	   loaded yet). cores use this to show the guest a REAL-TIME lid-open - the
	   lid lifts when the user ejects, not a compressed pulse after the fact. */
	return pcd.swap_enable && pcd.swap_ejected;
}

// did a physical swap complete during the last mount session? read-and-clear;
// the menu's autoboot consults it on core exit so a disc the user swapped in
// MID-GAME (vib ribbon's music cd) is treated as already-played, not as a
// fresh insert to auto-launch. FILE-backed because the menu runs in a fresh
// process (core exit is an exec); unlink doubles as an atomic read-and-clear.
int physcd_swap_happened(void)
{
	return unlink(PHYSCD_SWAPPED_MARKER) == 0;
}

// arm/disarm mid-mount disc-swap detection. the core calls this on a physical
// mount so the prefetch thread watches for an eject-then-insert.
void physcd_swap_enable(int enable)
{
	pcd.swap_enable = enable ? 1 : 0;
	if (enable) unlink(PHYSCD_SWAPPED_MARKER);  /* a new mount session starts clean */
	if (!enable) pcd.swap_ejected = 0;
	if (!enable) pcd.swap_ready = 0;
}

// 1 once after a physical disc swap has been detected and the new toc loaded,
// so the core can re-announce the disc. O(1) read-and-clear, safe from the
// core poll thread.
int physcd_swap_consume(void)
{
	int r = pcd.swap_ready;
	pcd.swap_ready = 0;
	/* acquire: pair the prefetch thread's release fence so a caller that sees
	   the flag also sees the reloaded trk[]/leadout before it reads them via
	   physcd_current_toc (all swap consumers - psx + the cdd cores) */
	if (r) __sync_synchronize();
	return r;
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

	set_speed_cap();      /* a media change resets it on many drives */

	pcd.sub_ok = -1;
	probe_subchannel(toc->tracks[0].start + 16);

	/* prime cursor[0] at track 1 but DO NOT arm the data window: a window
	   activates only when the core actually reads from it (read_sector_impl).
	   an audio-cd player reads only audio, so on a mixed-mode (cd-extra) disc
	   the head is never pulled to the data track it will never ask for. a game
	   activates window 0 on its first boot read, at the cursor already primed
	   here. */
	for (int w = 0; w < NWIN; w++) { pcd.cursor[w] = 0; pcd.wactive[w] = 0; }
	pcd.cursor[0] = toc->tracks[0].start;
	pcd.last_win = -1;
	pcd.st_hit = pcd.st_miss = pcd.st_bad = pcd.st_bad_logged = 0;
	pcd.st_edc_checked = pcd.st_edc_bad = pcd.st_form_bad = pcd.st_edc_logged = 0;
	pcd.st_edc_last_lba = -1;
	pcd.st_worst_ms = pcd.st_worst_io_ms = 0.0;

	pcd.leadout = lead.cdte_addr.lba;         /* unblocks prefetch    */

	/* cold-start spin-up pre-warm, audio-first discs only (a pure audio cd or
	   a cd-extra album, whose track 1 is audio). such a disc plays as audio
	   the moment the core's cd player opens, but a cold drive's spin-up takes
	   SECONDS - the cdda prefetch cannot out-read a not-yet-at-speed drive, so
	   the first ~6s stutter ("durr durr") until it ramps. arm the cdda window
	   and queue a deep read of the first audio track so the prefetch thread
	   spins the drive up AND fills the cache before playback. the psx read path
	   subtracts the 150 pregap, so tracks[0].start is the exact lba the core
	   will ask for. games (data-first) skip this and stay lazy; a slow
	   multisession data track is never pre-warmed.
	   NOT on a mid-game swap (pcd.swapping): physcd_load_toc is shared between a
	   fresh mount and the swap reload, and a running game reads the new disc
	   the instant it is announced - a pre-warm firing then seizes the drive and
	   pcd.io during exactly that read and starves it (Vib Ribbon swapping in a
	   music cd bounced to its menu / never registered a usable disc). the
	   pre-warm is only for a fresh audio-player boot, where the drive warms
	   during the bios boot before anything reads. */
	pcd.prewarm = -1;
	if (!pcd.swapping && pcd.ntrk && pcd.trk[0].audio) {
		int pw_end = toc->tracks[0].start + PREWARM_SECTORS;
		if (pw_end > pcd.trk[0].end) pw_end = pcd.trk[0].end;  /* stay inside track 1 */
		if (pw_end > pcd.leadout)    pw_end = pcd.leadout;
		/* publish the payload (window + bound) BEFORE the arm flag, mirroring
		   how leadout is written last to unblock the prefetch: the prefetch
		   thread gates on pcd.prewarm, so a store fence keeps it from seeing
		   the flag armed while prewarm_end is still stale (which would abort
		   the pre-warm and silently give back the cold-start stutter). */
		pcd.cursor[1] = toc->tracks[0].start;
		pcd.wactive[1] = 1;
		pcd.prewarm_end = pw_end;
		__sync_synchronize();
		pcd.prewarm = toc->tracks[0].start;
	}

	printf("\x1b[32mphyscd: toc loaded, %d tracks, leadout %d, subchannel %s\n\x1b[0m",
		n, pcd.leadout,
		pcd.sub_ok == 1 ? "yes" : pcd.sub_ok == 0 ? "no" : "unknown");
	return 0;
}

/* fill a toc_t from the CURRENTLY mounted disc without touching the drive
   or the cache - unlike physcd_load_toc, which re-reads the toc, resets
   the cache and re-probes subchannel. for callers that need the toc
   mid-session (the RA hash reader), safe to call while a disc is playing.
   -1 if nothing is mounted. */
int physcd_current_toc(toc_t *toc)
{
	// pcd.swapping: a reload is rewriting trk[]/leadout, do not copy mid-flight
	if (!toc || pcd.fd < 0 || pcd.swapping || pcd.ntrk < 1 || pcd.leadout <= 0) return -1;

	memset(toc, 0, sizeof(toc_t));
	for (int i = 0; i < pcd.ntrk; i++) {
		cd_track_t *trk = &toc->tracks[i];
		trk->start = pcd.trk[i].start;
		trk->end = pcd.trk[i].end;
		trk->type = pcd.trk[i].audio ? TT_CDDA : TT_MODE1;
		trk->sector_size = PHYSCD_RAW;   /* we always read raw */
		trk->offset = 0;
		trk->index_num = 2;
		trk->indexes[0] = 0;
		trk->indexes[1] = 0;
	}
	toc->last = pcd.ntrk;
	toc->end = pcd.leadout;
	toc->sectorSize = PHYSCD_RAW;
	toc->phys = 1;
	return 0;
}

/* spin the platter up + prime the disc start before a core that reads the disc
 * the instant it mounts (cd-i's bios does). Blocks the mount path, which runs
 * BEFORE the core is told a disc is present (no fpga read waits on us); on a
 * warm drive it returns at once.
 *
 * NB this is a mitigation, not the cold-load fix. The real cause of the cd-i
 * cold-load choppiness was udev's blkid builtin superblock-probing the disc
 * START (blocks 0/8/16/24/56/128) from the BOOT coldplug event, grinding 1-3s
 * per failed cooked read against a mode-2 disc for up to udev's 180s event
 * timeout, seesawing the head against the game's deep reads. That is fixed by
 * the rule physcd_install_udev_rule() ships (see it for the full story) -
 * pausing the exec queue at mount was falsified on hardware (an in-flight
 * worker survives the pause). */
void physcd_prewarm_blocking(void)
{
	if (pcd.fd < 0 || pcd.ntrk < 1 || pcd.leadout <= 0) return;

	int wlba = pcd.trk[0].start;
	double w0 = now_ms();
	int warm = 0;

	/* SYNC fills only - they never stamp slots on a cold NAK. loop until one
	   burst genuinely completes: on a drive that NAKs while spinning up that
	   means "platter now at speed", and on one that just reads slowly the
	   first (slow) burst is itself the spin-up. */
	while (now_ms() - w0 < 8000) {
		if (!fill_cache(wlba, SYNC_BURST, 1)) { warm = 1; break; }
	}
	if (warm) {
		/* SEEK-CALIBRATION pass. a spinning platter is only half of warm: a
		   cold drive's first LONG-THROW seeks run ~270ms+ (servo/sled
		   settling), and the cd-i core grants exactly 250ms of simulated
		   seek time (its RTL kSeekTime) before it starts consuming - so the
		   first cutscene's segment seeks each lose that race by ~20ms, the
		   flushed 27-sector core fifo comes up empty, and the CDIC replays
		   the last audio buffer = the fmv "doubled voices" (measured: 18
		   misses, worst 271ms, on an otherwise perfect cold boot; the same
		   seeks on a settled drive run tens of ms). exercise the sled
		   across the disc NOW, before the core is told a disc exists, so
		   its first real seeks land inside the grace window. sync fills
		   only (they never stamp slots on failure); a failed burst still
		   moved the head, which is the point. */
		int span = pcd.leadout - wlba;
		if (span > 8 * SYNC_BURST) {
			static const int quarters[] = { 2, 3, 1 };   /* mid, outer, back in */
			for (unsigned i = 0; i < sizeof(quarters) / sizeof(quarters[0]); i++) {
				int slba = wlba + (int)(((int64_t)span * quarters[i]) / 4);
				if (slba + SYNC_BURST > pcd.leadout) slba = pcd.leadout - SYNC_BURST;
				fill_cache(slba, SYNC_BURST, 1);
			}
		}

		/* prime a short lead at the start LAST, so any direct-mapped window
		   collisions from the seek pass are overwritten and the lead is
		   intact; point the data cursor here so the prefetch thread keeps
		   pulling ahead from the disc start. */
		for (int i = 1; i < 8; i++)
			if (fill_cache(wlba + i * SYNC_BURST, SYNC_BURST, 1)) break;
		pcd.cursor[0] = wlba;
		pcd.wactive[0] = 1;
		printf("physcd: drive spun up + seek-warmed + primed in %.0f ms\n", now_ms() - w0);
	}
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

	if (pcd.fd < 0 || pcd.swapping || lba < 0 || lba >= pcd.leadout) {
		/* pcd.swapping: a disc swap is reloading the toc on the prefetch
		   thread, so bail here before touching pcd.trk[]/leadout while they
		   are being rewritten. always leave dst defined: megacd's ReadCDDA
		   ignores the return value and would otherwise ship an uninitialized
		   stack buffer to the fpga */
		memset(dst, 0, PHYSCD_RAW);
		if (sub96) memset(sub96, 0, PHYSCD_SUB);
		return -1;
	}

	int w = win_of(lba);
	pcd.wactive[w] = 1;
	pcd.last_win = w;        /* the stream the core is actually reading */

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

/* cd+g probe for an audio-only disc. cd+g karaoke graphics ride in the
   R-W subchannel: the 96 raw subchannel bytes per sector carry 96
   six-bit symbols (low 6 bits of each byte) forming four 24-symbol
   packs, and a pack whose command symbol is 9 ("TV graphics") is cd+g.
   the subchannel comes off the disc raw and uncorrected, so a stray
   bit flip can fake a lone 9 - require a pile of hits before believing
   it. */
static int probe_cdg(void)
{
	/* the toc-time subchannel probe can come back inconclusive (-1,
	   "couldn't tell, retry later") and nothing else ever retries it -
	   without this a cd+g disc identified in that state is stuck as
	   plain AUDIO for the whole insertion. one more try, aimed at the
	   audio region we are about to sample anyway. */
	if (pcd.sub_ok < 0 && pcd.ntrk > 0 && pcd.trk[0].audio)
		probe_subchannel(pcd.trk[0].start + 3 * 75);

	if (pcd.sub_ok != 1) return 0;     /* drive gives no subchannel */

	uint8_t raw[PHYSCD_RAW], sub[PHYSCD_SUB];
	int hits = 0;

	/* sample windows spread across the WHOLE program area, not the head
	   of the first tracks: discs put packs where the lyrics are, not at
	   track starts (hardware showed a karaoke disc with track 1 empty
	   everywhere and track 2 empty until a third of the way in). early
	   exit once the disc has proven itself. */
	enum { CDG_WINDOWS = 10, CDG_WSEC = 20, CDG_NEED = 6 };
	int lo = pcd.trk[0].start + 150;
	int hi = pcd.leadout - 150;
	if (hi <= lo) return 0;

	for (int w = 0; w < CDG_WINDOWS && hits < CDG_NEED; w++) {
		int base = lo + (int)(((int64_t)(hi - lo) * w) / CDG_WINDOWS);
		for (int s = 0; s < CDG_WSEC && base + s < hi; s++) {
			if (!physcd_read_sector_sub(base + s, raw, sub)) continue;
			for (int p = 0; p < PHYSCD_SUB; p += 24)
				if ((sub[p] & 0x3F) == 9) hits++;
		}
	}

	if (hits >= CDG_NEED)
		printf("\x1b[32mphyscd: CD+G disc (%d graphics packs sampled)\n\x1b[0m", hits);
	return hits >= CDG_NEED;
}

physcd_disc_t physcd_identify()
{
	uint8_t raw[PHYSCD_RAW * 2];

	if (!physcd_disc_present()) return PHYSCD_DISC_NONE;
	if (pcd.first_data_lba < 0) {
		/* toc not loaded yet or audio-only disc                  */
		toc_t tmp;
		if (physcd_load_toc(&tmp)) return PHYSCD_DISC_NONE;
		if (pcd.first_data_lba < 0)
			return probe_cdg() ? PHYSCD_DISC_CDG : PHYSCD_DISC_AUDIO;
	}

	int base = pcd.first_data_lba;

	if (!physcd_read_sector(base, raw, NULL)) {
		if (!memcmp(raw + 16, "SEGADISCSYSTEM", 14)) return PHYSCD_DISC_MEGACD;
		if (!memcmp(raw + 16, "SEGA SEGASATURN", 15)) return PHYSCD_DISC_SATURN;
		/* 3do volume header: record type 0x01 then five 0x5A sync bytes.
		   not iso9660, so this is the only marker. */
		if (raw[16] == 0x01 && raw[17] == 0x5A && raw[18] == 0x5A
			&& raw[19] == 0x5A && raw[20] == 0x5A && raw[21] == 0x5A)
			return PHYSCD_DISC_3DO;
	}

	if (!physcd_read_sector(base + 16, raw, NULL)) {
		uint8_t *iso = raw + 16;
		if (memcmp(iso + 1, "CD001", 5)) iso = raw + 24;   /* mode2 form1 */
		/* cd-i (green book) uses the iso9660 volume-descriptor slot but with
		   the "CD-I " standard identifier; mutually exclusive with CD001, so
		   this never trips over the six CD001 console signatures below. */
		if (!memcmp(iso + 1, "CD-I ", 5)) return PHYSCD_DISC_CDI;
		if (!memcmp(iso + 1, "CD001", 5)) {
			if (!memcmp(iso + 8, "PLAYSTATION", 11)) return PHYSCD_DISC_PSX;
			if (!memcmp(iso + 8, "NGCD", 4)) return PHYSCD_DISC_NEOGEO;
			/* video cd / super vcd / photo cd / cd-i digital video are all
			   CD-BRIDGE discs (white book): a "CD001" iso volume whose SYSTEM
			   identifier (same field psx stamps "PLAYSTATION" in) is
			   "CD-RTOS CD-BRIDGE", and which carry a /CDI application on the
			   disc that a cd-i player auto-runs. so boot the cd-i core and let
			   the disc's own app drive it. whether it actually plays depends on
			   the core emulating the cd-i digital-video (mpeg) hardware -
			   experimental. cd-i GAME discs use the "CD-I " standard id caught
			   above, so this never collides with them. */
			if (!memcmp(iso + 8, "CD-RTOS CD-BRIDGE", 17)) return PHYSCD_DISC_VCD;
		}
	}

	/* neogeo cd: the boot file IPL.TXT is always in the root directory.
	   the PVD system-id "NGCD" check above is not reliable across all
	   discs, so scan the directory region for the filename as a robust
	   fallback. */
	for (int s = 16; s <= 40; s++) {
		uint8_t user[2048];
		if (physcd_read_data2048(base + s, user)) continue;
		if (memmem(user, sizeof(user), "IPL.TXT", 7)) return PHYSCD_DISC_NEOGEO;
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

/*
 * a human-readable name for the menu. the iso9660 volume label is the
 * one name field present on every data disc regardless of console -
 * "SONIC_CD", "FF8_DISK1" etc - so use that. d-characters forbid
 * spaces, so labels conventionally use '_' for them: convert back for
 * display. returns strlen written, 0 when there is no usable label.
 */
int physcd_disc_label(char *out, int outsz)
{
	uint8_t user[2048];

	if (!out || outsz < 2) return 0;
	out[0] = 0;

	if (!physcd_disc_present()) return 0;
	if (pcd.first_data_lba < 0) {
		toc_t tmp;
		if (physcd_load_toc(&tmp)) return 0;
		if (pcd.first_data_lba < 0) return 0;   /* audio cd, no label */
	}

	/* iso9660 primary volume descriptor lives at logical sector 16 */
	if (physcd_read_data2048(pcd.first_data_lba + 16, user)) return 0;
	if (user[0] != 1 || memcmp(user + 1, "CD001", 5)) return 0;

	/* volume identifier: 32 bytes at offset 40, space-padded */
	char lbl[33];
	memcpy(lbl, user + 40, 32);
	lbl[32] = 0;

	int end = 32;
	while (end > 0 && (lbl[end - 1] == ' ' || lbl[end - 1] == 0)) end--;
	lbl[end] = 0;

	for (int i = 0; i < end; i++) {
		if (lbl[i] == '_') lbl[i] = ' ';
		/* drop anything non-printable so a mangled label can't corrupt
		   the osd row */
		else if (lbl[i] < 0x20 || (uint8_t)lbl[i] > 0x7E) lbl[i] = ' ';
	}

	/* "PLAYSTATION" is the generic volume id most psx discs carry - it
	   is the console, not the game, so treat it as no useful name and
	   let the caller fall back to the serial */
	if (!strcasecmp(lbl, "PLAYSTATION")) return 0;

	snprintf(out, outsz, "%s", lbl);
	return strlen(out);
}

/*
 * psx game serial (SLES-01234 etc) as a name fallback: psx discs
 * frequently have a blank or generic iso label, but the boot
 * executable is named after the serial and appears as a root-directory
 * filename "SLES_012.34;1". scan the directory region for it. this is
 * exactly what psx_get_game_info does, replicated here so it works at
 * menu-watch time with no core loaded. returns strlen, 0 if none.
 * BLOCKING (reads the disc) - call only from the watcher thread.
 */
int physcd_disc_serial(char *out, int outsz)
{
	static const char *pfx[] = {
		"SCES","SLES","SCUS","SLUS","SCPS","SLPS","SLPM","SCPM",
		"SIPS","SCED","SLED","SCZS","PAPX","PCPX","PEPX","PUPX"
	};
	if (!out || outsz < 2) return 0;
	out[0] = 0;
	if (pcd.first_data_lba < 0) return 0;

	/* iso root directory sits a few sectors past the PVD; scan a window
	   that covers the usual layouts */
	for (int s = 16; s <= 64; s++) {
		uint8_t user[2048];
		if (physcd_read_data2048(pcd.first_data_lba + s, user)) continue;

		for (int p = 0; p < (int)(sizeof(pfx) / sizeof(pfx[0])); p++) {
			uint8_t *m = (uint8_t *)memmem(user, sizeof(user), pfx[p], 4);
			if (!m) continue;

			char *start = (char *)m;
			char *semi = (char *)memmem(start, sizeof(user) - (start - (char *)user), ";", 1);
			if (!semi) continue;
			int len = (int)(semi - start);
			if (len < 8 || len > 11) continue;   /* SLES_012.34 = 11 */

			char id[16];
			memcpy(id, start, len);
			id[len] = 0;
			if (id[4] == '_') id[4] = '-';        /* SLES_ -> SLES- */
			char *dot = strchr(id, '.');          /* drop the dot   */
			if (dot) memmove(dot, dot + 1, strlen(dot));
			snprintf(out, outsz, "%s", id);
			return (int)strlen(out);
		}
	}
	return 0;
}

/* friendly console name for the menu row ("Mega CD", not "MegaCD") */
const char *physcd_console_name(physcd_disc_t t)
{
	switch (t) {
	case PHYSCD_DISC_MEGACD: return "Mega CD";
	case PHYSCD_DISC_SATURN: return "Saturn";
	case PHYSCD_DISC_PSX:    return "PlayStation";
	case PHYSCD_DISC_PCECD:  return "TurboGrafx-CD";
	case PHYSCD_DISC_NEOGEO: return "Neo Geo CD";
	case PHYSCD_DISC_3DO:    return "3DO";
	case PHYSCD_DISC_CDI:    return "CD-i";
	case PHYSCD_DISC_VCD:    return "Video CD";
	case PHYSCD_DISC_CDG:    return "CD+G";
	default:                 return physcd_disc_name(t);
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
	case PHYSCD_DISC_3DO:    return "3DO";
	case PHYSCD_DISC_CDI:    return "CD-i";
	case PHYSCD_DISC_VCD:    return "Video CD";
	case PHYSCD_DISC_CDG:    return "CD+G";
	case PHYSCD_DISC_AUDIO:  return "Audio CD";
	case PHYSCD_DISC_NONE:   return "No Disc";
	default:                 return "Unknown";
	}
}

int physcd_watch_start(void)
{
	/* the menu is about to open the drive to watch for a disc, so make the
	   acoustic seek let go of it first (it only runs for image games) */
	physcd_acoustic_pause();
	if (physcd_open(NULL)) return -1;
	pcd.ev = (int)PHYSCD_EV_NONE;
	pcd.watch_mode = 1;
	printf("physcd: watching %s for a disc\n", cur_dev);
	return 0;
}

void physcd_watch_stop(void)
{
	pcd.watch_mode = 0;
	pcd.ev = (int)PHYSCD_EV_NONE;

	/* clear the cached snapshot so a stop/restart that straddles a disc
	   removal cannot leave watch_present stale at 1 */
	pthread_mutex_lock(&pcd.lock);
	pcd.watch_present = 0;
	pcd.watch_type = 0;
	pcd.watch_label[0] = 0;
	pcd.watch_dirty = 1;
	pthread_mutex_unlock(&pcd.lock);

	physcd_close();

	/* the drive is free again: a core is loading, so let acoustic have it for
	   image-backed play (harmless if the core turns out to mount a real disc,
	   physcd_drive_busy keeps acoustic off the drive in that case) */
	physcd_acoustic_resume();
}

int physcd_watching(void)
{
	return pcd.watch_mode;
}

/* O(1) snapshot for the menu row - reads cached watcher state, never
   touches the drive, safe from the ui thread. returns 1 if a disc is
   present, filling type and a display name (label if we got one, else
   the console name). */
int physcd_menu_status(char *name, int namesz, physcd_disc_t *type)
{
	pthread_mutex_lock(&pcd.lock);
	int present = pcd.watch_present;
	int t = pcd.watch_type;
	/* raw label, EMPTY when the disc has none - the caller decides the
	   fallback. do NOT substitute the console name here, or a disc with
	   no title renders "Play: Saturn - Saturn". */
	if (name && namesz > 0) snprintf(name, namesz, "%s", pcd.watch_label);
	pthread_mutex_unlock(&pcd.lock);

	if (type) *type = (physcd_disc_t)t;
	return present;
}

/* edge-triggered: 1 once after the disc presence changed, so the menu
   can rebuild the core list to add/remove the Play Disc row. locked so
   an edge the watcher sets between our read and clear is not lost. */
int physcd_menu_dirty(void)
{
	pthread_mutex_lock(&pcd.lock);
	int d = pcd.watch_dirty;
	pcd.watch_dirty = 0;
	pthread_mutex_unlock(&pcd.lock);
	return d;
}

physcd_event_t physcd_poll_event(physcd_disc_t *type, physcd_region_t *region, int *initial)
{
	physcd_event_t e = (physcd_event_t)pcd.ev;
	if (e == PHYSCD_EV_NONE) return e;

	/* read the payload BEFORE clearing ev: the watcher only ever
	   publishes ev last, so this ordering cannot see a torn event */
	if (type) *type = (physcd_disc_t)pcd.ev_type;
	if (region) *region = (physcd_region_t)pcd.ev_region;
	if (initial) *initial = pcd.ev_initial;
	pcd.ev = (int)PHYSCD_EV_NONE;
	return e;
}

/* drop everything we know about the disc in the drive, so the next
   identify re-reads the toc instead of trusting the last one */
void physcd_forget_disc(void)
{
	pcd.first_data_lba = -1;
	pcd.leadout = 0;
	pcd.ntrk = 0;
	if (pcd.cache) {
		pthread_mutex_lock(&pcd.lock);
		for (int i = 0; i < CACHE_SECTORS; i++) pcd.cache[i].lba = -1;
		pthread_mutex_unlock(&pcd.lock);
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
	pcd.uncap = 0;        /* the speed uncap is per-mount opt-in, never inherited */
	cur_dev[0] = 0;
}
