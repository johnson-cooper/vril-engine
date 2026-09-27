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
#define PS2_IOP_USB      (1 << 3)   // usbd + bdm + fat + usb mass storage
#define PS2_IOP_AUDIO    (1 << 4)   // libsd + nzpsnd

// Bring the IOP to a known state and load the requested module groups.
// boot_path is argv[0]; reset is skipped when `no_reset` is set (needed when
// launched from ps2link on real hardware, where a reset kills host:).
void PS2_IOP_Init(const char *boot_path, int groups, int no_reset);
int  PS2_IOP_GroupReady(int group);
void PS2_IOP_Report(void);

// Storage
// Wait until a block device (mass:, ...) answers, up to timeout_ms.
int  PS2_WaitForDevice(const char *root, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // PS2_IOP_H
