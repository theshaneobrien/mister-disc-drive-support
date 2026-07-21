#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "../../file_io.h"
#include "../../user_io.h"
#include "../../spi.h"
#include "../../hardware.h"
#include "../../menu.h"
#include "../../cheats.h"
#include "saturn.h"
#include "../physcd/mister_physcd.h"

static int need_reset = 0;
uint32_t saturn_frame_cnt = 0;
uint8_t time_mode;

static uint32_t CalcTimerOffset(uint8_t speed) {
	static uint8_t adj0 = 0x1, adj1 = 0x1, adj2 = 0x1;

	uint32_t offs;
	if (speed == 2) {
		offs = 6 + ((adj2 & 0x03) != 0x00 ? 1 : 0);	//6.6
		adj2 <<= 1;
		if (adj2 >= 0x08) adj2 = 0x01;
		adj0 = adj1 = 0x1;
	}
	else if (speed == 1) {
		offs = 13 + ((adj1 & 0x01) != 0x00 ? 1 : 0);	//13.3
		adj1 <<= 1;
		if (adj1 >= 0x08) adj1 = 0x01;
		adj0 = adj2 = 0x1;
	}
	else {
		offs = 16 + ((adj0 & 0x03) != 0x00 ? 1 : 0);	//16.7
		adj0 <<= 1;
		if (adj0 >= 0x08) adj0 = 0x01;
		adj1 = adj2 = 0x1;
	}
	return offs;
}

void saturn_poll()
{
	static unsigned long poll_timer = 0;
	static uint8_t last_req = 255;

	/* a physically-swapped disc: adopt its toc (cached, no drive read), show
	   the guest a real lid-open dwell, then close - it then gets STOP with the
	   new toc and re-scans. (a bare STOP was hardware-proven insufficient.) */
	static uint32_t swap_close_at = 0;
	if (satcdd.is_phys() && physcd_swap_consume() && satcdd.SwapPhys())
	{
		/* every mount - including the proven no-reset OSD swap - pushes the
		   disc's ip.bin boot header to the core (BOOT_IO_INDEX); the bios
		   disc-change validation consults it, and a stale header reads as
		   "wrong disc" and dumps the guest into the cd player (hardware-
		   observed on Policenauts). one bounded sector read, during the lid
		   dwell so the header is in place before the guest sees STOP. */
		static uint8_t hdr[256];
		if (satcdd.GetBootHeader(hdr) > 0)
			saturn_send_data(hdr, 256, BOOT_IO_INDEX);
		swap_close_at = GetTimer(PHYSCD_SWAP_DWELL_MS);
	}
	if (satcdd.is_phys() && swap_close_at && CheckTimer(swap_close_at))
	{
		swap_close_at = 0;
		satcdd.SwapClose();
	}

	if (!poll_timer || CheckTimer(poll_timer))
	{
		poll_timer = GetTimer(0);

		uint16_t data_in[6];
		uint8_t req = spi_uio_cmd_cont(UIO_CD_GET);
		if (req != last_req)
		{
			last_req = req;

			for (int i = 0; i < 6; i++) data_in[i] = spi_w(0);
			DisableIO();

			satcdd.SetCommand((uint8_t*)data_in);
			satcdd.CommandExec();
		}
		else
			DisableIO();

		satcdd.Process(&time_mode);
		poll_timer += CalcTimerOffset(time_mode);

		uint16_t* s = (uint16_t*)satcdd.GetStatus();
		spi_uio_cmd_cont(UIO_CD_SET);
		for (int i = 0; i < 6; i++) spi_w(s[i]);
		DisableIO();

		satcdd.Update();
		saturn_frame_cnt++;

		unsigned long curr_timer = GetTimer(0);
		if (curr_timer >= poll_timer) {
			poll_timer = curr_timer + CalcTimerOffset(time_mode);
#ifdef SATURN_DEBUG
			user_io_status_set("[63]", 1); 
			printf("\x1b[32mSaturn: ");
			printf("Time over: next = %lu, curr = %lu", poll_timer, curr_timer);
			printf("\n\x1b[0m");
#endif // SATURN_DEBUG
		}
#ifdef SATURN_DEBUG
		else 
			user_io_status_set("[63]", 0);
#endif // SATURN_DEBUG
	}
}

static char buf[1024];

static void saturn_get_save_without_disk(char *buf)
{
	char *p1, *p2;

	if ((p1 = strstr(buf, "disc")) != 0 || (p1 = strstr(buf, "Disc")) != 0 || (p1 = strstr(buf, "DISC")) != 0)
	{
		p2 = p1 + 4;

		if (p1 > buf && *(--p1) == '(') p1--;
		if (p1 > buf && *(--p1) == ' ') p1--;
		if (*p2 == ' ') p2++;
		if ((*p2 >= '0' && *p2 <= '9') || (*p2 >= 'a' && *p2 <= 'f') || (*p2 >= 'A' && *p2 <= 'F')) {
			p2++;
			if (*p2 == ')') p2++;
			p1++;
			strcpy(p1, p2);
		}
	}
}

void saturn_mount_save(const char *filename, bool is_auto)
{
	user_io_set_index(SAVE_IO_INDEX);
	user_io_set_download(1);
	if (strlen(filename))
	{
		if (is_auto)
		{
			FileGenerateSavePath(filename, buf);
			saturn_get_save_without_disk(buf);
		} else {
			strncpy(buf, filename, sizeof(buf));
		}
#ifdef SATURN_DEBUG
		printf("Saturn save filename = %s\n", buf);
#endif // SATURN_DEBUG
		user_io_file_mount(buf, 1, 1);
	}
	else
	{
		user_io_file_mount("", 1);
	}
	user_io_set_download(0);
}

static int saturn_load_rom(const char *basename, const char *name, int sub_index)
{
	strcpy(buf, basename);
	char *p = strrchr(buf, '/');
	if (p)
	{
		p++;
		strcpy(p, name);
		if (user_io_file_tx(buf, sub_index << 6)) return 1;
	}

	return 0;
}

int saturn_set_image(int num, const char *filename)
{
	static char last_dir[1024] = {};

	(void)num;

	int reset_after_insert_disc = !user_io_status_get("[4]");
	int phys = !strcmp(filename, PHYSCD_SENTINEL);
	int mounted = 0;

	satcdd.Unload();
	satcdd.Reset();
	physcd_swap_enable(0);              // any remount/unmount disarms swap detection

	int same_game = *filename && *last_dir && !strncmp(last_dir, filename, strlen(last_dir));
	/* the phys sentinel is one fixed string, so a remount always looks
	   like the same game and would skip the bios load + reset (and a
	   failed mount could never recover). a physical mount is a fresh
	   disc: re-init every time, regardless of the reset-on-insert
	   toggle [4] (which would otherwise leave a physical mount with no
	   bios). */
	if (phys) { same_game = 0; reset_after_insert_disc = 1; }
	strcpy(last_dir, filename);
	char *p = strrchr(last_dir, '/');
	if (p) *p = 0;

	if (!same_game && reset_after_insert_disc)
	{
		saturn_mount_save("", true);

		user_io_status_set("[0]", 1);
		saturn_reset();

		// load CD BIOS. NOTE saturn region is NOT bios-based: the core
		// has one boot.rom and its own region option (set it to Auto in
		// the core OSD and it reads the region off the disc). so there
		// is deliberately no per-region bios matching here, unlike psx.
		int bios_loaded = 0;
		if (!phys) // per-game bios lives next to the image, no folder for a physical disc
		{
			bios_loaded = saturn_load_rom(filename, "cd_bios.rom", 0)    // from disk folder
				|| saturn_load_rom(last_dir, "cd_bios.rom", 0);         // from parent folder
		}
		if (!bios_loaded)
		{
			sprintf(buf, "%s/boot.rom", HomeDir()); // from home folder
			if (!user_io_file_tx(buf))
			{
				Info("CD BIOS not found!", 4000);
			}
		}
	}

	satcdd.wwf_hack = false;
	satcdd.roadrash_hack = false;
	if (strlen(filename))
	{
		if (satcdd.Load(filename) > 0)
		{
			mounted = 1;
			satcdd.SendData = saturn_send_data;
			if (phys) physcd_swap_enable(1);   // arm mid-mount physical disc-swap detection

			if (!same_game && reset_after_insert_disc)
			{
				//saturn_load_rom(filename, "cart.rom", 1);
				// physical disc has no game folder: fixed save name
				saturn_mount_save(phys ? "physcd" : filename, true);
				//cheats_init(filename, 0);
			}

			if (satcdd.GetBootHeader((uint8_t*)buf) > 0)
			{
				saturn_send_data((uint8_t*)buf, 256, BOOT_IO_INDEX);

				char *id = buf + 0x20;
				if (!strncmp(id,"T-8126H",7) ||
					!strncmp(id, "T-8120G", 7) ||
					!strncmp(id, "T-8112H", 7) ||
					!strncmp(id, "T-99901G", 8) ||
					!strncmp(id, "T-8112G", 7)) satcdd.wwf_hack = true;
				if (satcdd.wwf_hack) {
#ifdef SATURN_DEBUG
					printf("\x1b[32mSaturn: WWF games hack!!!\n\x1b[0m");
#endif // SATURN_DEBUG
				}

				if (!strncmp(id, "T-5008H", 7) ||
					!strncmp(id, "T-10609G", 8)) satcdd.roadrash_hack = true;
				if (satcdd.roadrash_hack) {
#ifdef SATURN_DEBUG
					printf("\x1b[32mSaturn: Road Rash games hack!!!\n\x1b[0m");
#endif // SATURN_DEBUG
				}
			}
		}
	}

	user_io_status_set("[0]", 0);

	// autoboot needs to know a disc actually mounted, not just that the
	// core was recognised
	return mounted;
}

void saturn_reset() {
	need_reset = 1;
}

int saturn_send_data(uint8_t* buf, int len, uint8_t index) {
	// set index byte
	user_io_set_index(index);

	user_io_set_download(1);
	user_io_file_tx_data(buf, len);
	user_io_set_download(0);
	return 1;
}

static char save_blank[] = {
	0x00, 0x42, 0x00, 0x61, 0x00, 0x63, 0x00, 0x6B, 0x00, 0x55, 0x00, 0x70, 0x00, 0x52, 0x00, 0x61,
	0x00, 0x6D, 0x00, 0x20, 0x00, 0x46, 0x00, 0x6F, 0x00, 0x72, 0x00, 0x6D, 0x00, 0x61, 0x00, 0x74,
	0x00, 0x42, 0x00, 0x61, 0x00, 0x63, 0x00, 0x6B, 0x00, 0x55, 0x00, 0x70, 0x00, 0x52, 0x00, 0x61,
	0x00, 0x6D, 0x00, 0x20, 0x00, 0x46, 0x00, 0x6F, 0x00, 0x72, 0x00, 0x6D, 0x00, 0x61, 0x00, 0x74,
	0x00, 0x42, 0x00, 0x61, 0x00, 0x63, 0x00, 0x6B, 0x00, 0x55, 0x00, 0x70, 0x00, 0x52, 0x00, 0x61,
	0x00, 0x6D, 0x00, 0x20, 0x00, 0x46, 0x00, 0x6F, 0x00, 0x72, 0x00, 0x6D, 0x00, 0x61, 0x00, 0x74,
	0x00, 0x42, 0x00, 0x61, 0x00, 0x63, 0x00, 0x6B, 0x00, 0x55, 0x00, 0x70, 0x00, 0x52, 0x00, 0x61,
	0x00, 0x6D, 0x00, 0x20, 0x00, 0x46, 0x00, 0x6F, 0x00, 0x72, 0x00, 0x6D, 0x00, 0x61, 0x00, 0x74,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

void saturn_fill_blanksave(uint8_t *buffer, uint32_t lba)
{
	if (lba == 0 || lba == 128)
	{
		memcpy(buffer, save_blank, 512);
	}
	else
	{
		memset(buffer, 0, 512);
	}
}
