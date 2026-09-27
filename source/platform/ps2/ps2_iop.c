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
DECLARE_IRX(mx4sio_bd_mini);
DECLARE_IRX(iLinkman);
DECLARE_IRX(IEEE1394_bd_mini);
DECLARE_IRX(mmceman);
DECLARE_IRX(dev9);
DECLARE_IRX(atad);
DECLARE_IRX(apa);
DECLARE_IRX(fs);
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
	int inherited;  // preserved from the launcher's IOP when reset is skipped
} ps2_irx_t;

// Conservative, well-tested HDD/PFS cache settings used by wLaunchELF-style
// homebrew. They keep IOP RAM bounded while leaving enough handles for the game.
static const char hdd_args[] = "-o\0" "4\0" "-n\0" "20";
static const char pfs_args[] = "-m\0" "4\0" "-o\0" "10\0" "-n\0" "40";

// Order matters: dependencies first.
static ps2_irx_t irx_table[] = {
	{ "iomanX",          PS2_IOP_CORE,  iomanx_irx,          &size_iomanx_irx,          NULL, 0 },
	// fileXio: keep its read/write staging buffer modest (IOP RAM)
	{ "fileXio",         PS2_IOP_CORE,  filexio_irx,         &size_filexio_irx,         NULL, 0 },

	// SIO2 is shared by pads, real memory cards, MX4SIO and MMCE.
	{ "sio2man",         PS2_IOP_PAD | PS2_IOP_MC | PS2_IOP_MASS | PS2_IOP_MMCE,
	                     sio2man_irx, &size_sio2man_irx, NULL, 0 },
	{ "padman",          PS2_IOP_PAD,   padman_irx,          &size_padman_irx,          NULL, 0 },
	{ "mcman",           PS2_IOP_MC,    mcman_irx,           &size_mcman_irx,           NULL, 0 },
	{ "mcserv",          PS2_IOP_MC,    mcserv_irx,          &size_mcserv_irx,          NULL, 0 },

	// BDM mass storage. The pathname exposed by bdmfs is massN: regardless of
	// whether the block transport is USB, MX4SIO or i.Link, so restore all
	// three lightweight transports when the ELF itself came from massN:.
	{ "usbd_mini",       PS2_IOP_MASS, usbd_mini_irx,        &size_usbd_mini_irx,        NULL, 0 },
	{ "bdm",             PS2_IOP_MASS, bdm_irx,              &size_bdm_irx,              NULL, 0 },
	{ "bdmfs_fatfs",     PS2_IOP_MASS, bdmfs_fatfs_irx,      &size_bdmfs_fatfs_irx,      NULL, 0 },
	{ "usbmass_bd_mini", PS2_IOP_MASS, usbmass_bd_mini_irx,  &size_usbmass_bd_mini_irx,  NULL, 0 },
	{ "mx4sio_bd_mini",  PS2_IOP_MASS, mx4sio_bd_mini_irx,   &size_mx4sio_bd_mini_irx,   NULL, 0 },
	{ "iLinkman",        PS2_IOP_MASS, iLinkman_irx,         &size_iLinkman_irx,         NULL, 0 },
	{ "IEEE1394_bd_mini",PS2_IOP_MASS, IEEE1394_bd_mini_irx, &size_IEEE1394_bd_mini_irx, NULL, 0 },

	// MMCE exposes mmce0:/mmce1: directly through iomanX/fileXio.
	{ "mmceman",         PS2_IOP_MMCE, mmceman_irx,          &size_mmceman_irx,          NULL, 0 },

	// Internal HDD: DEV9 -> ATA -> APA (hdd0:) -> PFS (pfs0:).
	{ "dev9",            PS2_IOP_HDD,  dev9_irx,             &size_dev9_irx,             NULL, 0 },
	{ "atad",            PS2_IOP_HDD,  atad_irx,             &size_atad_irx,             NULL, 0 },
	{ "apa",             PS2_IOP_HDD,  apa_irx,              &size_apa_irx,              hdd_args, sizeof(hdd_args) },
	{ "fs",              PS2_IOP_HDD,  fs_irx,               &size_fs_irx,               pfs_args, sizeof(pfs_args) },

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
	m->inherited = 0;
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
	const int storage_groups = PS2_IOP_MASS | PS2_IOP_MMCE | PS2_IOP_HDD;
	(void)boot_path;

	groups_requested = groups;
	groups_ready = 0;
	iop_was_reset = 0;
	for (i = 0; i < IRX_COUNT; i++) {
		irx_table[i].id = -1;
		irx_table[i].result = 0;
		irx_table[i].loaded = 0;
		irx_table[i].inherited = 0;
	}
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

		// When the launcher's IOP must be preserved (notably a raw pfsN:
		// boot path), do not replace its filesystem/device stack: that would
		// destroy the very mount we are trying to keep. Pad/audio services are
		// still loaded normally.
		if (no_reset &&
		    ((m->group == PS2_IOP_CORE) ||
		     ((m->group & storage_groups) &&
		      !(m->group & (PS2_IOP_PAD | PS2_IOP_MC | PS2_IOP_AUDIO))))) {
			m->loaded = 1;
			m->inherited = 1;
			continue;
		}
		load_irx(m);
	}

	// A group is ready when all of its modules loaded (or were inherited).
	for (i = 0; i <= 6; i++) {
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
		PS2_Log("  %-16s %6u bytes  %s\n", m->name, *m->size,
			m->inherited ? "inherited" : (m->loaded ? "loaded" : "FAILED"));
		if (m->loaded && !m->inherited)
			total += *m->size;
	}
	PS2_Log("  total module images: %u KiB of 2048 KiB IOP RAM\n", total / 1024);
}

int PS2_IOP_StorageGroupsForBootPath(const char *boot_path)
{
	if (!boot_path || !*boot_path)
		return PS2_IOP_MASS;
	if (!strncmp(boot_path, "mass", 4))
		return PS2_IOP_MASS;
	if (!strncmp(boot_path, "mmce", 4))
		return PS2_IOP_MMCE;
	if (!strncmp(boot_path, "mc0:", 4) || !strncmp(boot_path, "mc1:", 4))
		return PS2_IOP_MC;
	if (!strncmp(boot_path, "hdd", 3))
		return PS2_IOP_HDD;
	// pfsN: is already a mounted HDD partition. Without the APA partition
	// name in argv[0] it cannot be reconstructed after reset, so it is
	// deliberately preserved instead (see PS2_IOP_BootPathNeedsNoReset).
	return 0;
}

int PS2_IOP_BootPathNeedsNoReset(const char *boot_path)
{
	const char *colon;
	if (!boot_path || !*boot_path)
		return 0;

	// A bare pfsN: path has already lost the APA partition name needed to
	// remount it, and host: on real hardware belongs to ps2link. Preserve
	// those launch environments automatically.
	if (!strncmp(boot_path, "pfs", 3) || !strncmp(boot_path, "host:", 5))
		return 1;

	// Known devices below can be rebuilt from modules embedded in this ELF.
	if (!strncmp(boot_path, "mass", 4) ||
	    !strncmp(boot_path, "mmce", 4) ||
	    !strncmp(boot_path, "hdd", 3) ||
	    !strncmp(boot_path, "mc0:", 4) ||
	    !strncmp(boot_path, "mc1:", 4))
		return 0;

	// Generic fallback for launcher-provided devices we do not know how to
	// recreate: if argv[0] has a device prefix, retain the launcher's IOP so
	// the existing iomanX device registration survives.
	colon = strchr(boot_path, ':');
	return colon != NULL && strncmp(boot_path, "cdrom", 5) != 0;
}

int PS2_IOP_ShouldWaitForDevice(const char *root)
{
	if (!root || !strchr(root, ':'))
		return 0;
	// host: is synchronous and waiting on it can stall PCSX2/ps2link startup.
	return strncmp(root, "host:", 5) != 0;
}

static int prepare_mass_boot_path(const char *boot_path, char *resolved, int resolved_size)
{
	char normalized[512];
	char candidate[512];
	const char *colon;
	const char *suffix;
	ps2_clock_t start;
	size_t i;
	int unit;

	if (!boot_path || strncmp(boot_path, "mass", 4))
		return 0;

	snprintf(normalized, sizeof(normalized), "%s", boot_path);
	for (i = 0; normalized[i]; i++)
		if (normalized[i] == '\\')
			normalized[i] = '/';

	colon = strchr(normalized, ':');
	if (!colon) {
		PS2_Log("IOP: malformed mass boot path: %s\n", normalized);
		return -1;
	}
	suffix = colon + 1;
	if (!*suffix)
		suffix = "/";

	// BDM numbering belongs to the current IOP session. A launcher may have
	// called the boot device mass1:, while after our clean reset the same
	// physical drive becomes mass0:. Find the actual post-reset unit by
	// probing the exact ELF path, preferring the original name first.
	start = ps2_clock();
	for (;;) {
		int fd = open(normalized, O_RDONLY);
		if (fd >= 0) {
			close(fd);
			snprintf(resolved, resolved_size, "%s", normalized);
			return 1;
		}

		for (unit = 0; unit < 10; unit++) {
			if (suffix[0] == '/')
				snprintf(candidate, sizeof(candidate), "mass%d:%s", unit, suffix);
			else
				snprintf(candidate, sizeof(candidate), "mass%d:/%s", unit, suffix);
			fd = open(candidate, O_RDONLY);
			if (fd >= 0) {
				close(fd);
				snprintf(resolved, resolved_size, "%s", candidate);
				if (strcmp(normalized, candidate))
					PS2_Log("IOP: BDM boot device renumbered %.*s -> mass%d:\n",
						(int)(colon - normalized + 1), normalized, unit);
				return 1;
			}
		}

		if ((int)((ps2_clock() - start) / PS2_CLOCKS_PER_MSEC) > 10000)
			break;
		DelayThread(50 * 1000);
	}

	PS2_Log("IOP: could not locate boot file after BDM enumeration: %s\n", normalized);
	return -1;
}

static int prepare_hdd_boot_path(const char *boot_path, char *resolved, int resolved_size)
{
	char normalized[512];
	char partition[160];
	const char *p;
	const char *inner;
	const char *sep;
	const char *slash;
	size_t part_len;
	int ret;
	size_t i;

	if (!boot_path || strncmp(boot_path, "hdd0:", 5))
		return 0;

	snprintf(normalized, sizeof(normalized), "%s", boot_path);
	for (i = 0; normalized[i]; i++)
		if (normalized[i] == '\\')
			normalized[i] = '/';

	p = normalized + 5;
	inner = NULL;

	if (*p == '/') {
		// wLaunchELF-style path: hdd0:/partition/path/to/file
		p++;
		slash = strchr(p, '/');
		part_len = slash ? (size_t)(slash - p) : strlen(p);
		inner = slash ? slash : "/";
	} else {
		// Standard homebrew boot path:
		// hdd0:partition:pfs:/path/to/file
		sep = strstr(p, ":pfs:");
		if (!sep) {
			PS2_Log("IOP: unsupported HDD boot path (missing :pfs:): %s\n", normalized);
			return -1;
		}
		part_len = (size_t)(sep - p);
		inner = sep + 5;
		if (!*inner)
			inner = "/";
	}

	if (part_len == 0 || part_len + 6 >= sizeof(partition)) {
		PS2_Log("IOP: invalid HDD partition in boot path: %s\n", normalized);
		return -1;
	}

	snprintf(partition, sizeof(partition), "hdd0:%.*s", (int)part_len, p);

	ret = fileXioMount("pfs0:", partition, FIO_MT_RDWR);
	if (ret < 0) {
		// A launcher may have left pfs0: mounted when -noiopreset was used.
		fileXioUmount("pfs0:");
		ret = fileXioMount("pfs0:", partition, FIO_MT_RDWR);
	}
	if (ret < 0) {
		PS2_Log("IOP: failed to mount %s as pfs0: (%d)\n", partition, ret);
		return -1;
	}

	if (inner[0] == '/')
		snprintf(resolved, resolved_size, "pfs0:%s", inner);
	else
		snprintf(resolved, resolved_size, "pfs0:/%s", inner);

	PS2_Log("IOP: remounted HDD boot partition %s -> pfs0:\n", partition);
	return 1;
}

int PS2_IOP_PrepareBootPath(const char *boot_path, char *resolved, int resolved_size)
{
	int prepared;

	if (!resolved || resolved_size <= 0)
		return 0;
	resolved[0] = 0;

	if (!boot_path || !*boot_path)
		return 1;

	prepared = prepare_mass_boot_path(boot_path, resolved, resolved_size);
	if (prepared < 0)
		return 0;
	if (prepared > 0)
		return 1;

	prepared = prepare_hdd_boot_path(boot_path, resolved, resolved_size);
	if (prepared < 0)
		return 0;
	if (prepared > 0)
		return 1;

	snprintf(resolved, resolved_size, "%s", boot_path);
	return 1;
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
