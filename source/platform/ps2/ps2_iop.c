/*
 * Copyright (C) 2026 NZ:P Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 */
// ps2_iop.c -- IOP bring-up and embedded IRX module management
//
// IOP RAM is 2 MiB, shared by the IOP kernel, every loaded module and their
// buffers. Modules are grouped by the service they provide and only groups
// the game actually uses are loaded. Every module is linked into the ELF with
// PS2BUILD's embed_irx, which generates <name>_irx / size_<name>_irx.

#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <sbv_patches.h>
// fileXioInit() is the one direct fileXio call: it installs fileXio as the
// newlib (POSIX) backend. Everything else uses POSIX calls.
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <dirent.h>
#include <delaythread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ps2sdkapi.h>

#include "ps2_iop.h"

extern void PS2_Log(const char *fmt, ...);

#define DECLARE_IRX(name) \
	extern unsigned char name##_irx[]; \
	extern unsigned int size_##name##_irx

DECLARE_IRX(iomanx);
DECLARE_IRX(filexio);
DECLARE_IRX(sio2man);
DECLARE_IRX(padman);
DECLARE_IRX(mcman);
DECLARE_IRX(mcserv);
DECLARE_IRX(usbd_mini);
DECLARE_IRX(bdm);
DECLARE_IRX(bdmfs_fatfs);
DECLARE_IRX(usbmass_bd_mini);
DECLARE_IRX(libsd);
DECLARE_IRX(nzpsnd);

typedef struct {
	const char *name;
	int group;
	unsigned char *data;
	unsigned int *size;
	const char *args;
	int args_len;
	// runtime
	int id;         // module id (<0 = failed)
	int result;     // module start result
	int loaded;
} ps2_irx_t;

// Order matters: dependencies first.
static ps2_irx_t irx_table[] = {
	{ "iomanX",          PS2_IOP_CORE,  iomanx_irx,          &size_iomanx_irx,          NULL, 0 },
	// fileXio: keep its read/write staging buffer modest (IOP RAM)
	{ "fileXio",         PS2_IOP_CORE,  filexio_irx,         &size_filexio_irx,         NULL, 0 },
	{ "sio2man",         PS2_IOP_PAD | PS2_IOP_MC, sio2man_irx, &size_sio2man_irx,     NULL, 0 },
	{ "padman",          PS2_IOP_PAD,   padman_irx,          &size_padman_irx,          NULL, 0 },
	{ "mcman",           PS2_IOP_MC,    mcman_irx,           &size_mcman_irx,           NULL, 0 },
	{ "mcserv",          PS2_IOP_MC,    mcserv_irx,          &size_mcserv_irx,          NULL, 0 },
	{ "usbd_mini",       PS2_IOP_USB,   usbd_mini_irx,       &size_usbd_mini_irx,       NULL, 0 },
	{ "bdm",             PS2_IOP_USB,   bdm_irx,             &size_bdm_irx,             NULL, 0 },
	{ "bdmfs_fatfs",     PS2_IOP_USB,   bdmfs_fatfs_irx,     &size_bdmfs_fatfs_irx,     NULL, 0 },
	{ "usbmass_bd_mini", PS2_IOP_USB,   usbmass_bd_mini_irx, &size_usbmass_bd_mini_irx, NULL, 0 },
	{ "libsd",           PS2_IOP_AUDIO, libsd_irx,           &size_libsd_irx,           NULL, 0 },
	{ "nzpsnd",          PS2_IOP_AUDIO, nzpsnd_irx,          &size_nzpsnd_irx,          NULL, 0 },
};
#define IRX_COUNT ((int)(sizeof(irx_table) / sizeof(irx_table[0])))

static int groups_requested;
static int groups_ready;
static int iop_was_reset;

static int load_irx(ps2_irx_t *m)
{
	int res = 0;
	m->id = SifExecModuleBuffer(m->data, *m->size, m->args_len, m->args, &res);
	m->result = res;
	m->loaded = (m->id >= 0 && (res & 3) != 1);   // MODULE_NO_RESIDENT_END == 1
	if (!m->loaded)
		PS2_Log("IOP: FAILED to load %s (id %d, result %d)\n", m->name, m->id, res);
	else
		PS2_Log("IOP: loaded %-16s %6u bytes\n", m->name, *m->size);
	return m->loaded;
}

void PS2_IOP_Init(const char *boot_path, int groups, int no_reset)
{
	int i;
	(void)boot_path;

	groups_requested = groups;
	SifInitRpc(0);

	if (!no_reset) {
		// Reset to a clean IOP: the launcher may have left arbitrary modules
		// (and their memory) resident.
		while (!SifIopReset("", 0))
			;
		while (!SifIopSync())
			;
		SifInitRpc(0);
		iop_was_reset = 1;
	}

	SifLoadFileInit();
	SifInitIopHeap();
	// Allow loading modules from EE RAM and without the rom0: prefix check.
	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();

	for (i = 0; i < IRX_COUNT; i++) {
		ps2_irx_t *m = &irx_table[i];
		if (!(m->group & groups))
			continue;
		load_irx(m);
	}

	// A group is ready when all of its modules loaded.
	for (i = 0; i < 5; i++) {
		int g = 1 << i, j, ok = 1;
		if (!(groups & g))
			continue;
		for (j = 0; j < IRX_COUNT; j++)
			if ((irx_table[j].group & g) && !irx_table[j].loaded)
				ok = 0;
		if (ok)
			groups_ready |= g;
	}

	if (groups_ready & PS2_IOP_CORE) {
		if (fileXioInit() < 0)
			PS2_Log("IOP: fileXioInit failed\n");
	}
}

int PS2_IOP_GroupReady(int group)
{
	return (groups_ready & group) == group;
}

void PS2_IOP_Report(void)
{
	int i;
	unsigned int total = 0;
	PS2_Log("IOP modules (%s):\n", iop_was_reset ? "after reset" : "no reset");
	for (i = 0; i < IRX_COUNT; i++) {
		ps2_irx_t *m = &irx_table[i];
		if (!(m->group & groups_requested))
			continue;
		PS2_Log("  %-16s %6u bytes  %s\n", m->name, *m->size, m->loaded ? "loaded" : "FAILED");
		if (m->loaded)
			total += *m->size;
	}
	PS2_Log("  total module images: %u KiB of 2048 KiB IOP RAM\n", total / 1024);
}

int PS2_WaitForDevice(const char *root, int timeout_ms)
{
	ps2_clock_t start = ps2_clock();
	char probe[256];
	int n = snprintf(probe, sizeof(probe), "%s", root);
	if (n <= 0)
		return 0;
	// devices such as "mass:" need a path, "mass:/" works for dopen
	if (probe[n - 1] == ':')
		snprintf(probe + n, sizeof(probe) - n, "/");
	for (;;) {
		DIR *d = opendir(probe);
		if (d) {
			closedir(d);
			return 1;
		}
		if ((int)((ps2_clock() - start) / PS2_CLOCKS_PER_MSEC) > timeout_ms)
			return 0;
		// Give the IOP (USB enumeration) time; this only runs at boot.
		DelayThread(50 * 1000);
	}
}
