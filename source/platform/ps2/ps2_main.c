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
	size_t heap_size;
	int no_reset;
	int groups;

	printf("\nNZ:P PS2 %s (%s) built %s\n", GIT_HASH, GIT_BRANCH, BUILD_DATE);
	printf("boot: %s\n", argc > 0 && argv[0] ? argv[0] : "(none)");

	PS2_MemInit();

	// IOP bring-up. Everything the game needs is embedded in the ELF.
	no_reset = has_arg(argc, argv, "-noiopreset");
	groups = PS2_IOP_CORE | PS2_IOP_PAD | PS2_IOP_MC | PS2_IOP_USB | PS2_IOP_AUDIO;
	PS2_IOP_Init(argc > 0 ? argv[0] : NULL, groups, no_reset);

	PS2_SetGameRootFromBootPath(argc > 0 ? argv[0] : NULL);
	if (!strncmp(PS2_GetGameRoot(), "mass", 4)) {
		if (!PS2_WaitForDevice(PS2_GetGameRoot(), 8000))
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
