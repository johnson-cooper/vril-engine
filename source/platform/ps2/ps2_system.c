/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2026 NZ:P Team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// ps2_system.c -- PlayStation 2 system interface (files, time, errors, logging)

#include "../../nzportable_def.h"
#include "ps2_iop.h"
#include "gs/gs_core.h"

#include <kernel.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "ps2_profile.h"
#include <fnmatch.h>
#include <debug.h>
#include <ps2sdkapi.h>
#include <libpad.h>

qboolean isDedicated;
int ps2_debug_mode;
int ps2_low_memory_mode;
static char ps2_game_root[MAX_OSPATH];
static int  ps2_log_to_console;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

// Before the console exists everything goes to the EE tty (visible through
// ps2link / PCSX2 "EE Console"). Afterwards Con_Printf mirrors to the tty via
// Sys_Printf.
void PS2_Log(const char *fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (ps2_log_to_console)
		Con_Printf("%s", buf);
	else
		printf("%s", buf);
}

void PS2_LogToConsole(int enable) { ps2_log_to_console = enable; }

// ---------------------------------------------------------------------------
// Files. POSIX calls go through the PS2BUILD newlib/fileXio layer, which
// routes them to iomanX devices (massN:, mmceN:, pfsN:, mcN:, host:, etc.).
// ---------------------------------------------------------------------------

#define MAX_HANDLES 32
static int sys_handles[MAX_HANDLES];
static int sys_handle_used[MAX_HANDLES];

static int Sys_FindHandle(void)
{
	int i;
	for (i = 1; i < MAX_HANDLES; i++)
		if (!sys_handle_used[i])
			return i;
	Sys_Error("Sys_FindHandle: out of file handles");
	return -1;
}

int Sys_FileOpenRead(char *path, int *hndl)
{
	int fd, i, len;
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		*hndl = -1;
		return -1;
	}
	i = Sys_FindHandle();
	sys_handles[i] = fd;
	sys_handle_used[i] = 1;
	len = (int)lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	*hndl = i;
	return len;
}

int Sys_FileOpenWrite(char *path)
{
	int fd, i;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		Con_Printf("Sys_FileOpenWrite: could not open %s\n", path);
		return -1;
	}
	i = Sys_FindHandle();
	sys_handles[i] = fd;
	sys_handle_used[i] = 1;
	return i;
}

void Sys_FileClose(int handle)
{
	if (handle <= 0 || handle >= MAX_HANDLES || !sys_handle_used[handle])
		return;
	close(sys_handles[handle]);
	sys_handle_used[handle] = 0;
}

void Sys_FileSeek(int handle, int position)
{
	lseek(sys_handles[handle], position, SEEK_SET);
}

int Sys_FileRead(int handle, void *dest, int count)
{
	int total = 0;
	ps2_clock_t t0 = ps2_clock();
	while (count > 0) {
		int r = (int)read(sys_handles[handle], (u8 *)dest + total, count);
		if (r <= 0)
			break;
		total += r;
		count -= r;
	}
	PS2_ProfileAdd(PROF_FILESYSTEM, (unsigned int)(ps2_clock() - t0));
	return total;
}

int Sys_FileWrite(int handle, void *data, int count)
{
	return (int)write(sys_handles[handle], data, count);
}

int Sys_FileTime(char *path)
{
	struct stat st;
	if (stat(path, &st) < 0)
		return -1;
	return st.st_mtime > 0 ? (int)st.st_mtime : 1;
}

void Sys_mkdir(char *path)
{
	mkdir(path, 0777);
}

// Directory enumeration (names only)
static DIR *find_dir;
static char find_pattern[MAX_OSPATH];
static char find_name[MAX_OSPATH];

char *Sys_FindNextFile(void)
{
	struct dirent *de;
	if (!find_dir)
		return NULL;
	while ((de = readdir(find_dir)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (fnmatch(find_pattern, de->d_name, 0) == 0) {
			strlcpy(find_name, de->d_name, sizeof(find_name));
			return find_name;
		}
	}
	return NULL;
}

char *Sys_FindFirstFile(char *path, char *pattern)
{
	Sys_FindClose();
	find_dir = opendir(path);
	if (!find_dir)
		return NULL;
	strlcpy(find_pattern, pattern, sizeof(find_pattern));
	return Sys_FindNextFile();
}

void Sys_FindClose(void)
{
	if (find_dir)
		closedir(find_dir);
	find_dir = NULL;
}
void Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length)
{
	(void)startaddr;
	(void)length;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// Derive the installation root from the resolved boot path. HDD launches are
// normalized to pfs0: by PS2_IOP_PrepareBootPath() before this function:
//   "mass1:/NZP/nzp.elf" -> "mass1:/NZP"
//   "mmce0:/NZP/nzp.elf" -> "mmce0:/NZP"
//   "pfs0:/NZP/nzp.elf"  -> "pfs0:/NZP"
//   "mc0:/APPS/nzp.elf"  -> "mc0:/APPS"
//   "host:nzp.elf"       -> "host:"
void PS2_SetGameRootFromBootPath(const char *boot)
{
	const char *slash, *colon;
	size_t len;
	if (!boot || !*boot) {
		strlcpy(ps2_game_root, "mass0:/NZP", sizeof(ps2_game_root));
		return;
	}
	slash = strrchr(boot, '/');
	if (!slash)
		slash = strrchr(boot, '\\');
	colon = strchr(boot, ':');
	if (slash) {
		len = (size_t)(slash - boot);
	} else if (colon) {
		len = (size_t)(colon - boot) + 1;
	} else {
		strlcpy(ps2_game_root, "mass0:/NZP", sizeof(ps2_game_root));
		return;
	}
	if (len >= sizeof(ps2_game_root))
		len = sizeof(ps2_game_root) - 1;
	memcpy(ps2_game_root, boot, len);
	ps2_game_root[len] = 0;
	// PCSX2 passes Windows paths ("host:E:\dir\nzp.elf"); use forward slashes
	for (len = 0; ps2_game_root[len]; len++)
		if (ps2_game_root[len] == '\\')
			ps2_game_root[len] = '/';
	// "cdrom0:\NZP\NZP.ELF;1" style paths are not supported as a data root
	if (!strncmp(ps2_game_root, "cdrom", 5))
		strlcpy(ps2_game_root, "mass0:/NZP", sizeof(ps2_game_root));
}

const char *PS2_GetGameRoot(void)
{
	return ps2_game_root;
}

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------

void Sys_PrintSystemInfo(void)
{
	Con_Printf("NZ:P PlayStation 2 (%s)\n", __DATE__);
	Con_Printf("Game root: %s\n", ps2_game_root);
	Con_Printf("Video: %s %dx%d\n", gs_video.pal ? "PAL" : "NTSC", gs_video.width, gs_video.height);
}

static int fatal_active;

// Last resort error display: reinitialises the GS with the debug text
// renderer, prints the message and a memory summary, then waits for Start
// and returns to the launcher.
static void PS2_ShowFatal(const char *msg)
{
	struct padButtonStatus pad;
	if (fatal_active)
		return;
	fatal_active = 1;
	printf("\n=== NZ:P PS2 FATAL ===\n%s\n", msg);
	init_scr();
	scr_clear();
	scr_printf("=== Nazi Zombies: Portable (PS2) - fatal error ===\n\n");
	scr_printf("%s\n\n", msg);
	scr_printf("EE free: %u KiB\n", (unsigned)(PS2_MemFree_EE() / 1024));
	scr_printf("\nPress START to exit.\n");
	// padman may not be up if the error happened very early
	if (PS2_IOP_GroupReady(PS2_IOP_PAD)) {
		for (;;) {
			if (padRead(0, 0, &pad) != 0) {
				u32 btns = 0xffff ^ pad.btns;
				if (btns & PAD_START)
					break;
			}
		}
	} else {
		SleepThread();
	}
	Exit(1);
}

void PS2_Fatal(const char *fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	Sys_Error("%s", buf);
}

void Sys_SystemError(char *error)
{
	// Memory state is the single most useful thing to know on this platform.
	if (host_initialized)
		PS2_MemReport("fatal error");
	PS2_ShowFatal(error);
}

void Sys_Printf(char *fmt, ...)
{
	char buf[2048];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	printf("%s", buf);
}

void Sys_Quit(void)
{
	if (host_initialized)
		Host_Shutdown();
	GS_Shutdown();
	Exit(0);
}

double Sys_FloatTime(void)
{
	static ps2_clock_t base;
	ps2_clock_t now = ps2_clock();
	if (!base)
		base = now;
	return (double)(now - base) / (double)PS2_CLOCKS_PER_SEC;
}

char *Sys_ConsoleInput(void) { return NULL; }
void Sys_Sleep(void) {}
void Sys_HighFPPrecision(void) {}
void Sys_LowFPPrecision(void) {}
void Sys_SetFPCW(void) {}
void Sys_DebugLog(char *file, char *fmt, ...) { (void)file; (void)fmt; }

void Sys_SendKeyEvents(void)
{
	// Controller events are generated from IN_Commands (ps2_input.c).
}

void Sys_CaptureScreenshot(void)
{
	Con_Printf("Screenshots are not supported on PS2 yet.\n");
}
