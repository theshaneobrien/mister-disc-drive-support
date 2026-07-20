/*
 * physcd_autoboot.cpp - insert a disc, get the game.
 * see physcd_autoboot.h for why this is split across two processes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../user_io.h"
#include "../../menu.h"
#include "../../osd.h"
#include "../../fpga_io.h"
#include "../../file_io.h"
#include "../../hardware.h"
#include "../../cfg.h"
#include "../../bootcore.h"
#include "../megacd/megacd.h"
#include "../psx/psx.h"
#include "mister_physcd.h"
#include "physcd_autoboot.h"

/* survives the exec, which is the whole point - see the header */
#define MARKER "/tmp/physcd_autoboot"

/*
 * disc type -> core name for findCore.
 *
 * this is the rbf BASENAME and findCore is CASE-SENSITIVE, so it must
 * not be the uppercase string the is_*() helpers compare against:
 * "MEGACD" finds nothing, "MegaCD" finds MegaCD_20240101.rbf.
 */
static const char *core_name_for(physcd_disc_t t)
{
	switch (t) {
	case PHYSCD_DISC_MEGACD: return "MegaCD";
	case PHYSCD_DISC_PSX:    return "PSX";
	case PHYSCD_DISC_SATURN: return "Saturn";
	case PHYSCD_DISC_PCECD:  return "TurboGrafx16";
	case PHYSCD_DISC_NEOGEO: return "NeoGeo";
	default:                 return NULL;
	}
}

/* is the core running right now the one this disc wants? */
static int core_matches(physcd_disc_t t)
{
	switch (t) {
	case PHYSCD_DISC_MEGACD: return is_megacd();
	case PHYSCD_DISC_PSX:    return is_psx();
	case PHYSCD_DISC_SATURN: return is_saturn();
	case PHYSCD_DISC_PCECD:  return is_pce();
	case PHYSCD_DISC_NEOGEO: return is_neogeo_cd();
	default:                 return 0;
	}
}

int physcd_mount_current_core(void)
{
	if (is_megacd())
	{
		mcd_set_image(0, PHYSCD_SENTINEL);
		return 1;
	}
	if (is_psx())
	{
		// f_index/s_index as the menu uses for the cd slot
		psx_mount_cd(1, 1, PHYSCD_SENTINEL);
		return 1;
	}

	printf("physcd: core '%s' has no physical disc support yet\n", user_io_get_core_name());
	return 0;
}

// ------------------------------------------------------ phase B: new core

void physcd_autoboot_startup(void)
{
	int want = 0;

	if (!cfg.physcd_autoboot) return;

	FILE *f = fopen(MARKER, "r");
	if (!f)
	{
		/* no marker. at the MENU that means "start watching for a
		   disc" - and it is done HERE, at startup, deliberately:
		   physcd_open scans and ioctls up to eight devices and can
		   block for seconds on a spinning-up drive, which is fine
		   during boot but would freeze the ui if the menu tick did
		   it. after this the tick only polls an event, in O(1). */
		if (is_menu()) physcd_watch_start();
		return;                      /* a core loaded by hand: leave it alone */
	}

	if (fscanf(f, "%d", &want) != 1) want = 0;
	fclose(f);
	unlink(MARKER);                  /* strictly one-shot, even on error */

	if (want <= 0) return;

	/* the marker carries the DISC TYPE, not a core name: phase A only
	   knows which rbf it loaded, never what the fpga will report as
	   its core name, but every core can answer "is this disc mine?" */
	if (!core_matches((physcd_disc_t)want))
	{
		printf("physcd: autoboot marker was for a %s disc, this is '%s' - ignoring\n",
			physcd_disc_name((physcd_disc_t)want), user_io_get_core_name());
		return;
	}
	const char *want_name = physcd_disc_name((physcd_disc_t)want);

	/* "user_io_init returned" is HPS-side readiness only and says
	   nothing about the core's own cpu/bios being up. there is no
	   handshake to wait on - MGL exists precisely because cores need
	   wall-clock time after this point - so dwell, then mount. */
	int delay = cfg.physcd_mount_delay ? cfg.physcd_mount_delay : 2;
	printf("physcd: autoboot - waiting %ds for the %s core to settle\n", delay, want_name);
	sleep(delay);

	if (!physcd_mount_current_core()) return;

	/* Info() works here: no menu is open in a freshly loaded core */
	char msg[128];
	const char *id = is_psx() ? psx_get_game_id() : NULL;
	if (id && *id) snprintf(msg, sizeof(msg), "Disc mounted\n%s", id);
	else snprintf(msg, sizeof(msg), "Disc mounted");
	Info(msg, 3000);
}

// ------------------------------------------------------ phase A: the menu

enum { AB_IDLE = 0, AB_BANNER, AB_LOADING, AB_FAILED };

static int ab_state = AB_IDLE;
static int ab_ticks = 0;
static physcd_disc_t ab_type = PHYSCD_DISC_NONE;
static physcd_region_t ab_region = PHYSCD_REGION_UNKNOWN;
static char ab_rbf[1024] = {};
static int ab_latched = 0;      /* do not retry this disc forever */

int physcd_autoboot_busy(void)
{
	return ab_state == AB_BANNER || ab_state == AB_LOADING;
}

static void banner(const char *l1, const char *l2)
{
	char s[64];
	OsdWrite(12, "\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81");
	snprintf(s, sizeof(s), " %s", l1);
	OsdWrite(13, s, 0, 0);
	snprintf(s, sizeof(s), " %s", l2 ? l2 : "");
	OsdWrite(14, s, 0, 0);
	OsdWrite(15, "", 0, 0);
}

int physcd_autoboot_menu_tick(void)
{
	if (!cfg.physcd_autoboot) return 0;

	/* bootcore is counting down and owns these very osd lines - do not
	   fight it, and do not steal a boot the user configured */
	if (cfg.bootcore[0] != '\0' && btimeout > 0) return 0;

	/* watching is started once at boot (see physcd_autoboot_startup):
	   opening the drive blocks, and this tick runs on the ui side */
	if (!physcd_watching()) return 0;

	physcd_disc_t type = PHYSCD_DISC_NONE;
	physcd_region_t region = PHYSCD_REGION_UNKNOWN;
	physcd_event_t ev = physcd_poll_event(&type, &region);

	if (ev == PHYSCD_EV_DISC_OUT)
	{
		ab_latched = 0;              /* new media clears the latch */
		if (ab_state != AB_LOADING) ab_state = AB_IDLE;
		return 0;
	}

	if (ev == PHYSCD_EV_DISC_IN && !ab_latched && ab_state == AB_IDLE)
	{
		ab_type = type;
		ab_region = region;
		ab_ticks = 0;

		const char *core = core_name_for(type);
		if (!core)
		{
			ab_state = AB_FAILED;
			ab_latched = 1;
			banner(physcd_disc_name(type), "No core for this disc");
			return 1;
		}

		if (!find_core_rbf(core, ab_rbf, sizeof(ab_rbf)))
		{
			ab_state = AB_FAILED;
			ab_latched = 1;
			printf("physcd: no rbf found for core '%s'\n", core);
			banner(physcd_disc_name(type), "Core not installed");
			return 1;
		}

		ab_state = AB_BANNER;
	}

	switch (ab_state)
	{
	case AB_BANNER:
	{
		/* the banner MUST get screen time before the load: the very
		   first thing fpga_load_rbf does is OsdDisable(), and then it
		   execs - so a message drawn in the same tick is never seen. */
		char l2[64];
		const char *rn = physcd_region_name(ab_region);
		snprintf(l2, sizeof(l2), "Loading%s%s...", *rn ? " " : "", *rn ? rn : "");
		banner(physcd_disc_name(ab_type), l2);

		if (++ab_ticks >= 15)        /* ~1.5s at the 100ms tick */
		{
			ab_state = AB_LOADING;

			/* hand off to the process that replaces us */
			FILE *f = fopen(MARKER, "w");
			if (f) { fprintf(f, "%d\n", (int)ab_type); fclose(f); }

			/* release the drive before the exec, or the prefetch
			   thread can be mid-SG_IO when the process is torn down
			   and leave the device busy for our successor */
			physcd_watch_stop();

			printf("physcd: autoboot loading %s\n", ab_rbf);
			fpga_load_rbf(ab_rbf);   /* does not return */
		}
		return 1;
	}

	case AB_FAILED:
		if (++ab_ticks >= 30) { ab_state = AB_IDLE; ab_ticks = 0; }
		return 1;

	default:
		return 0;
	}
}
