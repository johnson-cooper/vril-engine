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
// ps2_profile.h -- lightweight frame profiler (ps2_profile 1)
#ifndef PS2_PROFILE_H
#define PS2_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	PROF_FRAME = 0,
	PROF_RENDER,        // R_RenderView total
	PROF_WORLD,         // world surfaces
	PROF_ENTITIES,      // alias/brush entities
	PROF_PARTICLES,
	PROF_HUD,           // 2D / HUD / menus
	PROF_SERVER,        // server frame (QuakeC + physics)
	PROF_AUDIO,         // mixing + submission
	PROF_FILESYSTEM,    // blocking file reads
	PROF_GS_SUBMIT,     // EndFrame flush
	PROF_NUM
} ps2_prof_section_t;

void PS2_ProfileInit(void);
void PS2_ProfileBeginFrame(void);
void PS2_ProfileEndFrame(void);
void PS2_ProfileBegin(ps2_prof_section_t s);
void PS2_ProfileEnd(ps2_prof_section_t s);
void PS2_ProfileAdd(ps2_prof_section_t s, unsigned int ticks);

#ifdef __cplusplus
}
#endif

#endif
