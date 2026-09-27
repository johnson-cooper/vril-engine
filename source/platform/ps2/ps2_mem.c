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
// ps2_mem.c -- PlayStation 2 memory accounting and diagnostics
//
// Sources of truth:
//   * ELF sections: linker symbols from the ps2sdk linkfile.
//   * malloc heap: newlib mallinfo() (covers every malloc, including libc
//     internals such as FILE buffers).
//   * engine hunk / cache: walked read-only using zone.c's globals. The hunk
//     is itself one malloc'd block (quakeparms_t.membase), so it is reported
//     as a sub-part of the malloc heap, broken down by allocation name.
//   * tagged platform allocations: PS2_MemAlloc counters.
//   * GS VRAM: gs_core statistics.

#include "../../nzportable_def.h"
#include "ps2_iop.h"
#include "gs/gs_core.h"

#include <kernel.h>
#include <malloc.h>
#include <unistd.h>

extern char _ftext[], _etext[], _fdata[], _edata[], _fbss[], _end[];

// zone.c globals (layouts mirrored from zone.c; keep in sync)
extern byte *hunk_base;
extern int   hunk_size;
extern int   hunk_low_used;
extern int   hunk_high_used;

#define PS2_HUNK_SENTINEL 0x1df001ed
typedef struct {
	int  sentinel;
	int  size;
	char name[32];
} ps2_hunk_t;

typedef struct ps2_cache_system_s {
	int size;
	void *user;
	char name[32];
	struct ps2_cache_system_s *prev, *next;
	struct ps2_cache_system_s *lru_prev, *lru_next;
} ps2_cache_system_t;
extern ps2_cache_system_t cache_head;

extern int ps2_audio_bytes(void);

static const char *tag_names[PS2_MEM_NUM_TAGS] = {
	"Textures", "Lightmaps", "Models(EE)", "Audio buffers", "Temporary", "Other"
};

static size_t tag_current[PS2_MEM_NUM_TAGS];
static size_t tag_peak[PS2_MEM_NUM_TAGS];
static size_t heap_peak;
static size_t hunk_peak;
static size_t free_low;          // lowest EE free seen

// Size is stored in a 16 byte header so PS2_MemFree can account it without
// relying on malloc internals.
typedef struct { size_t size; u32 tag; u32 magic; u32 pad; } ps2_alloc_hdr_t;
#define PS2_ALLOC_MAGIC 0x50533241u

static size_t heap_limit(void)
{
	// newlib grows the heap with sbrk up to the kernel provided end of heap
	// (just below the main thread stack).
	return (size_t)EndOfHeap();
}

size_t PS2_MemFree_EE(void)
{
	struct mallinfo mi = mallinfo();
	size_t top = (size_t)sbrk(0);
	size_t lim = heap_limit();
	size_t headroom = lim > top ? lim - top : 0;
	return headroom + (size_t)mi.fordblks;
}

void PS2_MemSample(void)
{
	struct mallinfo mi = mallinfo();
	size_t f;
	if ((size_t)mi.uordblks > heap_peak)
		heap_peak = (size_t)mi.uordblks;
	if ((size_t)(hunk_low_used + hunk_high_used) > hunk_peak)
		hunk_peak = (size_t)(hunk_low_used + hunk_high_used);
	f = PS2_MemFree_EE();
	if (!free_low || f < free_low)
		free_low = f;
}

void *PS2_MemAlloc(ps2_mem_tag_t tag, size_t size, const char *what)
{
	ps2_alloc_hdr_t *h;
	if (tag >= PS2_MEM_NUM_TAGS)
		tag = PS2_MEM_OTHER;
	h = (ps2_alloc_hdr_t *)memalign(64, size + sizeof(ps2_alloc_hdr_t) + 48);
	if (!h) {
		PS2_MemOOM(what ? what : tag_names[tag], size);
		return NULL;
	}
	// keep the returned pointer 64-byte aligned (cache line) with the header
	// immediately before it
	h = (ps2_alloc_hdr_t *)((u8 *)h + 48);
	h->size = size;
	h->tag = (u32)tag;
	h->magic = PS2_ALLOC_MAGIC;
	tag_current[tag] += size;
	if (tag_current[tag] > tag_peak[tag])
		tag_peak[tag] = tag_current[tag];
	return (void *)(h + 1);
}

void PS2_MemFree(ps2_mem_tag_t tag, void *ptr)
{
	ps2_alloc_hdr_t *h;
	(void)tag;
	if (!ptr)
		return;
	h = (ps2_alloc_hdr_t *)ptr - 1;
	if (h->magic != PS2_ALLOC_MAGIC)
		Sys_Error("PS2_MemFree: bad pointer %p", ptr);
	if (h->tag < PS2_MEM_NUM_TAGS)
		tag_current[h->tag] -= h->size;
	h->magic = 0;
	free((u8 *)h - 48);
}

// ---------------------------------------------------------------------------
// Hunk breakdown
// ---------------------------------------------------------------------------

typedef enum { HC_MAP, HC_MODELS, HC_QUAKEC, HC_ZONE, HC_NET, HC_OTHER, HC_NUM } hunk_cat_t;
static const char *hunk_cat_names[HC_NUM] = { "Map/BSP", "Models", "QuakeC", "Zone", "Net", "Other" };

static hunk_cat_t classify_hunk(const char *name)
{
	size_t len = strlen(name);
	if (!strcmp(name, "zone"))
		return HC_ZONE;
	if (!strncmp(name, "edicts", 6) || !strncmp(name, "progs", 5) || !strcmp(name, "pr_strings") ||
	    !strncmp(name, "pr_", 3) || !strncmp(name, "qc", 2))
		return HC_QUAKEC;
	if (!strncmp(name, "qsocket", 7) || !strncmp(name, "net", 3))
		return HC_NET;
	if (len > 4 && (!strcmp(name + len - 4, ".mdl") || !strcmp(name + len - 4, ".spr") || !strcmp(name + len - 4, ".md3")))
		return HC_MODELS;
	if (cl.worldmodel && !strncmp(cl.worldmodel->name, "maps/", 5)) {
		char base[64];
		COM_FileBase(cl.worldmodel->name, base);
		if (!Q_strcasecmp(base, (char *)name))
			return HC_MAP;
	}
	if (sv.worldmodel && !strncmp(sv.worldmodel->name, "maps/", 5)) {
		char base[64];
		COM_FileBase(sv.worldmodel->name, base);
		if (!Q_strcasecmp(base, (char *)name))
			return HC_MAP;
	}
	// inline brush models and alias models use their file base as name
	if (strchr(name, '*'))
		return HC_MAP;
	return HC_OTHER;
}

static void hunk_breakdown(size_t *cat, int print_top)
{
	byte *p = hunk_base;
	byte *end = hunk_base + hunk_low_used;
	typedef struct { char name[32]; int size; } top_t;
	top_t top[8];
	int i;
	memset(top, 0, sizeof(top));
	memset(cat, 0, sizeof(size_t) * HC_NUM);
	if (!hunk_base)
		return;
	while (p < end) {
		ps2_hunk_t *h = (ps2_hunk_t *)p;
		char name[33];
		if (h->sentinel != PS2_HUNK_SENTINEL || h->size <= 0) {
			Con_Printf("  (hunk walk stopped: corrupt block at %p)\n", h);
			break;
		}
		memcpy(name, h->name, 32);
		name[32] = 0;
		cat[classify_hunk(name)] += (size_t)h->size;
		for (i = 0; i < 8; i++) {
			if (h->size > top[i].size) {
				memmove(&top[i + 1], &top[i], sizeof(top_t) * (7 - i));
				strlcpy(top[i].name, name, sizeof(top[i].name));
				top[i].size = h->size;
				break;
			}
		}
		p += h->size;
	}
	if (print_top) {
		Con_Printf("  largest hunk blocks:\n");
		for (i = 0; i < 8 && top[i].size; i++)
			Con_Printf("    %-24s %7.1f KiB\n", top[i].name, top[i].size / 1024.0f);
	}
}

static void cache_breakdown(size_t *audio, size_t *other)
{
	ps2_cache_system_t *cs;
	*audio = *other = 0;
	if (!cache_head.next)
		return;
	for (cs = cache_head.next; cs && cs != &cache_head; cs = cs->next) {
		size_t len = strnlen(cs->name, 32);
		if (len > 4 && !Q_strncasecmp(cs->name + len - 4, ".wav", 4))
			*audio += (size_t)cs->size;
		else
			*audio += 0, *other += (size_t)cs->size;
	}
}

#define KB(x) ((float)(x) / 1024.0f)
#define MB(x) ((float)(x) / (1024.0f * 1024.0f))

void PS2_MemReport(const char *label)
{
	struct mallinfo mi = mallinfo();
	size_t text = (size_t)(_etext - _ftext);
	size_t data = (size_t)(_edata - _fdata);
	size_t bss = (size_t)(_end - _fbss);
	size_t elf_total = (size_t)(_end - _ftext);
	size_t hcat[HC_NUM];
	size_t cache_audio, cache_other;
	size_t tagged = 0;
	size_t free_ee;
	gs_vram_stats_t vs;
	int i;

	PS2_MemSample();
	free_ee = PS2_MemFree_EE();
	hunk_breakdown(hcat, ps2_debug_mode);
	cache_breakdown(&cache_audio, &cache_other);
	for (i = 0; i < PS2_MEM_NUM_TAGS; i++)
		tagged += tag_current[i];

	Con_Printf("PS2 MEMORY [%s]\n", label ? label : "");
	Con_Printf("-----------\n");
	Con_Printf("EE free:        %6.2f MiB  (lowest %.2f MiB)\n", MB(free_ee), MB(free_low));
	Con_Printf("ELF image:      %6.2f MiB  (text %.0f K, data %.0f K, bss %.0f K)\n",
		MB(elf_total), KB(text), KB(data), KB(bss));
	Con_Printf("malloc heap:    %6.2f MiB  in use (arena %.2f MiB, peak %.2f MiB)\n",
		MB(mi.uordblks), MB(mi.arena), MB(heap_peak));
	Con_Printf("Engine hunk:    %6.2f MiB  (%.2f used, %.2f free, peak %.2f)\n",
		MB(hunk_size), MB(hunk_low_used + hunk_high_used),
		MB(hunk_size - hunk_low_used - hunk_high_used), MB(hunk_peak));
	Con_Printf("  Map/BSP:      %6.2f MiB\n", MB(hcat[HC_MAP]));
	Con_Printf("  Models:       %6.2f MiB\n", MB(hcat[HC_MODELS]));
	Con_Printf("  QuakeC:       %6.2f MiB\n", MB(hcat[HC_QUAKEC]));
	Con_Printf("  Zone:         %6.2f MiB\n", MB(hcat[HC_ZONE]));
	Con_Printf("  Net:          %6.2f MiB\n", MB(hcat[HC_NET]));
	Con_Printf("  Other:        %6.2f MiB\n", MB(hcat[HC_OTHER]));
	Con_Printf("  Cache(snd):   %6.2f MiB\n", MB(cache_audio));
	Con_Printf("  Cache(other): %6.2f MiB\n", MB(cache_other));
	Con_Printf("  Temp/high:    %6.2f MiB\n", MB(hunk_high_used));
	for (i = 0; i < PS2_MEM_NUM_TAGS; i++)
		Con_Printf("%-15s %6.2f MiB  (peak %.2f)\n", tag_names[i], MB(tag_current[i]), MB(tag_peak[i]));
	Con_Printf("Audio (SPU2/IOP side): %.0f KiB\n", KB(ps2_audio_bytes()));

	GS_GetVramStats(&vs);
	Con_Printf("GS VRAM total %.0fK: frame %.0fK depth %.0fK textures %.0fK (used %.0fK, high %.0fK, %d resident, %d evictions)\n",
		KB(vs.total), KB(vs.framebuffers), KB(vs.depth), KB(vs.tex_region), KB(vs.tex_used),
		KB(vs.tex_highwater), vs.resident_textures, vs.evictions_total);
	PS2_IOP_Report();
}

void PS2_MemOOM(const char *what, size_t requested)
{
	Con_Printf("\nPS2 OOM:\n");
	Con_Printf("requested: %.1f KiB (%s)\n", KB(requested), what ? what : "?");
	Con_Printf("EE free:   %.1f KiB\n", KB(PS2_MemFree_EE()));
	PS2_MemReport("out of memory");
}

static void PS2_Mem_f(void)
{
	int old = ps2_debug_mode;
	ps2_debug_mode = 1;
	PS2_MemReport(Cmd_Argc() > 1 ? Cmd_Argv(1) : "manual");
	ps2_debug_mode = old;
}

static void PS2_VRAM_f(void)
{
	gs_vram_stats_t vs;
	GS_GetVramStats(&vs);
	Con_Printf("PS2 VRAM\n--------\n");
	Con_Printf("VRAM total          %7.0f KiB\n", KB(vs.total));
	Con_Printf("VRAM framebuffer    %7.0f KiB\n", KB(vs.framebuffers));
	Con_Printf("VRAM depth          %7.0f KiB\n", KB(vs.depth));
	Con_Printf("VRAM textures       %7.0f KiB (in use %0.f KiB, %d resident)\n", KB(vs.tex_region), KB(vs.tex_used), vs.resident_textures);
	Con_Printf("VRAM free           %7.0f KiB\n", KB(vs.tex_region - vs.tex_used));
	Con_Printf("VRAM tex high-water %7.0f KiB\n", KB(vs.tex_highwater));
	Con_Printf("evictions: %d this frame, %d total, ring wraps %d\n", vs.evictions_frame, vs.evictions_total, vs.wraps);
}

void PS2_MemInit(void)
{
	heap_peak = 0;
	hunk_peak = 0;
	free_low = 0;
	PS2_MemSample();
}

void PS2_MemRegisterCommands(void)
{
	Cmd_AddCommand("ps2_mem", PS2_Mem_f);
	Cmd_AddCommand("ps2_vram", PS2_VRAM_f);
}

// ---------------------------------------------------------------------------
// malloc failure interception (linked with -Wl,--wrap=malloc etc.)
// ---------------------------------------------------------------------------
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__real_memalign(size_t, size_t);

static int in_oom;

static void oom_hook(size_t size, const char *fn)
{
	if (in_oom)
		return;
	in_oom = 1;
	// Anything that can be regenerated could be released here. The engine
	// cache lives inside the hunk, so the only malloc-side cache is the GS
	// texture backing store which cannot be dropped without re-reading files.
	PS2_MemOOM(fn, size);
	in_oom = 0;
}

void *__wrap_malloc(size_t size)
{
	void *p = __real_malloc(size);
	if (!p && size)
		oom_hook(size, "malloc");
	return p;
}

void *__wrap_calloc(size_t n, size_t size)
{
	void *p = __real_calloc(n, size);
	if (!p && n && size)
		oom_hook(n * size, "calloc");
	return p;
}

void *__wrap_realloc(void *ptr, size_t size)
{
	void *p = __real_realloc(ptr, size);
	if (!p && size)
		oom_hook(size, "realloc");
	return p;
}

void *__wrap_memalign(size_t align, size_t size)
{
	void *p = __real_memalign(align, size);
	if (!p && size)
		oom_hook(size, "memalign");
	return p;
}
