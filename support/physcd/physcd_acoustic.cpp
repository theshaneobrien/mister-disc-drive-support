/*
 * physcd_acoustic.cpp - acoustic seek (prototype). see physcd_acoustic.h.
 *
 * we already know the lba an image-backed game is reading (the chd read path
 * hands it to physcd_acoustic_hint), so mirror those positions onto a real
 * usb drive holding a throwaway "prop" disc. the drive physically seeks and
 * spins in step with the game.
 *
 *  - the hint runs on the fpga-answering thread, so it does the bare minimum:
 *    stash the lba, bump an activity counter. no ioctls, no allocation, no
 *    blocking. everything expensive is on the background thread.
 *  - the background thread issues SCSI SEEK(10) toward the latest hinted lba:
 *    a big jump becomes a big head movement (the loud seek), and during
 *    steady sequential reads it just keep-alives every so often (the steady
 *    spin). when the game goes quiet it stops, and the drive spins down on
 *    its own - exactly like a real console between loads.
 *  - the prop disc only needs a table of contents so seeks land; the data on
 *    it is irrelevant because SEEK transfers nothing. a BLANK disc will not
 *    work (no toc, the drive will not accept lba seeks).
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

#define ACTIVE_MS       600       // no hints for this long -> game idle, spin down
#define JUMP_THRESHOLD  90        // lba delta that counts as a real seek (~1.2s)
#define KEEPALIVE_MS    400       // during sustained reads, re-seek this often to hold spin
#define SEEK_TIMEOUT_MS 4000

static struct {
	volatile int enabled;
	volatile int paused;
	volatile int running;
	volatile int unsupported;     // drive rejected SEEK; give up for this session
	volatile int target_lba;
	volatile unsigned seq;        // bumped by every hint; the thread watches it for activity
	int fd;                       // thread-owned: only the acoustic thread touches it
	int prop_max;                 // leadout lba of the prop disc, for clamping
	pthread_t thread;
} ac = { 0, 0, 0, 0, 0, 0, -1, 0, 0 };

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// SCSI SEEK(10): move the head to lba, transfer nothing. works on any disc
// with a toc no matter what (if anything) is burned to it.
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

// open the first /dev/srN that has a disc and read its leadout for clamping.
// refuses to touch the drive if physcd (a physical game mount) already holds
// it. O_CLOEXEC so a core load (which execs) never leaks the fd.
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
		ac.fd = fd;
		printf("physcd_acoustic: prop disc on %s, %d sectors\n", path, ac.prop_max);
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
	unsigned last_seq = 0;
	double last_active = 0;
	double last_open_try = 0;
	double last_seek = 0;
	int last_seek_lba = -1000000;   // far below any real lba, forces the first seek
	int seek_fails = 0;             // consecutive SEEK rejections (CHECK CONDITION)

	while (ac.running) {
		// paused (menu owns the drive), disabled, or the drive turned out not
		// to support SEEK: let go and idle
		if (!ac.enabled || ac.paused || ac.unsupported) {
			close_prop();
			struct timespec ts = { 0, 150 * 1000 * 1000 };
			nanosleep(&ts, NULL);
			continue;
		}

		unsigned seq = ac.seq;
		double now = now_ms();
		if (seq != last_seq) { last_seq = seq; last_active = now; }

		// game quiet for a while: stop touching the drive so it spins down,
		// which is exactly what a real console does between loads
		if (now - last_active >= ACTIVE_MS) {
			struct timespec ts = { 0, 80 * 1000 * 1000 };
			nanosleep(&ts, NULL);
			continue;
		}

		// physcd grabbed the drive (a physical mount): back off entirely.
		// cannot happen in today's topology - acoustic only gets hints during
		// image play, where physcd is closed - but this keeps the "never touch
		// the drive while physcd holds it" invariant true if that ever changes.
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
			last_seek_lba = -1000000;   // far below any real lba, forces the first seek      // force the first seek on a fresh disc
		}

		int lba = ac.target_lba;
		if (lba < 0) lba = 0;
		if (ac.prop_max > 0 && lba >= ac.prop_max) lba = ac.prop_max - 1;

		int jump = lba > last_seek_lba ? lba - last_seek_lba : last_seek_lba - lba;
		int do_seek = (jump >= JUMP_THRESHOLD) || (now - last_seek >= KEEPALIVE_MS);

		if (do_seek) {
			int r = sg_seek(ac.fd, lba);
			if (r == -1) {
				// ioctl failed: the drive likely dropped off the usb bus.
				// reopen next round.
				close_prop();
				seek_fails = 0;
			} else if (r < 0) {
				// SCSI CHECK CONDITION: the bridge rejected SEEK(10). plenty of
				// usb optical bridges do not support the legacy opcode. do NOT
				// close+reopen (that just grinds the drive once a second) -
				// count, and after a few give up for the whole session.
				if (++seek_fails >= 4) {
					printf("physcd_acoustic: drive does not accept SEEK, disabling for this session\n");
					ac.unsupported = 1;
					close_prop();
				}
			} else {
				seek_fails = 0;
				last_seek = now;
				last_seek_lba = lba;
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
		printf("physcd_acoustic: enabled (prototype) - put a prop disc in the drive\n");
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
