/*
 * physcd_autoboot.cpp - insert a disc, get the game.
 * see physcd_autoboot.h for why this is split across two processes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "../../user_io.h"
#include "../../menu.h"
#include "../../osd.h"
#include "../../fpga_io.h"
#include "../../file_io.h"
#include "../../hardware.h"
#include "../../cfg.h"
#include "../../bootcore.h"
#include "../arcade/mra_loader.h"
#include "../megacd/megacd.h"
#include "../psx/psx.h"
#include "../saturn/saturn.h"
#include "../neogeo/neogeocd.h"
#include "../3do/3do.h"
#include "../pcecd/pcecd.h"
#include "../cdi/cdi.h"
#include "mister_physcd.h"
#include "physcd_autoboot.h"
#include "physcd_acoustic.h"

#ifdef HAS_RCHEEVOS
// v2 (RA build): identify a physical disc for RetroAchievements and prefer
// the RA-patched core. all of this compiles out on the plain disc build.
#include "../../achievements.h"
extern const char *getRootDir();
#endif

/* both survive the exec, which is the whole point - see the header */
#define MARKER "/tmp/physcd_autoboot"   /* phase A -> phase B, one-shot   */
#define BOOTED "/tmp/physcd_booted"     /* what we last booted from disc  */
#define TIPFILE "config/physcd_sidecar_tip"  /* sd card: once-EVER, never nags */

static unsigned long pending_mount = 0;
static physcd_disc_t pending_type = PHYSCD_DISC_NONE;
static unsigned long sidecar_mount = 0;   /* side-by-side automount dwell */

/* shared PHYSCD_*_CORE ini-value parser: which mountable console a
   config string names. an empty or unknown value falls back to dflt. */
static physcd_disc_t parse_console(const char *c, physcd_disc_t dflt)
{
	if (!c || !*c)                       return dflt;
	if (!strcasecmp(c, "MegaCD"))        return PHYSCD_DISC_MEGACD;
	if (!strcasecmp(c, "Saturn"))        return PHYSCD_DISC_SATURN;
	if (!strcasecmp(c, "PSX") ||
	    !strcasecmp(c, "PlayStation"))   return PHYSCD_DISC_PSX;
	if (!strcasecmp(c, "NeoGeo") ||
	    !strcasecmp(c, "NeoGeoCD"))      return PHYSCD_DISC_NEOGEO;
	if (!strcasecmp(c, "3DO"))           return PHYSCD_DISC_3DO;
	if (!strcasecmp(c, "TurboGrafx16") ||
	    !strcasecmp(c, "PCECD") ||
	    !strcasecmp(c, "PCE"))           return PHYSCD_DISC_PCECD;
	if (!strcasecmp(c, "CD-i") ||
	    !strcasecmp(c, "CDI") ||
	    !strcasecmp(c, "CDi"))           return PHYSCD_DISC_CDI;
	return dflt;
}

/* which console's bios cd player an audio cd boots into. PHYSCD_AUDIO_CORE
   (default PSX) lets people pick - PSX is the verified one (its bios cd
   player is confirmed working); the others are plumbed but their bios cd
   player is a hardware-verify. */
static physcd_disc_t physcd_audio_console(void)
{
	return parse_console(cfg.physcd_audio_core, PHYSCD_DISC_PSX);
}

/* which core a Video CD (CD-Bridge disc) boots into. PHYSCD_VCD_CORE,
   default CD-i - the stock CDi core emulates the cd-i digital-video (mpeg)
   hardware and plays them (hardware-confirmed). overridable so people can
   point a vcd at another core to experiment (a real saturn/3do/cd32 played
   vcds too, but only with an mpeg add-on those cores do not model). */
static physcd_disc_t physcd_vcd_console(void)
{
	return parse_console(cfg.physcd_vcd_core, PHYSCD_DISC_CDI);
}

/* which core a cd+g karaoke disc (audio cd with graphics in the R-W
   subchannel) boots into. PHYSCD_CDG_CORE, default MegaCD - MegaCD and
   TurboGrafx16 draw the graphics through their bios cd player straight
   off a physical disc (hardware-confirmed). CD-i's player does cd+g too
   but only on a core built after 2026-05-24; older released cd-i cores
   show a blank graphics screen. any other console just plays the audio. */
static physcd_disc_t physcd_cdg_console(void)
{
	return parse_console(cfg.physcd_cdg_core, PHYSCD_DISC_MEGACD);
}

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
	case PHYSCD_DISC_3DO:    return "3DO";
	// rbf basename is "CDi" (no hyphen) for findCore; the core reports "CD-i"
	// INTERNALLY, which is what is_cdi()/core_matches compare against
	case PHYSCD_DISC_CDI:    return "CDi";
	// an audio cd boots a console's bios cd player - the mister as a cd
	// player, just like the real thing. which console is configurable.
	case PHYSCD_DISC_AUDIO:  return core_name_for(physcd_audio_console());
	// a video cd boots the core that can play it (CD-i by default,
	// configurable via PHYSCD_VCD_CORE)
	case PHYSCD_DISC_VCD:    return core_name_for(physcd_vcd_console());
	// a cd+g karaoke disc boots the core that draws the graphics
	// (CD-i by default, configurable via PHYSCD_CDG_CORE)
	case PHYSCD_DISC_CDG:    return core_name_for(physcd_cdg_console());
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
	// is_neogeo(), NOT is_neogeo_cd(): a freshly loaded NeoGeo core is
	// in cart mode; the mount enables cd mode
	case PHYSCD_DISC_NEOGEO: return is_neogeo();
	case PHYSCD_DISC_3DO:    return is_3do();
	case PHYSCD_DISC_CDI:    return is_cdi();
	case PHYSCD_DISC_AUDIO:  return core_matches(physcd_audio_console());
	case PHYSCD_DISC_VCD:    return core_matches(physcd_vcd_console());
	case PHYSCD_DISC_CDG:    return core_matches(physcd_cdg_console());
	default:                 return 0;
	}
}

/* can we actually MOUNT this disc type? core_name_for knows about
   cores we can boot to, but booting to a core that then cannot mount
   just dumps the user in a bare bios with no disc and no explanation */
static int mountable(physcd_disc_t t)
{
	return t == PHYSCD_DISC_MEGACD || t == PHYSCD_DISC_PSX
		|| t == PHYSCD_DISC_SATURN || t == PHYSCD_DISC_NEOGEO
		|| t == PHYSCD_DISC_3DO || t == PHYSCD_DISC_PCECD
		|| t == PHYSCD_DISC_CDI || t == PHYSCD_DISC_AUDIO
		|| t == PHYSCD_DISC_VCD || t == PHYSCD_DISC_CDG;
}

/* resolve the rbf for a core. on the RA build, PREFER the RA-patched core
   from _RA_Cores/Cores/<Core>.rbf: a stock core publishes no ram mirror,
   so achievements would stay silent. the _RA_Cores basenames match
   core_name_for() (MegaCD, PSX, Saturn, NeoGeo, TurboGrafx16). falls back
   to the normal newest-rbf scan. on the plain disc build this is just
   find_core_rbf. an absolute path (getRootDir is absolute) loads verbatim. */
static int physcd_find_core_rbf(const char *core, char *out, int outsz)
{
#ifdef HAS_RCHEEVOS
	char ra[1024];
	snprintf(ra, sizeof(ra), "%s/_RA_Cores/Cores/%s.rbf", getRootDir(), core);
	if (FileExists(ra)) { snprintf(out, outsz, "%s", ra); return 1; }
#endif
	return find_core_rbf(core, out, outsz);
}

int physcd_mount_current_core(void)
{
	// returns whether a disc really mounted, not merely whether the
	// core was recognised - autoboot reports success from this
	int recognised = 1;
	int mounted = 0;

	if (is_megacd()) mounted = mcd_set_image(0, PHYSCD_SENTINEL);
	// f_index/s_index as the menu uses for the cd slot
	else if (is_psx()) mounted = psx_mount_cd(1, 1, PHYSCD_SENTINEL);
	else if (is_saturn()) mounted = saturn_set_image(0, PHYSCD_SENTINEL);
	// NeoGeo core does both cart and CD: switch it to CD mode, then the
	// game streams off the disc via the shared (megacd) cdd
	else if (is_neogeo())
	{
		neocd_set_en(1);
		mounted = neocd_set_image(PHYSCD_SENTINEL);
	}
	else if (is_3do()) mounted = p3do_set_image(0, PHYSCD_SENTINEL);
	// pcecd_set_image is void; it sets pcecdd.loaded on a good mount
	else if (is_pce()) { pcecd_set_image(0, PHYSCD_SENTINEL); mounted = pcecdd.loaded; }
	// cd-i CD is disk slot 0; cdi_mount_cd now returns whether it mounted
	else if (is_cdi()) mounted = cdi_mount_cd(0, PHYSCD_SENTINEL);
	else recognised = 0;

	if (!recognised)
	{
		printf("physcd: core '%s' has no physical disc support yet\n", user_io_get_core_name());
		return 0;
	}

#ifdef HAS_RCHEEVOS
	// v2: identify the just-mounted physical disc for RetroAchievements. the
	// hasher reads through our cdreader (ra_cdreader_chd.cpp) since there is
	// no image file. harmless if RA is off or the core is stock -
	// achievements_load_game no-ops without an active handler. 3DO has no RA
	// core, so it simply finds no handler.
	if (mounted > 0) achievements_load_game(PHYSCD_SENTINEL, 0);
#endif

	return mounted;
}

int physcd_swap_current_core(void)
{
	// manual fallback for the swap_phys fifo: re-read the disc now in the
	// drive and hand it to the running game as a swap (no reset). every
	// swap-capable core auto-detects a physical swap in its poll; only PSX
	// is also wired to this manual command.
	if (is_psx()) { psx_swap_disc(); return 1; }

	printf("physcd: swap_phys is a PSX-only manual path; '%s' auto-detects physical swaps if it supports them\n", user_io_get_core_name());
	return 0;
}

// ------------------------------------------------------ phase B: new core

/* ---------------------------------------------------- side-by-side mode
 *
 * installed as /media/fat/MiSTer_Disc (or dropped over MiSTer_RA), the
 * binary is reached through stock MiSTer's per-core main= routing (a
 * [CD-*] section, or the RA_* routing that already exists on odelot
 * setups) instead of replacing /media/fat/MiSTer. the stock menu runs the
 * menu, so none of the menu-side disc logic exists - a routed core launch
 * has to mount the physical disc itself. that is this feature. a full
 * install never takes this path: its basename IS "MiSTer", and the menu
 * owns disc logic there exactly as before.
 */

static int routed_main(void)
{
	const char *app = getappname();          /* /proc/self/exe, absolute */
	if (!app || !*app) return 0;             /* readlink failed: fail CLOSED
	                                            (full-install behavior) */
	const char *base = strrchr(app, '/');
	base = base ? base + 1 : app;
	/* a binary replaced on disk mid-session reads "MiSTer (deleted)":
	   compare the name only, up to the first space */
	size_t n = strcspn(base, " ");
	return !(n == 6 && !strncmp(base, "MiSTer", 6));
}

static void sidecar_arm(void)
{
	if (!routed_main()) return;           /* full install: menu owns discs */
	if (!cfg.physcd_autoboot) return;     /* manual-only by choice; the
	                                         mount_phys fifo still works  */

	/* the same seven cores physcd_mount_current_core can serve */
	if (!(is_megacd() || is_psx() || is_saturn() || is_neogeo()
	      || is_3do() || is_pce() || is_cdi())) return;

	/* a game MGL: the user picked a FILE, do not mount over it. the
	   items have not loaded yet at startup time (they run on delays in
	   the menu state machine), so ask the parsed list, not the cores. */
	mgl_struct *mgl = mgl_get();
	if (mgl) for (int i = 0; i < mgl->count; i++)
		if (mgl->item[i].action == MGL_ACTION_LOAD) return;

	/* no cd hardware at all: stay out of the way, zero cost */
	int have = 0;
	for (int i = 0; i < 8 && !have; i++)
	{
		char dev[16];
		snprintf(dev, sizeof(dev), "/dev/sr%d", i);
		if (!access(dev, R_OK)) have = 1;
	}
	if (!have) return;

	/* open now - this can block seconds on a spinning-up drive, which is
	   fine at startup (same reasoning as the menu watcher's open below).
	   the PRESENCE decision waits for the poll: the settle dwell doubles
	   as cold-drive wake time, and presence at mount time is the truth
	   that matters. */
	physcd_open(NULL);
	sidecar_mount = GetTimer(cfg.physcd_mount_delay * 1000);
	printf("physcd: routed main (%s) - checking the drive in %ds\n",
		getappname(), cfg.physcd_mount_delay);
}

void physcd_autoboot_startup(void)
{
	int want = 0;

	// acoustic seek: start its background thread if the ini asks.
	// runs once per process; harmless when off.
	physcd_acoustic_config(cfg.physcd_acoustic);

	// persistent udev rule so boot coldplug never blkid-grinds a cd drive
	// (must be on disk BEFORE a boot to help that boot; installing at every
	// start is idempotent and survives linux image updates)
	physcd_quiet_udev();

	/* NB: not gated on cfg.physcd_autoboot. autoboot=0 means "do not
	   AUTO-load on insert", not "ignore the drive": the menu still
	   watches so the Load Disc row can appear, and a marker written by
	   that row must still be honoured here. only the auto-load trigger
	   in the menu tick checks the flag. */
	FILE *f = fopen(MARKER, "r");
	if (!f)
	{
		/* no marker. at the MENU that means "start watching for a
		   disc" - and it is done HERE, at startup, deliberately:
		   physcd_open scans and ioctls up to eight devices and can
		   block for seconds on a spinning-up drive, which is fine
		   during boot but would freeze the ui if the menu tick did
		   it. after this the tick only polls an event, in O(1). */
		if (is_menu()) { physcd_watch_start(); return; }

		/* not the menu and no marker. on a full install that is a core
		   loaded by hand: leave it alone. as a ROUTED alternate main a
		   marker cannot exist (the stock menu ran the exec chain), so
		   this is the side-by-side launch path - automount. */
		sidecar_arm();
		return;
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
	   wall-clock time after this point - so dwell, then mount.
	   the dwell is a TIMER, not sleep(): this runs before the main
	   loop, so sleeping here would leave the fresh core with no
	   user_io_poll, no osd and no input for the duration. */
	pending_mount = GetTimer(cfg.physcd_mount_delay * 1000);
	pending_type = (physcd_disc_t)want;
	printf("physcd: autoboot - mounting the %s disc in %ds\n",
		want_name, cfg.physcd_mount_delay);
}

void physcd_autoboot_poll(void)
{
	/* side-by-side automount (armed by sidecar_arm; never set on a full
	   install). presence is decided HERE, after the dwell. */
	if (sidecar_mount && CheckTimer(sidecar_mount))
	{
		sidecar_mount = 0;

		if (!physcd_disc_present())
		{
			/* empty drive: the user launched the core for something
			   else. release the fd/prefetch thread/cache and stay
			   SILENT - no disc is not an error, and this must never
			   become a nag. */
			printf("physcd: routed automount - no disc, standing down\n");
			physcd_close();
			return;
		}

		int ok = physcd_mount_current_core();
		if (is_pce() && menu_present()) MenuHide();   /* same as marker flow */

		if (ok)
		{
			char msg[192];
			const char *id = is_psx() ? psx_get_game_id() : NULL;
			if (id && *id) snprintf(msg, sizeof(msg), "Disc mounted\n%s", id);
			else snprintf(msg, sizeof(msg), "Disc mounted");

			if (!FileExists(TIPFILE) && FileSave(TIPFILE, (void*)"1", 1))
			{
				/* once EVER, marker on the sd card - never per-boot,
				   never again. deleting the file is the only way back.
				   marker write is checked FIRST: if it cannot persist
				   (read-only card), the tip does not show at all -
				   once-ever-or-nothing, never a repeat. */
				size_t len = strlen(msg);
				snprintf(msg + len, sizeof(msg) - len,
					"\nTip: full install adds\ninsert-and-play autoboot");
				Info(msg, 7000);
			}
			else Info(msg, 3000);
		}
		else Info("Disc could not be read", 5000);
		return;
	}

	if (!pending_mount || !CheckTimer(pending_mount)) return;
	pending_mount = 0;

	int ok = physcd_mount_current_core();

	/* the TurboGrafx16 core raises the OSD itself when it boots with no
	   media yet; our direct mount bypasses the file-browser path that would
	   otherwise close it (menu.cpp IMAGE_SELECTED -> MenuHide), so the menu
	   is left sitting over the core. close it here - gated to PCE because on
	   the other cores the only way a menu is up at mount time is the USER
	   opening it during the settle dwell, and closing it out from under them
	   would be a behavior change vs the proven releases. */
	if (is_pce() && menu_present()) MenuHide();

	/* Info() works here: no menu is open in a freshly loaded core (and if
	   the user opened one during the dwell, Info harmlessly skips) */
	char msg[128];
	if (ok)
	{
		const char *id = is_psx() ? psx_get_game_id() : NULL;
		if (id && *id) snprintf(msg, sizeof(msg), "Disc mounted\n%s", id);
		else snprintf(msg, sizeof(msg), "%s disc mounted", physcd_disc_name(pending_type));

		/* remember what we booted, so returning to the menu with this
		   same disc still in the drive does not autoboot it all over
		   again and make the menu unreachable */
		FILE *b = fopen(BOOTED, "w");
		if (b) { fprintf(b, "%d\n", (int)pending_type); fclose(b); }
	}
	else
	{
		snprintf(msg, sizeof(msg), "Disc could not be read");
	}
	Info(msg, ok ? 3000 : 5000);
}

// ------------------------------------------------------ phase A: the menu

enum { AB_IDLE = 0, AB_BANNER, AB_LOADING, AB_FAILED };

static int ab_state = AB_IDLE;
static int ab_ticks = 0;
static physcd_disc_t ab_type = PHYSCD_DISC_NONE;
static physcd_region_t ab_region = PHYSCD_REGION_UNKNOWN;
static char ab_rbf[1024] = {};
static int ab_latched = 0;      /* do not retry this disc forever */

static void banner_clear(void);

/* resolve rbf, write the phase-B handoff marker, release the drive and
   load. shared by the auto-load banner and the menu "Load Disc" row.
   on success it execs and never returns; returns 0 only on failure. */
static int do_load(physcd_disc_t type, const char *rbf)
{
	FILE *f = fopen(MARKER, "w");
	if (f) { fprintf(f, "%d\n", (int)type); fclose(f); }

	/* release the drive before the exec, or the prefetch thread can be
	   mid-SG_IO when the process is torn down and leave the device
	   busy for our successor */
	physcd_watch_stop();
	printf("physcd: loading %s for %s disc\n", rbf, physcd_disc_name(type));

	/* USUALLY execs and never returns, but returns -1 when the rbf
	   cannot be opened - undo the handoff so a stale marker does not
	   hijack the next manually loaded core */
	if (fpga_load_rbf(rbf) < 0)
	{
		unlink(MARKER);
		physcd_watch_start();
		return 0;
	}
	return 1;   /* not reached on success */
}

int physcd_is_menu_row(const char *name)
{
	return name && !strcmp(name, PHYSCD_MENU_SENTINEL);
}

int physcd_menu_row(char *out, int outsz)
{
	/* the row is tied to the DRIVE, not the disc: show it whenever a
	   drive is attached, so it is a stable fixture at the bottom of the
	   list rather than something that pops in and out. no drive -> no
	   row at all. */
	if (!physcd_watching()) return 0;

	physcd_disc_t t = PHYSCD_DISC_NONE;
	char label[64];
	int have = physcd_menu_status(label, sizeof(label), &t);

	/* a mountable disc whose core is installed -> a real Play action.
	   the installed-core check matters because the browser cannot draw
	   an error popup (its menustate is above MENU_INFO), so a row that
	   then fails to load would be worse than showing "Insert Disc". */
	if (have && mountable(t))
	{
		const char *core = core_name_for(t);
		char rbf[1024];
		if (core && physcd_find_core_rbf(core, rbf, sizeof(rbf)))
		{
			/* a real title only if there is a non-blank one that is not
			   just the console name - a whitespace-only label must not
			   render "Play:  - Mega CD", and a generic "SATURN" label
			   must not render "Play: Saturn - Saturn" */
			const char *title = label;
			while (*title == ' ') title++;
			int generic = !strcasecmp(title, physcd_console_name(t))
				|| !strcasecmp(title, physcd_disc_name(t));
			if (t == PHYSCD_DISC_AUDIO)  snprintf(out, outsz, "Play Audio CD");
			else if (t == PHYSCD_DISC_CDG) snprintf(out, outsz, "Play CD+G");
			else if (*title && !generic) snprintf(out, outsz, "Play: %s - %s", title, physcd_console_name(t));
			else                         snprintf(out, outsz, "Play %s Disc", physcd_console_name(t));
			return 1;
		}
	}

	/* drive attached but nothing playable in it (empty, audio, or a core
	   we do not support / is not installed) */
	snprintf(out, outsz, "Insert Disc");
	return 1;
}

/* the menu "Load Disc" row: load + mount whatever readable disc is in
   the drive right now. execs on success, returns 0 if nothing to do. */
int physcd_autoboot_load_disc(void)
{
	physcd_disc_t t = PHYSCD_DISC_NONE;
	if (!physcd_menu_status(NULL, 0, &t)) return 0;   /* no disc */
	if (!mountable(t)) return 0;

	const char *core = core_name_for(t);
	char rbf[1024];
	if (!core || !physcd_find_core_rbf(core, rbf, sizeof(rbf)))
	{
		printf("physcd: no rbf for %s disc\n", physcd_disc_name(t));
		return 0;
	}
	return do_load(t, rbf);
}

int physcd_autoboot_busy(void)
{
	/* AB_FAILED must be included or its countdown runs at the slow
	   1000ms menu tick and a 30-tick banner sits for 30 seconds */
	return ab_state != AB_IDLE;
}

void physcd_autoboot_cancel(void)
{
	if (ab_state == AB_IDLE) return;
	printf("physcd: autoboot cancelled\n");
	ab_state = AB_IDLE;
	ab_ticks = 0;
	ab_latched = 1;              /* until this disc is ejected */
	banner_clear();
}

static void banner(const char *l1, const char *l2)
{
	char s[64];
	OsdWrite(12, "\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81");
	snprintf(s, sizeof(s), " %s", l1);
	OsdWrite(13, s, 0, 0);
	snprintf(s, sizeof(s), " %s", l2 ? l2 : "");
	OsdWrite(14, s, 0, 0);
	OsdWrite(15, "   Press any key to cancel", 0, 0);
}

/* the banner paints over the menu's own rows 12-15, so anything that
   leaves a banner state has to put them back or the stale text sits
   over the file browser until some unrelated redraw */
static void banner_clear(void)
{
	for (int i = 12; i <= 15; i++) OsdWrite(i, "", 0, 0);
}

int physcd_autoboot_menu_tick(void)
{
	/* watching normally starts at boot, but a usb drive often has not
	   enumerated by then - and the user may plug one in later. retry,
	   but gate the BLOCKING open behind a cheap stat so the ui pays
	   nothing while no drive node exists. this runs even with autoboot
	   OFF so the manual Load Disc row can still see the disc. */
	if (!physcd_watching())
	{
		static unsigned long retry = 0;
		if (retry && !CheckTimer(retry)) return 0;
		retry = GetTimer(3000);

		int any = 0;
		for (int i = 0; i < 8 && !any; i++)
		{
			char p[32];
			snprintf(p, sizeof(p), "/dev/sr%d", i);
			if (!access(p, R_OK)) any = 1;
		}
		if (!any || physcd_watch_start()) return 0;
		return 0;
	}

	/* autoboot=0 is MANUAL mode: keep watching (above) so the menu row
	   works, but never auto-load on insert */
	if (!cfg.physcd_autoboot) return 0;

	/* bootcore is counting down and owns these very osd lines - do not
	   fight it, and do not steal a boot the user configured */
	if (cfg.bootcore[0] != '\0' && btimeout > 0) return 0;

	physcd_disc_t type = PHYSCD_DISC_NONE;
	physcd_region_t region = PHYSCD_REGION_UNKNOWN;
	int initial = 0;
	physcd_event_t ev = physcd_poll_event(&type, &region, &initial);

	if (ev == PHYSCD_EV_DISC_OUT)
	{
		ab_latched = 0;              /* new media clears the latch */
		unlink(BOOTED);              /* and forgets what we booted */
		physcd_swap_happened();      /* consume+discard: the swapped disc left too */
		if (ab_state != AB_LOADING)
		{
			if (ab_state != AB_IDLE) banner_clear();
			ab_state = AB_IDLE;
		}
		return 0;
	}

	if (ev == PHYSCD_EV_DISC_IN && !ab_latched && ab_state == AB_IDLE)
	{
		/* a disc that was ALREADY in the drive when we started
		   watching, and that we booted last time, must not boot
		   again - otherwise quitting a game back to the menu
		   immediately relaunches it and the menu is unreachable */
		if (initial)
		{
			int last = 0;
			FILE *b = fopen(BOOTED, "r");
			if (b) { if (fscanf(b, "%d", &last) != 1) last = 0; fclose(b); }
			int swapped = physcd_swap_happened();
			if (last == (int)type || swapped)
			{
				/* a disc swapped in DURING the last game (vib ribbon's music
				   cd, a multi-disc game's next disc) has already been played:
				   treat it as booted and refresh the marker, or its TYPE
				   mismatch against the original game reads as a fresh insert
				   and quitting to the menu auto-launches it. eject and
				   reinsert still boots it fresh, as a new insert should. */
				if (swapped && last != (int)type)
				{
					FILE *w = fopen(BOOTED, "w");
					if (w) { fprintf(w, "%d\n", (int)type); fclose(w); }
				}
				printf("physcd: %s disc already %s, not repeating\n",
					physcd_disc_name(type),
					swapped && last != (int)type ? "played (swapped in mid-game)" : "booted");
				ab_latched = 1;
				return 0;
			}
		}

		ab_type = type;
		ab_region = region;
		ab_ticks = 0;

		const char *core = mountable(type) ? core_name_for(type) : NULL;
		if (!core)
		{
			ab_state = AB_FAILED;
			ab_latched = 1;
			/* booting a core we cannot then mount into is worse than
			   doing nothing: it strands the user in a bare bios */
			banner(physcd_disc_name(type), "Not supported yet");
			return 1;
		}

		if (!physcd_find_core_rbf(core, ab_rbf, sizeof(ab_rbf)))
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
			/* do_load execs on success; only returns on failure */
			if (!do_load(ab_type, ab_rbf))
			{
				ab_state = AB_FAILED;
				ab_ticks = 0;
				ab_latched = 1;
				banner(physcd_disc_name(ab_type), "Core load failed");
			}
		}
		return 1;
	}

	case AB_LOADING:
		/* only reachable if the exec somehow did not happen; the
		   failure path above normally moves us out of here */
		return 1;

	case AB_FAILED:
		if (++ab_ticks >= 30)    /* ~3s: busy() keeps the 100ms tick */
		{
			banner_clear();
			ab_state = AB_IDLE;
			ab_ticks = 0;
		}
		return 1;

	default:
		return 0;
	}
}
