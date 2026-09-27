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
// ps2_profile.c -- lightweight frame profiler
//
// Sections accumulate ps2_clock() ticks (576 kHz). With "ps2_profile 1" the
// averages over the last period are printed every 2 seconds together with the
// GS statistics (DMA wait, vsync wait, uploads, vertices).

#include "../../nzportable_def.h"
#include "ps2_profile.h"
#include "gs/gs_core.h"

#include <ps2sdkapi.h>

static cvar_t ps2_profile = {"ps2_profile", "0"};

static const char *names[PROF_NUM] = {
	"frame", "render", "world", "entities", "particles", "hud",
	"server(qc+phys)", "audio", "filesystem", "gs submit"
};

static ps2_clock_t start[PROF_NUM];
static u32 acc[PROF_NUM];
static u32 worst_frame;
static u32 frames;
static u32 dma_wait, vsync_wait, uploads, upload_bytes, verts, kicks;
static double last_print;

void PS2_ProfileInit(void)
{
	Cvar_RegisterVariable(&ps2_profile);
}

void PS2_ProfileBegin(ps2_prof_section_t s) { start[s] = ps2_clock(); }
void PS2_ProfileEnd(ps2_prof_section_t s) { acc[s] += (u32)(ps2_clock() - start[s]); }
void PS2_ProfileAdd(ps2_prof_section_t s, unsigned int ticks) { acc[s] += ticks; }

void PS2_ProfileBeginFrame(void)
{
	start[PROF_FRAME] = ps2_clock();
}

#define MS(t) ((float)(t) * 1000.0f / (float)PS2_CLOCKS_PER_SEC)

void PS2_ProfileEndFrame(void)
{
	u32 ft = (u32)(ps2_clock() - start[PROF_FRAME]);
	double now;
	int i;

	acc[PROF_FRAME] += ft;
	if (ft > worst_frame)
		worst_frame = ft;
	frames++;
	dma_wait += gs_stats_last.dma_wait_ticks;
	vsync_wait += gs_stats_last.vsync_wait_ticks;
	uploads += gs_stats_last.uploads;
	upload_bytes += gs_stats_last.upload_bytes;
	verts += gs_stats_last.verts_out;
	kicks += gs_stats_last.kicks;

	now = Sys_FloatTime();
	if (now - last_print < 2.0)
		return;
	if (ps2_profile.value && frames) {
		Con_Printf("PS2 PROFILE (%u frames, avg ms; worst frame %.1f ms)\n", frames, MS(worst_frame));
		for (i = 0; i < PROF_NUM; i++)
			Con_Printf("  %-16s %6.2f\n", names[i], MS(acc[i]) / frames);
		Con_Printf("  %-16s %6.2f\n", "DMA wait", MS(dma_wait) / frames);
		Con_Printf("  %-16s %6.2f\n", "vsync wait", MS(vsync_wait) / frames);
		Con_Printf("  GS: %u verts/frame, %u uploads/frame (%.0f KiB), %u kicks/frame\n",
			verts / frames, uploads / frames, upload_bytes / 1024.0f / frames, kicks / frames);
	}
	for (i = 0; i < PROF_NUM; i++)
		acc[i] = 0;
	worst_frame = frames = 0;
	dma_wait = vsync_wait = uploads = upload_bytes = verts = kicks = 0;
	last_print = now;
}
