/*
Copyright 2005, 2006, 2007 Dennis van Weeren
Copyright 2008, 2009 Jakub Bednarski
Copyright 2012 Till Harbaum

This file is part of Minimig

Minimig is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

Minimig is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sched.h>
#include <inttypes.h>
#include <ctype.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include "menu.h"
#include "user_io.h"
#include "input.h"
#include "frame_timer.h"
#include "fpga_io.h"
#include "scheduler.h"
#include "osd.h"
#include "offload.h"
#include "achievements.h"
#include "support/physcd/physcd_autoboot.h"
#include "perf_log.h"

const char *version = "$VER:" VDATE;

// print the fault address plus pc/lr to stderr (unbuffered, survives)
// and die with the default action so the shell still reports the
// signal. pc/lr map to file:line via addr2line on the unstripped elf.
static void fault_handler(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = (ucontext_t *)ctx;
	char msg[160];
	int n = snprintf(msg, sizeof(msg),
		"\n*** %s: addr=%p pc=0x%08lx lr=0x%08lx ***\n",
		sig == SIGBUS ? "SIGBUS" : "SIGSEGV", si->si_addr,
		(unsigned long)uc->uc_mcontext.arm_pc,
		(unsigned long)uc->uc_mcontext.arm_lr);
	if (n > 0) write(2, msg, n);
	signal(sig, SIG_DFL);
	raise(sig);
}

int main(int argc, char *argv[])
{
	// line-buffer stdout even when redirected to a file: with the
	// default 4KB block buffering a crash eats every queued printf,
	// which turned a segfault hunt into archaeology during physcd
	// bring-up. costs nothing on the serial console.
	setvbuf(stdout, NULL, _IOLBF, 0);

	struct sigaction sa = {};
	sa.sa_sigaction = fault_handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);

	// Always pin main worker process to core #1 as core #0 is the
	// hardware interrupt handler in Linux.  This reduces idle latency
	// in the main loop by about 6-7x.
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(1, &set);
	sched_setaffinity(0, sizeof(set), &set);

	// overlay PoC: stamp the running build into the telemetry so "which
	// binary is actually flashed?" is always answerable from the log.
	// compile time beats a hand-bumped label - it can't go stale.
	perf_log("=== Main " VDATE " overlay-poc (built " __DATE__ " " __TIME__ ") started ===");

	offload_start();

	fpga_io_init();

	DISKLED_OFF;

	printf("\nMinimig by Dennis van Weeren");
	printf("\nARM Controller by Jakub Bednarski");
	printf("\nMiSTer code by Sorgelig\n\n");

	printf("Version %s\n\n", version + 5);

	if (argc > 1) printf("Core path: %s\n", argv[1]);
	if (argc > 2) printf("XML path: %s\n", argv[2]);

	if (!is_fpga_ready(1))
	{
		printf("\nGPI[31]==1. FPGA is uninitialized or incompatible core loaded.\n");
		printf("Quitting. Bye bye...\n");
		exit(0);
	}

	FindStorage();
	user_io_init((argc > 1) ? argv[1] : "",(argc > 2) ? argv[2] : NULL);
	achievements_init();

	// phase B of disc autoboot: if the menu process loaded us because a
	// disc was inserted, it left a marker. nothing to do otherwise, so a
	// core the user loaded by hand is never hijacked.
	physcd_autoboot_startup();

#ifdef USE_SCHEDULER
	scheduler_init();
	scheduler_run();
#else
	while (1)
	{
		if (!is_fpga_ready(1))
		{
			fpga_wait_to_reset();
		}

		user_io_poll();
		frame_timer();
		input_poll(0);
		physcd_autoboot_poll();
		HandleUI();
		OsdUpdate();
	}
#endif
	return 0;
}
