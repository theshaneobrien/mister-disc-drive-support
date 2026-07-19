/*
 * physcdd - physical cd autodetect daemon for the MiSTer physcd fork
 *
 * polls the drive every 2s; when a disc appears it fingerprints the
 * type, resolves the matching core rbf, loads the core through
 * /dev/MiSTer_cmd, waits for /tmp/CORENAME to flip, then issues
 * mount_phys. eject does nothing in v1 (core keeps running).
 *
 * config (optional): /media/fat/physcd.ini
 *   megacd=/media/fat/_Console/MegaCD_20231228.rbf
 *   psx=...  saturn=...  pcecd=...  neogeo=...
 *   device=/dev/sr0
 * unset types resolve to the newest /media/fat/_Console/<Name>*.rbf.
 *
 * cross-compile:
 *   arm-none-linux-gnueabihf-gcc -O2 -static -o physcdd physcdd.c
 * run from /media/fat/linux/user-startup.sh:
 *   /media/fat/physcdd >>/tmp/physcdd.log 2>&1 &
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/cdrom.h>
#include <scsi/sg.h>
#include <limits.h>

#define RAW_SECTOR 2352
#define CMD_FIFO   "/dev/MiSTer_cmd"
#define CORENAME   "/tmp/CORENAME"
#define INI_PATH   "/media/fat/physcd.ini"
#define CONSOLE_DIR "/media/fat/_Console"

typedef enum {
	DISC_NONE = 0, DISC_MEGACD, DISC_SATURN, DISC_PSX,
	DISC_PCECD, DISC_NEOGEO, DISC_AUDIO, DISC_UNKNOWN,
} disc_t;

/* per-type launch info: ini key, /tmp/CORENAME value, rbf name prefix */
static const struct {
	const char *ini_key;
	const char *core_name;
	const char *rbf_prefix;
} launch[] = {
	[DISC_MEGACD] = { "megacd", "MEGACD", "MegaCD" },
	[DISC_SATURN] = { "saturn", "Saturn", "Saturn" },
	[DISC_PSX]    = { "psx",    "PSX",    "PSX"    },
	[DISC_PCECD]  = { "pcecd",  "TGFX16", "TurboGrafx16" },
	[DISC_NEOGEO] = { "neogeo", "NEOGEO", "NeoGeo" },
};

static const char *disc_name(disc_t t)
{
	switch (t) {
	case DISC_MEGACD: return "MegaCD";
	case DISC_SATURN: return "Saturn";
	case DISC_PSX:    return "PSX";
	case DISC_PCECD:  return "TurboGrafx CD";
	case DISC_NEOGEO: return "NeoGeo CD";
	case DISC_AUDIO:  return "Audio CD";
	case DISC_NONE:   return "No Disc";
	default:          return "Unknown";
	}
}

static void log_line(const char *fmt, ...)
{
	char ts[32];
	time_t t = time(NULL);
	strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
	printf("[%s] ", ts);
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

// ---------------------------------------------------------------- reads

static int sg_read_cd(int fd, uint32_t lba, uint8_t flags, uint8_t *buf)
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
	cdb[9] = flags;

	memset(&io, 0, sizeof(io));
	io.interface_id = 'S';
	io.cmd_len = 12;
	io.cmdp = cdb;
	io.dxfer_direction = SG_DXFER_FROM_DEV;
	io.dxfer_len = RAW_SECTOR;
	io.dxferp = buf;
	io.sbp = sense;
	io.mx_sb_len = sizeof(sense);
	io.timeout = 10000;

	if (ioctl(fd, SG_IO, &io) < 0) return -1;
	if (io.status || io.host_status || io.driver_status) return -2;
	return 0;
}

static int cooked_read_raw(int fd, uint32_t lba, uint8_t *buf)
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

/* raw read with the full fallback ladder (data-track sectors only) */
static int read_sector(int fd, uint32_t lba, uint8_t *buf)
{
	if (!sg_read_cd(fd, lba, 0xF8, buf)) return 0;
	if (!sg_read_cd(fd, lba, 0x10, buf)) return 0;
	return cooked_read_raw(fd, lba, buf);
}

// ---------------------------------------------------------------- ident

static disc_t identify(int fd)
{
	struct cdrom_tochdr hdr;
	uint8_t buf[RAW_SECTOR * 2];
	uint32_t first_data = 0;
	int have_data = 0;

	if (ioctl(fd, CDROMREADTOCHDR, &hdr) < 0) return DISC_NONE;

	for (int t = hdr.cdth_trk0; t <= hdr.cdth_trk1; t++) {
		struct cdrom_tocentry e;
		memset(&e, 0, sizeof(e));
		e.cdte_track = t;
		e.cdte_format = CDROM_LBA;
		if (ioctl(fd, CDROMREADTOCENTRY, &e) < 0) continue;
		if (e.cdte_ctrl & CDROM_DATA_TRACK) {
			have_data = 1;
			first_data = e.cdte_addr.lba;
			break;
		}
	}
	if (!have_data) return DISC_AUDIO;

	if (!read_sector(fd, first_data, buf)) {
		if (!memcmp(buf + 16, "SEGADISCSYSTEM", 14)) return DISC_MEGACD;
		if (!memcmp(buf + 16, "SEGA SEGASATURN", 15)) return DISC_SATURN;
	}

	if (!read_sector(fd, first_data + 16, buf)) {
		uint8_t *iso = buf + 16;
		if (memcmp(iso + 1, "CD001", 5)) iso = buf + 24;   /* mode2 form1 */
		if (!memcmp(iso + 1, "CD001", 5)) {
			if (!memcmp(iso + 8, "PLAYSTATION", 11)) return DISC_PSX;
			if (!memcmp(iso + 8, "NGCD", 4)) return DISC_NEOGEO;
		}
	}

	if (!read_sector(fd, first_data, buf) &&
	    !read_sector(fd, first_data + 1, buf + RAW_SECTOR)) {
		for (int off = 0; off < (int)sizeof(buf) - 24; off++)
			if (!memcmp(buf + off, "PC Engine CD-ROM SYSTEM", 23))
				return DISC_PCECD;
	}

	return DISC_UNKNOWN;
}

// ---------------------------------------------------------------- launch

static int ini_lookup(const char *key, char *out, int sz)
{
	FILE *f = fopen(INI_PATH, "r");
	char line[512];
	int found = 0;

	if (!f) return 0;
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		if (strcasecmp(line, key)) continue;
		char *v = eq + 1;
		v[strcspn(v, "\r\n")] = 0;
		if (*v) { snprintf(out, sz, "%s", v); found = 1; }
		break;
	}
	fclose(f);
	return found;
}

/* newest (lexically largest, dates sort) CONSOLE_DIR/<prefix>[._]*.rbf */
static int find_rbf(const char *prefix, char *out, int sz)
{
	DIR *d = opendir(CONSOLE_DIR);
	struct dirent *e;
	char best[256] = "";
	int plen = strlen(prefix);

	if (!d) return 0;
	while ((e = readdir(d))) {
		int nlen = strlen(e->d_name);
		if (nlen < plen + 4) continue;
		if (strncasecmp(e->d_name, prefix, plen)) continue;
		if (e->d_name[plen] != '_' && e->d_name[plen] != '.') continue;
		if (strcasecmp(e->d_name + nlen - 4, ".rbf")) continue;
		if (strcmp(e->d_name, best) > 0) snprintf(best, sizeof(best), "%s", e->d_name);
	}
	closedir(d);

	if (!best[0]) return 0;
	snprintf(out, sz, "%s/%s", CONSOLE_DIR, best);
	return 1;
}

static int fifo_cmd(const char *fmt, ...)
{
	char cmd[1200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);

	int fd = open(CMD_FIFO, O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		log_line("cannot send '%s': %s (%s)", cmd, CMD_FIFO, strerror(errno));
		return -1;
	}
	dprintf(fd, "%s\n", cmd);
	close(fd);
	log_line("sent: %s", cmd);
	return 0;
}

static int core_running(const char *name)
{
	char cur[64] = "";
	FILE *f = fopen(CORENAME, "r");
	if (!f) return 0;
	if (fgets(cur, sizeof(cur), f)) cur[strcspn(cur, "\r\n")] = 0;
	fclose(f);
	return !strcasecmp(cur, name);
}

/* trailing digit of /dev/srN, so mount_phys pins the same drive we
   just fingerprinted rather than letting main autodetect a different one */
static int dev_index(const char *dev)
{
	const char *p = dev + strlen(dev) - 1;
	return (*p >= '0' && *p <= '9') ? *p - '0' : 0;
}

static void on_disc(disc_t type, const char *dev)
{
	char rbf[1024];

	log_line("disc detected: %s", disc_name(type));
	if (type < DISC_MEGACD || type > DISC_NEOGEO) return;

	if (!core_running(launch[type].core_name)) {
		if (!ini_lookup(launch[type].ini_key, rbf, sizeof(rbf)) &&
		    !find_rbf(launch[type].rbf_prefix, rbf, sizeof(rbf))) {
			log_line("no rbf found for %s (looked for %s/%s*.rbf and %s)",
				disc_name(type), CONSOLE_DIR, launch[type].rbf_prefix, INI_PATH);
			return;
		}

		if (fifo_cmd("load_core %s", rbf)) return;

		int up = 0;
		for (int i = 0; i < 30; i++) {          /* 15s */
			usleep(500 * 1000);
			if (core_running(launch[type].core_name)) { up = 1; break; }
		}
		if (!up) {
			log_line("core %s did not come up, skipping mount", launch[type].core_name);
			return;
		}
		sleep(3);                               /* let the core init + bios load */
	}

	fifo_cmd("mount_phys %d", dev_index(dev));
}

// ---------------------------------------------------------------- main

/* usb enumeration order shifts across reboots (the same drive shows up
   as sr0 one boot, sr1 the next), so rescan every time we reopen.
   O_NONBLOCK keeps the drive door usable. */
static int open_drive(char *dev, int devsz, const char *pinned)
{
	char path[64];
	int spare = -1;

	if (pinned && *pinned) {
		int fd = open(pinned, O_RDONLY | O_NONBLOCK);
		if (fd >= 0) { snprintf(dev, devsz, "%s", pinned); return fd; }
	}

	for (int i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "/dev/sr%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0) continue;
		if (ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT) == CDS_DISC_OK) {
			snprintf(dev, devsz, "%s", path);
			if (spare >= 0) close(spare);
			return fd;
		}
		if (spare < 0) { spare = fd; snprintf(dev, devsz, "%s", path); }
		else close(fd);
	}
	return spare;
}

int main(int argc, char **argv)
{
	char pinned[256] = "";
	char dev[256] = "";
	int last = -1;

	if (argc > 1) snprintf(pinned, sizeof(pinned), "%s", argv[1]);
	else ini_lookup("device", pinned, sizeof(pinned));

	log_line("physcdd started, watching %s", pinned[0] ? pinned : "/dev/sr0../dev/sr7 (autodetect)");

	int fd = -1;
	for (;;) {
		if (fd < 0) {
			fd = open_drive(dev, sizeof(dev), pinned);
			if (fd < 0) { sleep(5); continue; }
			log_line("using %s", dev);
		}

		int status = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
		int changed = ioctl(fd, CDROM_MEDIA_CHANGED, CDSL_CURRENT) > 0;

		if (status < 0) {                       /* drive unplugged? */
			close(fd);
			fd = -1;
			last = -1;
			sleep(5);
			continue;
		}

		if (status == CDS_DISC_OK && (last != CDS_DISC_OK || changed))
			on_disc(identify(fd), dev);

		last = status;
		sleep(2);
	}
	return 0;
}
