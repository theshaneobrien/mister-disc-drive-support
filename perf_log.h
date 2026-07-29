#ifndef PERF_LOG_H
#define PERF_LOG_H

#include <inttypes.h>

// Lightweight timing telemetry for the overlay/translation PoC.
// Appends one line per event to /tmp/overlay_perf.log (tmpfs - RAM only,
// wiped on boot, zero SD wear). Always on: the whole point of the PoC is
// to see where the milliseconds go, so every path logs unconditionally,
// like a game running with an uncapped framerate counter.
//
// Timestamps are CLOCK_MONOTONIC microseconds, so deltas can be computed
// across threads (main loop vs offload worker) and across log lines.
//
//   tail -f /tmp/overlay_perf.log

uint64_t perf_now_us();

// printf-style; a "[   sec.usec] " monotonic stamp is prepended and a
// newline appended. Thread-safe (main loop + offload worker both log).
void perf_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
