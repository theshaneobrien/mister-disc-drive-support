/*
 * physcd_probe - validate a USB CD-ROM drive for MiSTer physical disc use
 *
 * runs on a STOCK mister (kernel already has sr/cdrom built in).
 * reads the TOC, fingerprints the disc type (mega cd / saturn / psx /
 * pcecd / neogeo cd / audio), and benchmarks raw 2352-byte reads and
 * seek latency via SG_IO READ CD (0xBE).
 *
 * cross-compile:
 *   arm-none-linux-gnueabihf-gcc -O2 -static -o physcd_probe physcd_probe.c
 * run on mister:
 *   ./physcd_probe /dev/sr0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/cdrom.h>
#include <scsi/sg.h>
#include <limits.h>

#define RAW_SECTOR 2352

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* READ CD (0xBE): raw 2352-byte read. flags 0xF8 = sync + headers +
 * user data + EDC/ECC (data tracks); 0x10 = user data only (cd-da,
 * where user data IS the full 2352 - some usb bridges reject 0xF8 on
 * audio tracks, so the backend picks the byte per track type). */
static int read_cd_flags(int fd, uint32_t lba, uint32_t count, uint8_t flags, uint8_t *buf)
{
	uint8_t cdb[12] = { 0 };
	uint8_t sense[32];
	struct sg_io_hdr io;

	cdb[0] = 0xBE;                 /* READ CD */
	cdb[1] = 0;                    /* expected sector type: any */
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[6] = (count >> 16) & 0xFF;
	cdb[7] = (count >> 8) & 0xFF;
	cdb[8] = count & 0xFF;
	cdb[9] = flags;
	cdb[10] = 0;                   /* no subchannel (probe separately) */

	memset(&io, 0, sizeof(io));
	io.interface_id = 'S';
	io.cmd_len = 12;
	io.cmdp = cdb;
	io.dxfer_direction = SG_DXFER_FROM_DEV;
	io.dxfer_len = count * RAW_SECTOR;
	io.dxferp = buf;
	io.sbp = sense;
	io.mx_sb_len = sizeof(sense);
	io.timeout = 10000;

	if (ioctl(fd, SG_IO, &io) < 0) return -1;
	if (io.status || io.host_status || io.driver_status) return -2;
	return 0;
}

static int read_cd_raw(int fd, uint32_t lba, uint32_t count, uint8_t *buf)
{
	return read_cd_flags(fd, lba, count, 0xF8, buf);
}

/* cooked kernel fallback: one raw 2352 sector, MSF-addressed (+150) */
static int read_cooked_raw(int fd, uint32_t lba, uint8_t *buf)
{
	union {
		struct cdrom_msf msf;
		uint8_t raw[RAW_SECTOR];
	} req;

	uint32_t f = lba + 150;
	memset(&req, 0, sizeof(req));
	req.msf.cdmsf_min0 = f / (75 * 60);
	req.msf.cdmsf_sec0 = (f / 75) % 60;
	req.msf.cdmsf_frame0 = f % 75;

	if (ioctl(fd, CDROMREADRAW, &req) < 0) return -1;
	memcpy(buf, req.raw, RAW_SECTOR);
	return 0;
}

/* READ CD with raw P-W subchannel appended (2352 + 96) */
static int read_cd_raw_sub(int fd, uint32_t lba, uint8_t *buf)
{
	uint8_t cdb[12] = { 0 };
	uint8_t sense[32];
	struct sg_io_hdr io;

	cdb[0] = 0xBE;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0xF8;
	cdb[10] = 0x01;                /* raw P-W, 96 bytes */

	memset(&io, 0, sizeof(io));
	io.interface_id = 'S';
	io.cmd_len = 12;
	io.cmdp = cdb;
	io.dxfer_direction = SG_DXFER_FROM_DEV;
	io.dxfer_len = RAW_SECTOR + 96;
	io.dxferp = buf;
	io.sbp = sense;
	io.mx_sb_len = sizeof(sense);
	io.timeout = 10000;

	if (ioctl(fd, SG_IO, &io) < 0) return -1;
	if (io.status || io.host_status || io.driver_status) return -2;
	return 0;
}

struct track_info {
	int num;
	int is_data;
	uint32_t start_lba;
};

/* mega drive style header: "SEGA..." at 0x100, region at 0x1F0. mega cd
 * discs mirror it in their first data sector. a pal disc booted on a us
 * bios plays but runs at the wrong timing and stutters, so the mount
 * path uses this to pick a matching bios - print it here so a disc can
 * be checked without deploying. */
static const char *md_region(const uint8_t *user)
{
	if (memcmp(user + 0x100, "SEGA", 4)) return NULL;

	const char *f = (const char *)user + 0x1F0;
	int has_j = 0, has_u = 0, has_e = 0, junk = 0;
	for (int i = 0; i < 3; i++) {
		char c = f[i];
		if (c == 'J') has_j = 1;
		else if (c == 'U') has_u = 1;
		else if (c == 'E') has_e = 1;
		else if (c != ' ' && c != 0) junk = 1;
	}

	if (!junk && (has_j || has_u || has_e)) {
		if (has_u) return "US";
		if (has_e) return "EU";
		return "JP";
	}

	int v = -1;
	if (f[0] >= '0' && f[0] <= '9') v = f[0] - '0';
	else if (f[0] >= 'A' && f[0] <= 'F') v = f[0] - 'A' + 10;
	if (v > 0) {
		if (v & 4) return "US";
		if (v & 8) return "EU";
		if (v & 1) return "JP";
	}
	return NULL;
}

static const char *fingerprint(int fd, struct track_info *tracks, int ntracks)
{
	uint8_t buf[RAW_SECTOR * 2];
	int have_data = 0;
	uint32_t first_data_lba = 0;

	for (int i = 0; i < ntracks; i++) {
		if (tracks[i].is_data) { have_data = 1; first_data_lba = tracks[i].start_lba; break; }
	}
	if (!have_data) return "audio cd";

	/* sector 0 of first data track: sega signatures live at offset 16 in raw (after sync+header) */
	if (read_cd_raw(fd, first_data_lba, 1, buf) == 0) {
		uint8_t *user = buf + 16;
		if (!memcmp(user, "SEGADISCSYSTEM", 14)) return "mega cd";
		if (!memcmp(user, "SEGA SEGASATURN", 15)) return "saturn";
	}

	/* iso9660 pvd at lba 16: system identifier at user offset 8 */
	if (read_cd_raw(fd, first_data_lba + 16, 1, buf) == 0) {
		uint8_t *user = buf + 16;
		/* mode2 form1 (psx) has an 8-byte subheader before iso data */
		uint8_t *iso = user;
		if (memcmp(iso + 1, "CD001", 5)) iso = user + 8;
		if (!memcmp(iso + 1, "CD001", 5)) {
			if (!memcmp(iso + 8, "PLAYSTATION", 11)) return "psx";
			/* neogeo cd: iso9660, check for system id or fall through to file check */
			if (!memcmp(iso + 8, "NGCD", 4)) return "neogeo cd";
		}
	}

	/* pce cd: signature in the boot sector of the data track */
	if (read_cd_raw(fd, first_data_lba, 2, buf) == 0) {
		for (int off = 0; off < RAW_SECTOR * 2 - 24; off++) {
			if (!memcmp(buf + off, "PC Engine CD-ROM SYSTEM", 23)) return "pce cd";
		}
	}

	return "unknown data disc (possibly neogeo cd or pc-fx, needs iso file listing)";
}

int main(int argc, char **argv)
{
	const char *dev = (argc > 1) ? argv[1] : "/dev/sr0";
	int fd = open(dev, O_RDONLY | O_NONBLOCK);
	if (fd < 0) { perror(dev); return 1; }

	int status = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
	printf("drive status: %s\n",
		status == CDS_DISC_OK ? "disc ok" :
		status == CDS_NO_DISC ? "no disc" :
		status == CDS_TRAY_OPEN ? "tray open" :
		status == CDS_DRIVE_NOT_READY ? "not ready" : "unknown");
	if (status != CDS_DISC_OK) return 1;

	struct cdrom_tochdr hdr;
	if (ioctl(fd, CDROMREADTOCHDR, &hdr) < 0) { perror("toc hdr"); return 1; }
	printf("tracks: %d-%d\n", hdr.cdth_trk0, hdr.cdth_trk1);

	struct track_info tracks[100];
	int ntracks = 0;

	for (int t = hdr.cdth_trk0; t <= hdr.cdth_trk1; t++) {
		struct cdrom_tocentry e;
		memset(&e, 0, sizeof(e));
		e.cdte_track = t;
		e.cdte_format = CDROM_LBA;
		if (ioctl(fd, CDROMREADTOCENTRY, &e) < 0) continue;
		tracks[ntracks].num = t;
		tracks[ntracks].is_data = !!(e.cdte_ctrl & CDROM_DATA_TRACK);
		tracks[ntracks].start_lba = e.cdte_addr.lba;
		printf("  track %02d: %s  lba %u\n", t,
			tracks[ntracks].is_data ? "data " : "audio", e.cdte_addr.lba);
		ntracks++;
	}

	struct cdrom_tocentry lead;
	memset(&lead, 0, sizeof(lead));
	lead.cdte_track = CDROM_LEADOUT;
	lead.cdte_format = CDROM_LBA;
	if (ioctl(fd, CDROMREADTOCENTRY, &lead) == 0)
		printf("  leadout : lba %u (%u MB raw)\n", lead.cdte_addr.lba,
			(unsigned)((uint64_t)lead.cdte_addr.lba * RAW_SECTOR / (1024 * 1024)));

	printf("\ndisc type: %s\n", fingerprint(fd, tracks, ntracks));

	/* region, for sega discs that carry a mega drive style header */
	for (int i = 0; i < ntracks; i++) {
		uint8_t sec[RAW_SECTOR];
		if (!tracks[i].is_data) continue;
		if (read_cd_raw(fd, tracks[i].start_lba, 1, sec)) break;

		const char *r = md_region(sec + 16);
		if (r) printf("disc region: %s  (needs a %s BIOS: boot_%s.rom)\n", r, r, r);
		else printf("disc region: not found in header\n");
		break;
	}
	printf("\n");

	/* benchmark: sequential raw read */
	uint8_t *big = malloc(RAW_SECTOR * 32);
	if (!big) return 1;
	uint32_t base = tracks[0].start_lba + 200;
	double t0 = now_ms();
	int ok = 0;
	for (int i = 0; i < 16; i++)
		if (read_cd_raw(fd, base + i * 32, 32, big) == 0) ok++;
	double t1 = now_ms();
	if (ok)
		printf("sequential: %d/16 bursts ok, %.1f KB/s raw (need 172 for 1x cdda)\n",
			ok, (ok * 32.0 * RAW_SECTOR) / (t1 - t0) * 1000.0 / 1024.0);

	/* benchmark: seek latency, jump ~40k sectors back and forth */
	uint32_t far_lba = (lead.cdte_addr.lba > 60000) ? base + 40000 : base + lead.cdte_addr.lba / 2;
	double worst = 0, total = 0;
	int n = 0;
	for (int i = 0; i < 6; i++) {
		uint32_t lba = (i & 1) ? far_lba : base;
		double s0 = now_ms();
		if (read_cd_raw(fd, lba, 1, big) == 0) {
			double d = now_ms() - s0;
			total += d; n++;
			if (d > worst) worst = d;
		}
	}
	if (n)
		printf("seek+read : avg %.0f ms, worst %.0f ms across %d long jumps\n", total / n, worst, n);

	/* subchannel capability */
	uint8_t sub[RAW_SECTOR + 96];
	int r = read_cd_raw_sub(fd, base, sub);
	printf("raw subchannel read: %s\n", r == 0 ? "supported" : "NOT supported (libcrypt psx titles will need .sbi)");

	/* flags matrix: which READ CD flags byte this drive honors per
	 * track type, plus the cooked kernel fallback. the backend needs
	 * 0xF8-on-data; audio works with either 0xF8 or 0x10. */
	printf("\nflags matrix:\n");
	printf("  data  0xF8: %s\n", read_cd_flags(fd, base, 1, 0xF8, big) == 0 ? "ok" : "FAIL");
	printf("  data  CDROMREADRAW: %s\n", read_cooked_raw(fd, base, big) == 0 ? "ok" : "FAIL");

	uint32_t audio_lba = 0;
	int have_audio = 0;
	for (int i = 0; i < ntracks; i++) {
		if (!tracks[i].is_data) {
			audio_lba = tracks[i].start_lba + 5;
			have_audio = 1;
			break;
		}
	}
	if (have_audio) {
		printf("  audio 0xF8: %s\n", read_cd_flags(fd, audio_lba, 1, 0xF8, big) == 0 ? "ok" : "FAIL (backend uses 0x10 on audio)");
		printf("  audio 0x10: %s\n", read_cd_flags(fd, audio_lba, 1, 0x10, big) == 0 ? "ok" : "FAIL");
	} else {
		printf("  audio     : no audio track on this disc, insert a mixed-mode disc to test\n");
	}

	free(big);
	close(fd);
	return 0;
}
