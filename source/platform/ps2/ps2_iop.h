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
// ps2_iop.h -- IOP bring-up, embedded IRX modules, storage devices
#ifndef PS2_IOP_H
#define PS2_IOP_H

#ifdef __cplusplus
extern "C" {
#endif

// Groups of IOP services. Only what the game needs is loaded; everything is
// embedded in the ELF (ps2.yaml embed_irx) so no IRX folder has to be
// maintained next to the executable.
#define PS2_IOP_CORE     (1 << 0)   // iomanX + fileXio
#define PS2_IOP_PAD      (1 << 1)   // sio2man + padman
#define PS2_IOP_MC       (1 << 2)   // mcman + mcserv
#define PS2_IOP_MASS     (1 << 3)   // BDM/FAT + USB/MX4SIO/i.Link mass transports
#define PS2_IOP_AUDIO    (1 << 4)   // libsd + nzpsnd
#define PS2_IOP_MMCE     (1 << 5)   // mmceman (mmce0:/mmce1:)
#define PS2_IOP_HDD      (1 << 6)   // dev9 + atad + APA + PFS

// Backwards-compatible name used by older PS2 port code.
#define PS2_IOP_USB PS2_IOP_MASS

// Bring the IOP to a known state and load the requested module groups.
// boot_path is argv[0]; reset is skipped when `no_reset` is set (needed when
// launched from ps2link on real hardware, where a reset kills host:).
void PS2_IOP_Init(const char *boot_path, int groups, int no_reset);
int  PS2_IOP_GroupReady(int group);
void PS2_IOP_Report(void);

// Storage
// Select the minimum storage stack needed to recover argv[0] after an IOP
// reset. BDM-backed media all reappear as massN: devices.
int  PS2_IOP_StorageGroupsForBootPath(const char *boot_path);

// Some launchers expose mounts that cannot be reconstructed from argv[0]
// alone (raw pfsN:, host:, or custom iomanX devices). Preserve the launcher's
// IOP environment for those paths instead of destroying the mount.
int  PS2_IOP_BootPathNeedsNoReset(const char *boot_path);

// Recreate device state destroyed by an IOP reset and return a path that can
// be used by the normal POSIX/fileXio layer. HDD boot paths are remounted as
// pfs0:; massN: is also re-probed because BDM unit numbers can change across
// an IOP reset. Other devices are copied unchanged.
int  PS2_IOP_PrepareBootPath(const char *boot_path, char *resolved, int resolved_size);

// Whether a device can enumerate asynchronously and should be waited for
// before setup.ini/assets are opened.
int  PS2_IOP_ShouldWaitForDevice(const char *root);

// Wait until a storage device answers, up to timeout_ms.
int  PS2_WaitForDevice(const char *root, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // PS2_IOP_H
