#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>

#include "../../file_io.h"
#include "../../user_io.h"
#include "../../fpga_io.h"
#include "../../cfg.h"
#include "physcd_log.h"

#define LOG_NAME  "physcd_log.txt"
#define LOG_TMP   "physcd_log.tmp"
#define LOG_CAP   (192 * 1024)   /* truncate to the last LOG_KEEP below this */
#define LOG_KEEP  (128 * 1024)

static int g_level = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static double g_t0 = -1;

/* the paths are resolved ONCE, on the main thread, in physcd_log_startup().
   getFullPath() formats into a process-wide static buffer with no lock
   (file_io.cpp make_fullpath) that the menu thread uses constantly for real
   file opens - calling it from the watcher thread would let a disc insert
   corrupt an unrelated path mid-open. this file must never call it again. */
static char g_path[1024];
static char g_tmp[1024];

void physcd_log_config(int level)
{
	g_level = level;
}

int physcd_log_level(void)
{
	return g_level;
}

static double log_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	double t = ts.tv_sec + ts.tv_nsec / 1e9;
	if (g_t0 < 0) g_t0 = t;
	return t - g_t0;
}

/* keep the file from ever growing without bound: past LOG_CAP, keep only the
   last LOG_KEEP bytes. a support log is read from the end. tmp+rename, and on
   ANY failure the tmp goes and the original is left untouched - a truncation
   that half-works is worse than a big file. caller holds g_lock. */
static void log_compact(void)
{
	struct stat st;
	if (stat(g_path, &st) || st.st_size <= LOG_CAP) return;

	FILE *in = fopen(g_path, "rb");
	if (!in) return;
	if (fseek(in, st.st_size - LOG_KEEP, SEEK_SET)) { fclose(in); return; }

	FILE *out = fopen(g_tmp, "wb");
	if (!out) { fclose(in); return; }

	fprintf(out, "=== earlier entries trimmed (log passed %d KB) ===\n", LOG_CAP / 1024);

	/* the seek lands mid-line; drop the fragment so the first kept line is
	   whole and the file stays greppable */
	int c;
	while ((c = fgetc(in)) != EOF && c != '\n') {}

	char buf[8192];
	size_t n;
	int ok = 1;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
	if (ferror(in)) ok = 0;   /* a short read would rename a TRUNCATED copy over the log */

	fclose(in);
	if (fclose(out) || !ok) { unlink(g_tmp); return; }
	if (rename(g_tmp, g_path)) unlink(g_tmp);
}

/* one append, flushed to the card before returning. every caller is a session
   boundary on a thread where blocking is free - see the header. multi-line
   blocks come through here as ONE write: an fsync per line would put dozens
   of card writes inside pcd.io during a disc swap. */
static void log_emit(const char *text)
{
	if (!g_level || !g_path[0]) return;

	pthread_mutex_lock(&g_lock);
	log_compact();

	FILE *f = fopen(g_path, "a");
	if (f)
	{
		fputs(text, f);
		fflush(f);
		fsync(fileno(f));
		fclose(f);
	}
	/* a full or read-only card just loses the line. never retry, never warn:
	   the log is a convenience, not something to nag about. */
	pthread_mutex_unlock(&g_lock);
}

void physcd_log(const char *fmt, ...)
{
	if (!g_level) return;

	char line[512];
	int n = snprintf(line, sizeof(line), "[%8.3f] ", log_now());
	if (n < 0 || n >= (int)sizeof(line)) return;

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
	va_end(ap);

	size_t len = strlen(line);
	line[len] = '\n';
	line[len + 1] = 0;
	log_emit(line);
}

void physcd_log_block(const char *text)
{
	if (!g_level) return;

	char block[2048];
	snprintf(block, sizeof(block), "[%8.3f] %s", log_now(), text);
	log_emit(block);
}

void physcd_log_startup(void)
{
	physcd_log_config(cfg.physcd_log);
	if (!g_level) return;

	/* main thread, before any other thread exists - see the note on g_path */
	snprintf(g_path, sizeof(g_path), "%s", getFullPath(LOG_NAME));
	snprintf(g_tmp, sizeof(g_tmp), "%s", getFullPath(LOG_TMP));

	const char *app = getappname();
	const char *base = strrchr(app, '/');
	base = base ? base + 1 : app;

	/* wall clock as well as the monotonic stamps: those restart at 0.000 in
	   every process, so without this a log cannot be dated or matched to a
	   user saying "it happened about 8pm". no rtc on some setups, so this is
	   best-effort and may read as epoch - still better than nothing. */
	time_t now = time(NULL);
	char when[64];
	struct tm tmv;
	if (localtime_r(&now, &tmv)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);
	else snprintf(when, sizeof(when), "unknown");

	char head[1024];
	snprintf(head, sizeof(head),
		"\n=== physcd " VDATE " (built " __DATE__ " " __TIME__ ") | %s ===\n"
		"binary %s | core %s | pid %d\n"
		"cfg: autoboot=%u mount_delay=%u acoustic=%u log=%u\n"
		"cfg: audio_core=%s vcd_core=%s cdg_core=%s\n",
		when, base, is_menu() ? "MENU" : user_io_get_core_name(), (int)getpid(),
		cfg.physcd_autoboot, cfg.physcd_mount_delay, cfg.physcd_acoustic, cfg.physcd_log,
		cfg.physcd_audio_core, cfg.physcd_vcd_core, cfg.physcd_cdg_core);
	log_emit(head);
}
