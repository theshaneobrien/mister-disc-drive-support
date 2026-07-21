/*
 * physcd_acoustic.cpp - acoustic seek. see physcd_acoustic.h.
 *
 * we already know the lba an image-backed game is reading (the chd read path
 * hands it to physcd_acoustic_hint), so mirror those positions onto a real
 * usb drive holding a throwaway "prop" disc. the drive physically spins and
 * seeks in step with the game.
 *
 *  - the hint runs on the fpga-answering thread, so it does the bare minimum:
 *    stash the lba, bump an activity counter. no ioctls, no allocation, no
 *    blocking. everything expensive is on the background thread.
 *  - the background thread issues actual READs (READ(10), opcode 0x28) toward
 *    the latest hinted lba. a SEEK only slews the head - the SPINDLE never
 *    loads - so it just clicks; a READ forces the disc up to speed and holds
 *    it, which is the whirr a real drive makes. richness comes from two
 *    things: the game lba is mapped across the prop disc's whole readable
 *    radius (position-dependent pitch), and the read burst size + cadence
 *    track how hard the game is hitting the disc (loud under load, gentle on
 *    a trickle). big jumps fire an immediate read at the new radius.
 *  - when the game goes quiet it stops reading (coast), and after a few
 *    seconds forces a full spindown so the next load has an audible spin-up.
 *  - SEEK(10) 0x2B is the graceful-degrade tier: if a bridge rejects READ we
 *    fall back to the original seek-only clicking rather than going silent.
 *  - the prop disc should be a DATA disc (mode1/2048) so READs land; for a
 *    seek-only fallback any disc with a toc works. a BLANK disc will not.
 *  - a physcd physical-game mount owns the same drive, so acoustic pauses at
 *    the menu (where physcd watches) and never opens the drive while physcd
 *    holds it.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <climits>        // INT_MAX, used by CDSL_CURRENT
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/cdrom.h>
#include <scsi/sg.h>

#include "mister_physcd.h"        // physcd_drive_busy
#include "physcd_acoustic.h"

#define ACTIVE_MS       600       // no hints this long -> stop reading (coast)
#define DEEP_IDLE_MS    4000      // idle this long -> full spindown (audible spin-up next)
#define JUMP_THRESHOLD  90        // mapped-lba delta that counts as a real reposition
#define KEEPALIVE_MS    400       // SEEK-fallback tier: re-seek at least this often
#define SAMPLE_MS       200       // how often the intensity sampler updates
#define MIN_BURST       2         // sectors per read at a trickle
#define MAX_BURST       32        // sectors per read under heavy load (64 KB)
#define READ_GUARD      32        // stay this far below leadout
#define READ_GIVEUP     6         // consecutive READ rejections -> seek-only fallback
#define REOPEN_GIVEUP   8         // reopens with no successful op -> disable for the session
#define GAME_LBA_MAX    360000    // game address space mapped across the prop stroke
#define SPEED_MIN       2         // light/idle target speed (x) - the audible floor
#define SPEED_MAX       12        // heavy-load target speed (x) - the audible ceiling
#define SPEED_STEP      2         // only step the speed in units this big
#define SPEED_DEBOUNCE_MS 300     // min gap between speed changes so the motor is not flapped
#define SEEK_TIMEOUT_MS 4000
#define READ_TIMEOUT_MS 4000

static struct {
	volatile int enabled;
	volatile int paused;
	volatile int running;
	volatile int unsupported;      // drive rejected SEEK; give up for this session
	volatile int read_unsupported; // drive rejected READ; fall back to seek-only
	volatile int target_lba;
	volatile unsigned seq;         // bumped by every hint; the thread watches it for activity
	int fd;                        // thread-owned: only the acoustic thread touches it
	int prop_max;                  // leadout lba of the prop disc
	int read_lo, read_hi;          // readable window on the prop disc
	pthread_t thread;
} ac = { 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0 };

// read sink - the data is thrown away, we only want the transfer. touched
// ONLY by the acoustic thread, so it never shares state with the hint path.
static uint8_t rbuf[MAX_BURST * 2048];

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// SCSI SEEK(10): move the head to lba, transfer nothing. the fallback tier -
// works on any disc with a toc, but only clicks the head.
static int sg_seek(int fd, int lba)
{
	uint8_t cdb[10] = { 0 };
	uint8_t sense[32];
	struct sg_io_hdr io;

	cdb[0] = 0x2B;                 // SEEK(10)
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;

	memset(&io, 0, sizeof(io));
	io.interface_id = 'S';
	io.cmd_len = 10;
	io.cmdp = cdb;
	io.dxfer_direction = SG_DXFER_NONE;
	io.sbp = sense;
	io.mx_sb_len = sizeof(sense);
	io.timeout = SEEK_TIMEOUT_MS;

	if (ioctl(fd, SG_IO, &io) < 0) return -1;
	if (io.status || io.host_status || io.driver_status) return -2;
	return 0;
}

// SCSI READ(10): read `blocks` 2048-byte sectors from lba into the sink. this
// is the primary sound source - it spins the disc up and holds it loaded.
// 0 ok, -1 ioctl failed (bus dropped), -2 CHECK CONDITION (bad sector / no
// READ support).
static int sg_read10(int fd, int lba, int blocks)
{
	uint8_t cdb[10] = { 0 };
	uint8_t sense[32];
	struct sg_io_hdr io;

	cdb[0] = 0x28;                 // READ(10)
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[7] = (blocks >> 8) & 0xFF;
	cdb[8] = blocks & 0xFF;

	memset(&io, 0, sizeof(io));
	io.interface_id = 'S';
	io.cmd_len = 10;
	io.cmdp = cdb;
	io.dxfer_direction = SG_DXFER_FROM_DEV;
	io.dxfer_len = blocks * 2048;
	io.dxferp = rbuf;
	io.sbp = sense;
	io.mx_sb_len = sizeof(sense);
	io.timeout = READ_TIMEOUT_MS;

	if (ioctl(fd, SG_IO, &io) < 0) return -1;
	if (io.status || io.host_status || io.driver_status) return -2;
	return 0;
}

// SCSI START STOP UNIT: force a full spindown (LoEj=0 so it never ejects),
// IMMED so it returns without stalling the loop. best effort, result ignored.
static void sg_start_stop(int fd, int start)
{
	uint8_t cdb[6] = { 0 };
	uint8_t sense[32];
	struct sg_io_hdr io;

	cdb[0] = 0x1B;                 // START STOP UNIT
	cdb[1] = 0x01;                 // IMMED
	cdb[4] = start ? 0x01 : 0x00;  // start bit; LoEj stays 0

	memset(&io, 0, sizeof(io));
	io.interface_id = 'S';
	io.cmd_len = 6;
	io.cmdp = cdb;
	io.dxfer_direction = SG_DXFER_NONE;
	io.sbp = sense;
	io.mx_sb_len = sizeof(sense);
	io.timeout = SEEK_TIMEOUT_MS;

	ioctl(fd, SG_IO, &io);
}

// change the drive's target speed (CDROM_SELECT_SPEED, the same ioctl physcd
// uses for its cap). the motor ramps toward the new speed, which is the spin
// up/down you actually hear. best effort - a drive that ignores it just holds
// a steady spin, i.e. the previous behaviour.
static void set_speed(int fd, int nx)
{
	ioctl(fd, CDROM_SELECT_SPEED, nx);
}

// scale the game lba onto the prop disc's readable window, so every burst
// lands on burned sectors and different game positions ride different radii
// (position-dependent pitch on a CLV drive). monotonic - no modulo wrap.
static int map_lba(int game_lba)
{
	int lo = ac.read_lo, hi = ac.read_hi;
	if (hi <= lo) return lo;
	if (game_lba < 0) game_lba = 0;
	if (game_lba > GAME_LBA_MAX) game_lba = GAME_LBA_MAX;
	int p = lo + (int)((int64_t)game_lba * (hi - lo) / GAME_LBA_MAX);
	if (p < lo) p = lo;
	if (p > hi) p = hi;
	return p;
}

// open the first /dev/srN that has a disc and read its leadout. refuses to
// touch the drive if physcd (a physical game mount) already holds it.
// O_CLOEXEC so a core load (which execs) never leaks the fd.
static int open_prop(void)
{
	if (physcd_drive_busy()) return -1;

	for (int i = 0; i < 8; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/sr%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) continue;

		if (ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT) != CDS_DISC_OK) { close(fd); continue; }

		struct cdrom_tochdr hdr;
		struct cdrom_tocentry e;
		if (ioctl(fd, CDROMREADTOCHDR, &hdr) < 0) { close(fd); continue; }
		memset(&e, 0, sizeof(e));
		e.cdte_track = CDROM_LEADOUT;
		e.cdte_format = CDROM_LBA;
		if (ioctl(fd, CDROMREADTOCENTRY, &e) < 0) { close(fd); continue; }

		ac.prop_max = e.cdte_addr.lba;
		// leave room so even a max burst starting at read_hi ends before leadout
		ac.read_lo = 0;
		ac.read_hi = ac.prop_max - MAX_BURST - READ_GUARD;
		if (ac.read_hi < ac.read_lo) ac.read_hi = ac.read_lo;
		ac.read_unsupported = 0;   // re-probe reads on each fresh disc
		ac.fd = fd;
		// only log on a genuine disc change, not on every silent reopen
		static int last_logged_max = -1;
		if (ac.prop_max != last_logged_max) {
			printf("physcd_acoustic: prop disc on %s, %d sectors\n", path, ac.prop_max);
			last_logged_max = ac.prop_max;
		}
		return 0;
	}
	return -1;
}

static void close_prop(void)
{
	if (ac.fd >= 0) { close(ac.fd); ac.fd = -1; }
	ac.prop_max = 0;
}

static void *acoustic_thread(void *arg)
{
	(void)arg;
	unsigned last_seq = 0, seq_prev = 0;
	double last_active = 0, last_open_try = 0, last_read = 0;
	double t_sample = 0, rate = 0;
	int last_pos = -1000000;         // far below any real lba, forces the first op
	int seek_fails = 0, read_fails = 0, reopen_fails = 0;
	int idle_stopped = 0;
	int cur_speed = 0;               // last speed we set (0 = none yet)
	double last_speed_ms = 0;

	while (ac.running) {
		// paused (menu owns the drive), disabled, or the drive rejected even
		// SEEK: let go and idle
		if (!ac.enabled || ac.paused || ac.unsupported) {
			close_prop();
			struct timespec ts = { 0, 150 * 1000 * 1000 };
			nanosleep(&ts, NULL);
			continue;
		}

		unsigned seq = ac.seq;
		double now = now_ms();
		if (seq != last_seq) { last_seq = seq; last_active = now; idle_stopped = 0; }

		// idle: at ACTIVE_MS stop reading and coast; at DEEP_IDLE_MS force one
		// full spindown so the next load has an audible spin-up.
		if (now - last_active >= ACTIVE_MS) {
			if (!idle_stopped && ac.fd >= 0 && !ac.read_unsupported
			    && now - last_active >= DEEP_IDLE_MS) {
				sg_start_stop(ac.fd, 0);
				idle_stopped = 1;
				cur_speed = 0;   // next active period re-ramps from the floor
			}
			struct timespec ts = { 0, 80 * 1000 * 1000 };
			nanosleep(&ts, NULL);
			continue;
		}

		// physcd grabbed the drive (a physical mount): back off entirely.
		if (physcd_drive_busy()) {
			close_prop();
			struct timespec ts = { 0, 200 * 1000 * 1000 };
			nanosleep(&ts, NULL);
			continue;
		}

		if (ac.fd < 0) {
			// rate-limit opens so a driveless or physcd-busy setup is not thrashed
			if (now - last_open_try < 1000) {
				struct timespec ts = { 0, 100 * 1000 * 1000 };
				nanosleep(&ts, NULL);
				continue;
			}
			last_open_try = now;
			if (open_prop()) {
				struct timespec ts = { 0, 100 * 1000 * 1000 };
				nanosleep(&ts, NULL);
				continue;
			}
			last_pos = -1000000;   // force the first op on a fresh disc
			cur_speed = 0;         // re-ramp speed on a fresh disc
		}

		// intensity sampler: seq bumps once per hinted sector, so its rate is
		// roughly the sectors/s the game is pulling (1x CD = 75/s). heavy load
		// -> big bursts, tiny gap (steady whirr); a trickle -> small spaced
		// reads (gentle hum).
		if (now - t_sample >= SAMPLE_MS) {
			unsigned d = seq - seq_prev;
			double dt = now - t_sample;
			double inst = dt > 0 ? d * 1000.0 / dt : 0;
			rate += (inst - rate) * 0.3;   // light smoothing
			seq_prev = seq;
			t_sample = now;
		}
		int burst = 2 + (int)((rate - 8) / 6);
		if (burst < MIN_BURST) burst = MIN_BURST;
		if (burst > MAX_BURST) burst = MAX_BURST;
		double gap = 120.0 - rate;
		if (gap < 10.0) gap = 10.0;
		if (gap > 120.0) gap = 120.0;

		// spin the motor UP under load and DOWN when light by ramping the
		// drive's target speed with intensity. only in the read tier - the
		// seek-only fallback is meant to be quiet clicking, not spinning. the
		// target glides one step per debounce with a full-step deadband, so it
		// does not snap or warble on a boundary. (1x CD = 75 sectors/s.)
		if (!ac.read_unsupported) {
			double target = SPEED_MIN + rate / 12.0;
			if (target > SPEED_MAX) target = SPEED_MAX;
			int want = cur_speed;
			if (cur_speed < SPEED_MIN) want = SPEED_MIN;                // up from the reset floor
			else if (target >= cur_speed + SPEED_STEP) want = cur_speed + SPEED_STEP;
			else if (target <= cur_speed - SPEED_STEP) want = cur_speed - SPEED_STEP;
			if (want > SPEED_MAX) want = SPEED_MAX;
			if (want < SPEED_MIN) want = SPEED_MIN;
			if (want != cur_speed && now - last_speed_ms >= SPEED_DEBOUNCE_MS) {
				set_speed(ac.fd, want);
				cur_speed = want;
				last_speed_ms = now;
			}
		}

		int lba = map_lba(ac.target_lba);
		int jump = lba > last_pos ? lba - last_pos : last_pos - lba;

		if (!ac.read_unsupported) {
			// TIER 1: real reads spin the disc
			if (jump >= JUMP_THRESHOLD || now - last_read >= gap) {
				int r = sg_read10(ac.fd, lba, burst);
				if (r == 0) {
					read_fails = 0; seek_fails = 0; reopen_fails = 0;
					last_read = now; last_pos = lba;
				} else if (r == -1) {
					// transport failure: the drive likely dropped off the bus.
					// reopen next round, but do not retry forever.
					close_prop(); read_fails = 0;
					if (++reopen_fails >= REOPEN_GIVEUP) {
						printf("physcd_acoustic: drive keeps dropping, disabling for this session\n");
						ac.unsupported = 1;
					}
				} else {
					// CHECK CONDITION: an isolated bad sector, or the bridge does
					// not do READ(10). click once; after a run of them, drop to
					// seek-only for the session rather than thrash.
					if (++read_fails >= READ_GIVEUP) {
						printf("physcd_acoustic: drive rejects READ(10), seek-only fallback\n");
						ac.read_unsupported = 1;
					} else {
						sg_seek(ac.fd, lba);
					}
					last_read = now; last_pos = lba;
				}
			}
		} else if (!ac.unsupported) {
			// TIER 2: seek-only (the original clicking behaviour)
			if (jump >= JUMP_THRESHOLD || now - last_read >= KEEPALIVE_MS) {
				int r = sg_seek(ac.fd, lba);
				if (r == -1) {
					close_prop(); seek_fails = 0;
					if (++reopen_fails >= REOPEN_GIVEUP) {
						printf("physcd_acoustic: drive keeps dropping, disabling for this session\n");
						ac.unsupported = 1;
					}
				} else if (r < 0) {
					if (++seek_fails >= 4) {
						printf("physcd_acoustic: drive does not accept SEEK, disabling for this session\n");
						ac.unsupported = 1;
						close_prop();
					}
				} else {
					seek_fails = 0; reopen_fails = 0;
					last_read = now; last_pos = lba;
				}
			}
		}

		struct timespec ts = { 0, 20 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}

	close_prop();
	return NULL;
}

void physcd_acoustic_config(int enabled)
{
	ac.enabled = enabled ? 1 : 0;
	if (ac.enabled && !ac.running) {
		ac.running = 1;
		ac.paused = 0;
		if (pthread_create(&ac.thread, NULL, acoustic_thread, NULL)) {
			ac.running = 0;
			printf("physcd_acoustic: could not start thread\n");
			return;
		}
		printf("physcd_acoustic: enabled - put a spare data disc in the drive\n");
	}
}

// MUST be called from a single thread. its only caller is the per-core CHD
// read path (mister_chd_read_sector), so target_lba and seq have exactly one
// writer - which is why plain volatile is enough here and seq++ (a non-atomic
// read-modify-write) is safe. if a core ever drove CHD reads from two threads
// these would need to become atomics.
void physcd_acoustic_hint(int lba)
{
	if (!ac.enabled || ac.paused) return;
	ac.target_lba = lba;
	ac.seq++;
}

void physcd_acoustic_pause(void)
{
	ac.paused = 1;
}

void physcd_acoustic_resume(void)
{
	ac.paused = 0;
}
