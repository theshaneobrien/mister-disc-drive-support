#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

#include "perf_log.h"

#define PERF_LOG_FILE "/tmp/overlay_perf.log"

uint64_t perf_now_us()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

void perf_log(const char *fmt, ...)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static FILE *f = NULL;

	pthread_mutex_lock(&lock);

	// lazy open, kept open. append mode: a core load is an exec
	// (fpga_load_rbf -> app_restart), so each process re-opens and the
	// log survives across core switches within a boot.
	if (!f) f = fopen(PERF_LOG_FILE, "a");
	if (f)
	{
		uint64_t us = perf_now_us();
		fprintf(f, "[%7u.%06u] ", (uint32_t)(us / 1000000), (uint32_t)(us % 1000000));

		va_list args;
		va_start(args, fmt);
		vfprintf(f, fmt, args);
		va_end(args);

		fputc('\n', f);
		fflush(f); // tail -f friendly; tmpfs so flush is cheap
	}

	pthread_mutex_unlock(&lock);
}
