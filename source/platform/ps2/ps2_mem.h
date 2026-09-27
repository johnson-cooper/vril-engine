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
// ps2_mem.h -- PlayStation 2 memory accounting and diagnostics
//
// The EE has 32 MiB of RAM (31 MiB usable by applications). Memory use is
// treated as a first-class engineering constraint on this port, so every
// large consumer is accounted here and the whole picture can be printed at
// any time ("ps2_mem" console command) and automatically at key points
// (startup, map load, after spawning, after unload, on OOM).
#ifndef PS2_MEM_H
#define PS2_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Categories for renderer/platform owned allocations (malloc side). Hunk,
// zone and cache usage are derived from the engine allocators directly.
typedef enum {
	PS2_MEM_TEXTURES = 0,    // EE backing store of GS textures (world, skins, pics)
	PS2_MEM_LIGHTMAPS,       // lightmap planes
	PS2_MEM_MODELS,          // renderer-side model data outside the hunk
	PS2_MEM_AUDIO,           // audio buffers owned by the platform
	PS2_MEM_TEMP,            // short lived conversion buffers
	PS2_MEM_OTHER,
	PS2_MEM_NUM_TAGS
} ps2_mem_tag_t;

// Tagged allocations. Always 16-byte aligned (GS DMA requirement). On failure
// these print a full memory report naming the request and return NULL.
void *PS2_MemAlloc (ps2_mem_tag_t tag, size_t size, const char *what);
void  PS2_MemFree (ps2_mem_tag_t tag, void *ptr);

// Print the memory report to the console/log.
void PS2_MemReport (const char *label);
// Update the high-water marks (cheap, call once per frame).
void PS2_MemSample (void);
// Bytes of EE RAM not yet claimed (sbrk headroom + free malloc chunks).
size_t PS2_MemFree_EE (void);
// Print report on allocation failure (called from OOM paths).
void PS2_MemOOM (const char *what, size_t requested);

void PS2_MemInit (void);
void PS2_MemRegisterCommands (void);

#ifdef __cplusplus
}
#endif

#endif // PS2_MEM_H
