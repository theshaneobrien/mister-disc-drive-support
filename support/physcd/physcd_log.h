#ifndef PHYSCD_LOG_INCLUDED
#define PHYSCD_LOG_INCLUDED

#include <stdint.h>

/*
 * physcd_log - the support log.
 *
 * a user who hits a problem has, until now, had nothing to send: every
 * printf in this fork goes to stdout, and cfg.cpp points stdout at
 * /dev/null unless DEBUG= is set in the ini. the one structured artifact
 * (/tmp/physcd_stats.log) is a 5-second snapshot on a tmpfs, so it is
 * gone by the time anyone thinks to look. this writes ONE plain-text file
 * next to the binary on the card, which survives reboots and can be
 * attached to an issue as-is.
 *
 * rules this file exists under, all learned the hard way:
 *  - NOTHING here may be called from the fpga-answering thread's per-sector
 *    path. every call site is a session boundary (startup, drive open, disc
 *    identify, toc load, mount, ra hash, core exit) on a thread where
 *    blocking is already free. the per-sector facts reach the log as
 *    COUNTERS, never as lines.
 *  - it is silent: no osd, no popups, no serial spam. the file is the
 *    entire ui.
 *  - it must never fill the card: the file is capped and truncated to its
 *    tail, and a failed write is dropped rather than retried.
 */

// level from the ini (PHYSCD_LOG). 0 = off, 1 = the support log.
// 2 and 3 are reserved for future verbosity; they read as 1 today.
void physcd_log_config(int level);
int  physcd_log_level(void);

// one line, printf style. newline is added. no-op at level 0.
__attribute__((format(printf, 1, 2)))
void physcd_log(const char *fmt, ...);

// a pre-formatted multi-line block (caller supplies the newlines) written in
// ONE append+fsync. use this for anything that would otherwise be a run of
// physcd_log() calls: a per-line fsync run is what turns a disc swap into a
// stall, since load_toc emits while pcd.io is held.
void physcd_log_block(const char *text);

// session header: build stamp, which binary is running, and the ini
// settings actually in effect. call once per process, after cfg_parse.
void physcd_log_startup(void);

// cumulative-per-mount counters (cache, integrity, stalls, subchannel).
// called at core exit and after an ra hash, never on a timer.
// implemented in mister_physcd.cpp, where the counters live.
void physcd_log_counters(void);

// zero-filled and edc-failed sector totals, for before/after snapshots
// around a read whose integrity matters (the ra hash). also in
// mister_physcd.cpp.
void physcd_integrity_snapshot(uint32_t *bad, uint32_t *edc_bad);

#endif
