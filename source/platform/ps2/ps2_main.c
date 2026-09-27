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
// ps2_main.c -- PlayStation 2 entry point and main loop

#include "../../nzportable_def.h"
#include "../../_build_info.h"
#include "ps2_iop.h"
#include "ps2_profile.h"

#include <kernel.h>
#include <sys/stat.h>


extern void PS2_SetGameRootFromBootPath(const char *boot);
extern void PS2_LogToConsole(int enable);
extern void PS2_Log(const char *fmt, ...);

// Engine hunk budget. This single block holds the BSP, models, QuakeC,
// the zone and the sound/data cache. See README_PS2.md "Memory budget".
// Overridable with "-heap <MiB>" in setup.ini or on the command line.
#define PS2_DEFAULT_HUNK_MB 12

// Main thread stack. The default ps2sdk stack is 128 KiB which is too small
// for Quake's recursive BSP walks + QuakeC + deep call chains; set in
// ps2.yaml via --defsym _stack_size.

static int has_arg(int argc, char **argv, const char *name)
{
	int i;
	for (i = 1; i < argc; i++)
		if (argv[i] && !strcmp(argv[i], name))
			return 1;
	return 0;
}

// DualShock 2 defaults, following the NZ:P twin stick console layout with
// fire on R1 and aim on L1. Everything stays rebindable in the menus.
static void PS2_ApplyDefaultBindings(void)
{
	Cbuf_AddText("unbindall\n");
	Cbuf_AddText("bind START \"togglemenu\"\n");
	Cbuf_AddText("bind RTRIGGER \"+attack\"\n");
	Cbuf_AddText("bind LTRIGGER \"+aim\"\n");
	Cbuf_AddText("bind ZRTRIGGER \"+grenade\"\n");
	Cbuf_AddText("bind ZLTRIGGER \"impulse 33\"\n");
	Cbuf_AddText("bind LEFTFACE \"+reload\"\n");
	Cbuf_AddText("bindhold LEFTFACE \"+use\"\n");
	Cbuf_AddText("bind TOPFACE \"+switch\"\n");
	Cbuf_AddText("bind BOTTOMFACE \"+jump\"\n");
	Cbuf_AddText("bind RIGHTFACE \"impulse 31\"\n");
	Cbuf_AddText("bindhold RIGHTFACE \"impulse 32\"\n");
	Cbuf_AddText("bind LTHUMB \"impulse 23\"\n");
	Cbuf_AddText("bind RTHUMB \"+knife\"\n");
	Cbuf_AddText("bind DPAD_RIGHT \"impulse 22\"\n");
	Cbuf_AddText("bind SELECT \"+showscores\"\n");
	Cbuf_AddText("in_mlook 1\n");
	Cbuf_AddText("cl_maxfps 30\n");
	PS2_Log("PS2: no ps2_config.cfg yet, applied DualShock 2 default bindings\n");
}

static int file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

int main(int argc, char **argv)
{
	quakeparms_t parms;
	startup_arguments_t startup;
	const char *base_directory;
	char setup_path[MAX_OSPATH];
	char startup_error[256];
	char resolved_boot[512];
	const char *boot_path;
	size_t heap_size;
	int no_reset;
	int groups;

	boot_path = argc > 0 && argv[0] ? argv[0] : NULL;
	printf("\nNZ:P PS2 %s (%s) built %s\n", GIT_HASH, GIT_BRANCH, BUILD_DATE);
	printf("boot: %s\n", boot_path ? boot_path : "(none)");

	PS2_MemInit();

	// IOP bring-up. The storage stack is selected from argv[0] so the game can
	// recover the device it was launched from without keeping every driver in
	// the 2 MiB IOP at all times.
	no_reset = has_arg(argc, argv, "-noiopreset") ||
	           PS2_IOP_BootPathNeedsNoReset(boot_path);
	groups = PS2_IOP_CORE | PS2_IOP_PAD | PS2_IOP_MC | PS2_IOP_AUDIO |
	         PS2_IOP_StorageGroupsForBootPath(boot_path);
	PS2_IOP_Init(boot_path, groups, no_reset);

	if (!PS2_IOP_PrepareBootPath(boot_path, resolved_boot, sizeof(resolved_boot)))
		Sys_Error("PS2: could not restore boot storage for %s",
			boot_path ? boot_path : "(none)");

	PS2_SetGameRootFromBootPath(resolved_boot[0] ? resolved_boot : boot_path);
	if (PS2_IOP_ShouldWaitForDevice(PS2_GetGameRoot())) {
		if (!PS2_WaitForDevice(PS2_GetGameRoot(), 10000))
			printf("warning: %s did not become ready\n", PS2_GetGameRoot());
	}
	printf("game root: %s\n", PS2_GetGameRoot());

	snprintf(setup_path, sizeof(setup_path), "%s/setup.ini", PS2_GetGameRoot());
	if (!Startup_LoadArguments(&startup, argc, argv, file_exists(setup_path) ? setup_path : NULL,
		startup_error, sizeof(startup_error)))
		Sys_Error("Startup: %s", startup_error);
	if (!Startup_GetBaseDirectory(&startup, PS2_GetGameRoot(), &base_directory,
		startup_error, sizeof(startup_error)))
		Sys_Error("Startup: %s", startup_error);
	if (Startup_FindArgument(&startup, "-developer") || Startup_FindArgument(&startup, "-ps2debug"))
		ps2_debug_mode = 1;
	if (Startup_FindArgument(&startup, "-lowmem"))
		ps2_low_memory_mode = 1;

	memset(&parms, 0, sizeof(parms));
	parms.membase = Startup_AllocateHeap(&startup, (size_t)PS2_DEFAULT_HUNK_MB * 1024 * 1024,
		&heap_size, startup_error, sizeof(startup_error));
	if (!parms.membase) {
		PS2_MemOOM("engine hunk", (size_t)PS2_DEFAULT_HUNK_MB * 1024 * 1024);
		Sys_Error("Startup: %s", startup_error);
	}
	parms.memsize = (int)heap_size;
	parms.basedir = (char *)base_directory;

	COM_InitArgv(startup.argc, startup.argv);
	parms.argc = com_argc;
	parms.argv = com_argv;

	Host_Init(&parms);
	PS2_LogToConsole(1);

	// First run: no PS2 settings yet, so nzp.rc fell back to the shared
	// (handheld) config.cfg. Apply the DualShock 2 layout on top; the first
	// Host_WriteConfiguration then creates ps2_config.cfg.
	{
		char cfg[MAX_OSPATH];
		snprintf(cfg, sizeof(cfg), "%s/%sconfig.cfg", com_gamedir, FILE_SPECIAL_PREFIX);
		if (!file_exists(cfg))
			PS2_ApplyDefaultBindings();
	}
	PS2_MemReport("startup");

	{
		double oldtime = Sys_FloatTime();
		double map_start = 0;
		struct model_s *last_world = NULL;
		int spawn_reported = 0;
		for (;;) {
			double now = Sys_FloatTime();
			PS2_ProfileBeginFrame();
			Host_Frame((float)(now - oldtime));
			music_update();
			PS2_MemSample();
			PS2_ProfileEndFrame();
			oldtime = now;

			// Memory checkpoints: after the level has been running a few
			// seconds (entities/AI spawned), and after it is unloaded.
			if (cl.worldmodel != last_world) {
				if (!cl.worldmodel && last_world)
					PS2_MemReport("after map unload");
				last_world = cl.worldmodel;
				map_start = now;
				spawn_reported = 0;
			} else if (cl.worldmodel && !spawn_reported && now - map_start > 8.0) {
				PS2_MemReport("after spawning gameplay entities");
				spawn_reported = 1;
			}
		}
	}
	return 0;
}
