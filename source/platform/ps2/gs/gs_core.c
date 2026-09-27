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
// gs_core.c -- PlayStation 2 Graphics Synthesizer backend core
//
// See gs_core.h for the overall design. Summary of the data path:
//
//   renderer --GS_DrawArray--> transform (EE, clip space) --> outcode test
//      --> [polygon clip only if needed] --> project to 12.4 fixed point
//      --> REGLIST GIF packets written into the current DMA chain buffer
//
//   GS_BindTexture --> (not resident) VRAM ring allocation
//      --> BITBLTBUF/TRXPOS/TRXREG/TRXDIR + IMAGE GIF tag + DMA REF to the
//          EE backing store (no copy) --> TEXFLUSH
//
// Two chain buffers are used. While the GIF DMA channel consumes one, the
// EE fills the other. A buffer is only re-used after the transfer started
// from it has completed (enforced in pkt_kick()).

#include <kernel.h>
#include <graph.h>
#include <graph_vram.h>
#include <gs_gp.h>
#include <gs_psm.h>
#include <gs_privileged.h>
#include <gif_tags.h>
#include <ps2sdkapi.h>

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "gs_core.h"

// Provided by the platform layer (ps2_system.c); keeps this file free of
// engine headers.
extern void PS2_Fatal(const char *fmt, ...);
extern void PS2_Log(const char *fmt, ...);

gs_video_t    gs_video;
gs_stats_t    gs_stats;
gs_stats_t    gs_stats_last;

// ===========================================================================
// DMA (GIF channel, source chain mode)
// ===========================================================================

#define D2_CHCR ((volatile u32 *)0x1000A000)
#define D2_MADR ((volatile u32 *)0x1000A010)
#define D2_QWC  ((volatile u32 *)0x1000A020)
#define D2_TADR ((volatile u32 *)0x1000A030)

#define DMATAG_ID_CNT 1
#define DMATAG_ID_REF 3
#define DMATAG_ID_END 7

#define DMA_TAG(qwc, id, addr) \
	((u64)((qwc) & 0xffff) | ((u64)((id) & 7) << 28) | ((u64)((u32)(addr) & 0x7fffffff) << 32))

// Size of each chain buffer. Geometry is streamed: when a buffer fills it is
// kicked and the other buffer is used, so this bounds latency, not the
// amount of geometry per frame.
#define PKT_BUFFER_QWORDS (128 * 1024 / 16)
#define PKT_GUARD_QWORDS  8   // space always kept for closing tags

static u64 pkt_storage[2][PKT_BUFFER_QWORDS * 2] __attribute__((aligned(128)));
static int pkt_index;        // buffer being written
static u64 *pkt_base;
static u64 *pkt_ptr;         // always qword aligned between operations
static u64 *pkt_end;
static u64 *pkt_cnt;         // open CNT tag or NULL
static int  pkt_in_flight;   // a transfer was started and may still run

static inline void dma_gif_wait(void)
{
	if (!pkt_in_flight)
		return;
	if (*D2_CHCR & 0x100) {
		ps2_clock_t t0 = ps2_clock();
		while (*D2_CHCR & 0x100)
			;
		gs_stats.dma_wait_ticks += (u32)(ps2_clock() - t0);
	}
	pkt_in_flight = 0;
}

static void dma_gif_start_chain(const void *tadr)
{
	dma_gif_wait();
	*D2_QWC  = 0;
	*D2_TADR = (u32)tadr & 0x0fffffff;
	__asm__ __volatile__("sync.l");
	// DIR=1 (from memory), MOD=1 (source chain), TTE=0, STR=1
	*D2_CHCR = (1 << 0) | (1 << 2) | (1 << 8);
	pkt_in_flight = 1;
}

static inline void pkt_open_cnt(void)
{
	if (!pkt_cnt) {
		pkt_cnt = pkt_ptr;
		pkt_ptr += 2;
	}
}

static inline void pkt_close_cnt(void)
{
	if (pkt_cnt) {
		int qwc = (int)((pkt_ptr - pkt_cnt) >> 1) - 1;
		pkt_cnt[0] = DMA_TAG(qwc, DMATAG_ID_CNT, 0);
		pkt_cnt[1] = 0;
		pkt_cnt = NULL;
	}
}

static void pkt_select(int index)
{
	pkt_index = index;
	pkt_base  = pkt_storage[index];
	pkt_ptr   = pkt_base;
	pkt_end   = pkt_base + (PKT_BUFFER_QWORDS - PKT_GUARD_QWORDS) * 2;
	pkt_cnt   = NULL;
}

static void gif_tag_close(void);

// Terminate the current buffer and hand it to the GIF DMA channel.
static void pkt_kick(void)
{
	gif_tag_close();
	if (pkt_ptr == pkt_base)
		return;
	pkt_close_cnt();
	pkt_ptr[0] = DMA_TAG(0, DMATAG_ID_END, 0);
	pkt_ptr[1] = 0;
	pkt_ptr += 2;
	gs_stats.packet_bytes += (u32)((u8 *)pkt_ptr - (u8 *)pkt_base);
	SyncDCache(pkt_base, pkt_ptr);
	dma_gif_start_chain(pkt_base);
	gs_stats.kicks++;
	// The other buffer's transfer completed before the one we just started
	// was allowed to begin, so it is free to fill.
	pkt_select(pkt_index ^ 1);
}

// Make sure `qwords` qwords of GIF payload fit in the current CNT block.
static inline void pkt_reserve(int qwords)
{
	if (pkt_ptr + (qwords + 2) * 2 > pkt_end)
		pkt_kick();
	if (pkt_ptr + (qwords + 2) * 2 > pkt_end)
		PS2_Fatal("GS: packet request of %d qwords exceeds chain buffer", qwords);
	pkt_open_cnt();
}

// Append a DMA REF to external (16 byte aligned, written back) data.
static void pkt_ref(const void *data, int qwc)
{
	pkt_close_cnt();
	pkt_ptr[0] = DMA_TAG(qwc, DMATAG_ID_REF, data);
	pkt_ptr[1] = 0;
	pkt_ptr += 2;
}

// ===========================================================================
// GIF helpers
// ===========================================================================

#define GIF_AD 0x0e

// A+D register block: tag + n qwords
static u64 *ad_ptr;
static int  ad_count;
static u64 *ad_tag;

static inline void ad_begin(int max_regs)
{
	gif_tag_close();
	pkt_reserve(max_regs + 1);
	ad_tag = pkt_ptr;
	pkt_ptr += 2;
	ad_ptr = pkt_ptr;
	ad_count = 0;
}

static inline void ad_write(int reg, u64 value)
{
	ad_ptr[0] = value;
	ad_ptr[1] = (u64)reg;
	ad_ptr += 2;
	ad_count++;
}

static inline void ad_end(void)
{
	if (ad_count == 0) {
		pkt_ptr = ad_tag;   // nothing written, drop the tag
		return;
	}
	ad_tag[0] = GIF_SET_TAG(ad_count, 1, 0, 0, GIF_FLG_PACKED, 1);
	ad_tag[1] = GIF_AD;
	pkt_ptr = ad_ptr;
}

// ===========================================================================
// Render state
// ===========================================================================

enum {
	DIRTY_FRAME    = 1 << 0,
	DIRTY_ZBUF     = 1 << 1,
	DIRTY_TEST     = 1 << 2,
	DIRTY_ALPHA    = 1 << 3,
	DIRTY_TEX0     = 1 << 4,
	DIRTY_TEX1     = 1 << 5,
	DIRTY_CLAMP    = 1 << 6,
	DIRTY_SCISSOR  = 1 << 7,
	DIRTY_XYOFFSET = 1 << 8,
	DIRTY_FOGCOL   = 1 << 9,
	DIRTY_MISC     = 1 << 10,  // DTHE, COLCLAMP, TEXA, PRMODECONT, PABE, FBA
	DIRTY_ALL      = 0x7ff
};

typedef struct {
	int  caps[GS_CAP_COUNT];
	u32  color;
	int  tfx, tcc;
	int  min_filter, mag_filter;
	int  wrap_u, wrap_v;
	int  blend_a, blend_b, blend_c, blend_d, blend_fix;
	int  alpha_func, alpha_ref;
	int  depth_func;
	int  depth_mask;
	float depth_near, depth_far;
	int  depth_offset;
	int  shade;
	int  sc_x0, sc_y0, sc_x1, sc_y1;
	float vp_cx, vp_cy, vp_hw, vp_hh;
	int  of_x, of_y;
	float fog_start, fog_end;
	u32  fog_color;
	u32  fbmsk;
	float tex_su, tex_sv, tex_ou, tex_ov;
	gs_texture_t *texture;
	u32  dirty;
} gs_state_t;

static gs_state_t st;
static int draw_buffer;       // index into gs_video.fb_addr being drawn
static u32 clear_color;
static int clear_depth = 65535;

// guard band limits in NDC units, recomputed on viewport change
static float gb_x = 6.0f, gb_y = 6.0f;

static void state_recompute_guard_band(void)
{
	// GS primitive coordinates are 12.4 unsigned: keep 64px margin from the
	// 0..4096 range around the viewport centre.
	float lim_x = 2048.0f - 64.0f - fabsf(st.vp_cx - 2048.0f);
	float lim_y = 2048.0f - 64.0f - fabsf(st.vp_cy - 2048.0f);
	gb_x = st.vp_hw > 0.0f ? lim_x / st.vp_hw : 1.0f;
	gb_y = st.vp_hh > 0.0f ? lim_y / st.vp_hh : 1.0f;
	if (gb_x < 1.0f) gb_x = 1.0f;
	if (gb_y < 1.0f) gb_y = 1.0f;
}

static int gs_ztest_from(int func)
{
	// renderer: smaller is closer. GS: larger is closer (inverted storage).
	switch (func) {
	case GS_NEVER:   return 0;
	case GS_ALWAYS:  return 1;
	case GS_LESS:    return 3;   // GREATER
	case GS_LEQUAL:  return 2;   // GEQUAL
	case GS_EQUAL:   return 2;   // no EQUAL on GS, GEQUAL is the usual substitute
	default:         return 2;
	}
}

static int gs_atest_from(int func)
{
	switch (func) {
	case GS_NEVER:    return 0;
	case GS_ALWAYS:   return 1;
	case GS_LESS:     return 2;
	case GS_LEQUAL:   return 3;
	case GS_EQUAL:    return 4;
	case GS_GEQUAL:   return 5;
	case GS_GREATER:  return 6;
	case GS_NOTEQUAL: return 7;
	default:          return 1;
	}
}

static int tex_log2(int v)
{
	int l = 0;
	while ((1 << l) < v)
		l++;
	return l;
}

static void vram_ensure_resident(gs_texture_t *t);

static void state_flush(void)
{
	u32 d = st.dirty;
	gs_texture_t *t = st.caps[GS_CAP_TEXTURE_2D] ? st.texture : NULL;

	if (t)
		vram_ensure_resident(t);   // may emit an upload

	d = st.dirty;
	if (!d)
		return;

	ad_begin(16);
	if (d & DIRTY_FRAME) {
		ad_write(GS_REG_FRAME_1, GS_SET_FRAME(gs_video.fb_addr[draw_buffer] >> 11,
			gs_video.width >> 6, gs_video.fb_psm, st.fbmsk));
	}
	if (d & DIRTY_ZBUF) {
		ad_write(GS_REG_ZBUF_1, GS_SET_ZBUF(gs_video.z_addr >> 11, gs_video.z_psm & 0xf,
			st.depth_mask || !st.caps[GS_CAP_DEPTH_TEST] ? 1 : 0));
	}
	if (d & DIRTY_TEST) {
		int ate = st.caps[GS_CAP_ALPHA_TEST] ? 1 : 0;
		int ztst = st.caps[GS_CAP_DEPTH_TEST] ? gs_ztest_from(st.depth_func) : 1;
		// Alpha 0x80 == 1.0 on the GS.
		int aref = (st.alpha_ref + 1) >> 1;
		ad_write(GS_REG_TEST_1, GS_SET_TEST(ate, gs_atest_from(st.alpha_func), aref, 0, 0, 0, 1, ztst));
	}
	if (d & DIRTY_ALPHA) {
		ad_write(GS_REG_ALPHA_1, GS_SET_ALPHA(st.blend_a, st.blend_b, st.blend_c, st.blend_d, st.blend_fix));
	}
	if ((d & DIRTY_TEX0) && t) {
		int tfx = st.tfx == GS_TFX_MODULATE ? 0 : 1;   // 0 MODULATE, 1 DECAL
		int clut = (t->psm == GS_TEX_T8 || t->psm == GS_TEX_T4);
		int cbp = clut ? t->vram_block + t->clut_offset : 0;
		ad_write(GS_REG_TEX0_1, GS_SET_TEX0(t->vram_block, t->tbw, t->psm, t->tw, t->th,
			st.tcc, tfx, cbp, 0, 0, 0, clut ? 1 : 0));
	}
	if (d & DIRTY_TEX1) {
		ad_write(GS_REG_TEX1_1, GS_SET_TEX1(0, 0, st.mag_filter, st.min_filter, 0, 0, 0));
	}
	if ((d & DIRTY_CLAMP) && t) {
		ad_write(GS_REG_CLAMP_1, GS_SET_CLAMP(st.wrap_u, st.wrap_v, 0, (1 << t->tw) - 1, 0, (1 << t->th) - 1));
	}
	if (d & DIRTY_SCISSOR) {
		if (st.caps[GS_CAP_SCISSOR_TEST])
			ad_write(GS_REG_SCISSOR_1, GS_SET_SCISSOR(st.sc_x0, st.sc_x1, st.sc_y0, st.sc_y1));
		else
			ad_write(GS_REG_SCISSOR_1, GS_SET_SCISSOR(0, gs_video.width - 1, 0, gs_video.height - 1));
	}
	if (d & DIRTY_XYOFFSET) {
		ad_write(GS_REG_XYOFFSET_1, GS_SET_XYOFFSET(st.of_x << 4, st.of_y << 4));
	}
	if (d & DIRTY_FOGCOL) {
		ad_write(GS_REG_FOGCOL, GS_SET_FOGCOL(st.fog_color & 0xff, (st.fog_color >> 8) & 0xff, (st.fog_color >> 16) & 0xff));
	}
	if (d & DIRTY_MISC) {
		ad_write(GS_REG_PRMODECONT, 1);
		ad_write(GS_REG_COLCLAMP, GS_SET_COLCLAMP(1));
		ad_write(GS_REG_DTHE, GS_SET_DTHE(st.caps[GS_CAP_DITHER] ? 1 : 0));
		// CT16 texture alpha expansion: A=0 -> 0, A=1 -> 0x80 (1.0)
		ad_write(GS_REG_TEXA, GS_SET_TEXA(0x00, 0, 0x80));
		ad_write(GS_REG_PABE, GS_SET_PABE(0));
		ad_write(GS_REG_FBA_1, GS_SET_FBA(0));
	}
	ad_end();
	st.dirty = 0;
}

void GS_Enable(int cap)
{
	if (cap < 0 || cap >= GS_CAP_COUNT || st.caps[cap])
		return;
	st.caps[cap] = 1;
	switch (cap) {
	case GS_CAP_DEPTH_TEST: st.dirty |= DIRTY_TEST | DIRTY_ZBUF; break;
	case GS_CAP_ALPHA_TEST: st.dirty |= DIRTY_TEST; break;
	case GS_CAP_TEXTURE_2D: st.dirty |= DIRTY_TEX0 | DIRTY_TEX1 | DIRTY_CLAMP; break;
	case GS_CAP_SCISSOR_TEST: st.dirty |= DIRTY_SCISSOR; break;
	case GS_CAP_DITHER: st.dirty |= DIRTY_MISC; break;
	default: break;
	}
}

void GS_Disable(int cap)
{
	if (cap < 0 || cap >= GS_CAP_COUNT || !st.caps[cap])
		return;
	st.caps[cap] = 0;
	switch (cap) {
	case GS_CAP_DEPTH_TEST: st.dirty |= DIRTY_TEST | DIRTY_ZBUF; break;
	case GS_CAP_ALPHA_TEST: st.dirty |= DIRTY_TEST; break;
	case GS_CAP_SCISSOR_TEST: st.dirty |= DIRTY_SCISSOR; break;
	case GS_CAP_DITHER: st.dirty |= DIRTY_MISC; break;
	default: break;
	}
}

int GS_IsEnabled(int cap)
{
	return (cap >= 0 && cap < GS_CAP_COUNT) ? st.caps[cap] : 0;
}

void GS_Color(u32 abgr) { st.color = abgr; }

void GS_TexFunc(int tfx, int tcc)
{
	if (st.tfx != tfx || st.tcc != tcc) {
		st.tfx = tfx;
		st.tcc = tcc;
		st.dirty |= DIRTY_TEX0;
	}
}

void GS_TexFilter(int min, int mag)
{
	min = min ? 1 : 0;
	mag = mag ? 1 : 0;
	if (st.min_filter != min || st.mag_filter != mag) {
		st.min_filter = min;
		st.mag_filter = mag;
		st.dirty |= DIRTY_TEX1;
	}
}

void GS_TexWrap(int u, int v)
{
	if (st.wrap_u != u || st.wrap_v != v) {
		st.wrap_u = u;
		st.wrap_v = v;
		st.dirty |= DIRTY_CLAMP;
	}
}

void GS_BlendRaw(int a, int b, int c, int d, int fix)
{
	if (st.blend_a != a || st.blend_b != b || st.blend_c != c || st.blend_d != d || st.blend_fix != fix) {
		st.blend_a = a; st.blend_b = b; st.blend_c = c; st.blend_d = d; st.blend_fix = fix;
		st.dirty |= DIRTY_ALPHA;
	}
}

// GS blend: ((A - B) * C >> 7) + D
//   A,B,D: 0 = Cs, 1 = Cd, 2 = 0
//   C:     0 = As, 1 = Ad, 2 = FIX
// Only equations of the form (Cs - Cd)*a + Cd and Cs*a + Cd*b (with one of
// the factors 1 or 0) are representable; everything else is mapped to the
// closest representable equation and reported once.
void GS_BlendFunc(int op, int src, int dst, u32 srcfix, u32 dstfix)
{
	static int warned;
	(void)op;

	if (src == GS_SRC_ALPHA && dst == GS_ONE_MINUS_SRC_ALPHA) {
		GS_BlendRaw(0, 1, 0, 1, 0);                  // (Cs - Cd) * As + Cd
	} else if (src == GS_SRC_ALPHA && (dst == GS_FIX && (dstfix & 0xffffff) == 0xffffff)) {
		GS_BlendRaw(0, 2, 0, 1, 0);                  // Cs * As + Cd  (additive)
	} else if (src == GS_FIX && dst == GS_FIX) {
		int sf = (int)(srcfix & 0xff), df = (int)(dstfix & 0xff);
		if (df >= 0xff) {
			GS_BlendRaw(0, 2, 2, 1, (sf + 1) >> 1);  // Cs * F + Cd
		} else if (sf >= 0xff && df == 0) {
			GS_BlendRaw(0, 2, 2, 2, 0x80);           // Cs
		} else if (sf == 0 && df == 0) {
			GS_BlendRaw(2, 2, 2, 2, 0);              // 0
		} else if (sf + df == 0xff || sf + df == 0x100) {
			GS_BlendRaw(0, 1, 2, 1, (sf + 1) >> 1);  // (Cs - Cd) * F + Cd
		} else {
			GS_BlendRaw(0, 2, 2, 1, (sf + 1) >> 1);
			if (!warned++) PS2_Log("GS: approximated FIX blend %06x/%06x\n", srcfix, dstfix);
		}
	} else if (src == GS_FIX && dst == GS_ONE_MINUS_SRC_ALPHA) {
		GS_BlendRaw(0, 1, 0, 1, 0);
	} else if (src == GS_ONE_MINUS_SRC_ALPHA && dst == GS_SRC_ALPHA) {
		GS_BlendRaw(1, 0, 0, 0, 0);                  // (Cd - Cs) * As + Cs
	} else if (src == GS_DST_COLOR || dst == GS_SRC_COLOR) {
		// Colour multiply is not expressible by the GS blend unit. Callers
		// that need it (lightmaps) use the dedicated alpha-channel path; here
		// fall back to a darkening approximation using source alpha.
		GS_BlendRaw(1, 2, 0, 2, 0);                  // Cd * As
		if (!(warned & 2)) { warned |= 2; PS2_Log("GS: colour-multiply blend approximated with Cd*As\n"); }
	} else {
		GS_BlendRaw(0, 1, 0, 1, 0);
		if (!(warned & 4)) { warned |= 4; PS2_Log("GS: unsupported blend %d/%d, using alpha blend\n", src, dst); }
	}
}

void GS_AlphaFunc(int func, int ref, int mask)
{
	(void)mask;
	if (st.alpha_func != func || st.alpha_ref != ref) {
		st.alpha_func = func;
		st.alpha_ref = ref & 0xff;
		st.dirty |= DIRTY_TEST;
	}
}

void GS_DepthFunc(int func)
{
	if (st.depth_func != func) {
		st.depth_func = func;
		st.dirty |= DIRTY_TEST;
	}
}

void GS_DepthMask(int mask)
{
	mask = mask ? 1 : 0;
	if (st.depth_mask != mask) {
		st.depth_mask = mask;
		st.dirty |= DIRTY_ZBUF;
	}
}

void GS_DepthRange(int near_z, int far_z)
{
	st.depth_near = (float)near_z;
	st.depth_far  = (float)far_z;
}

void GS_DepthOffset(int offset) { st.depth_offset = offset; }

void GS_ShadeModel(int mode) { st.shade = mode; }

void GS_Scissor(int x0, int y0, int x1, int y1)
{
	// GU semantics: x1/y1 exclusive
	if (x1 > gs_video.width) x1 = gs_video.width;
	if (y1 > gs_video.height) y1 = gs_video.height;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 <= x0) x1 = x0 + 1;
	if (y1 <= y0) y1 = y0 + 1;
	st.sc_x0 = x0; st.sc_y0 = y0; st.sc_x1 = x1 - 1; st.sc_y1 = y1 - 1;
	st.dirty |= DIRTY_SCISSOR;
}

void GS_Viewport(int cx, int cy, int w, int h)
{
	st.vp_cx = (float)cx;
	st.vp_cy = (float)cy;
	st.vp_hw = (float)w * 0.5f;
	st.vp_hh = (float)h * 0.5f;
	state_recompute_guard_band();
}

void GS_Offset(int x, int y)
{
	if (st.of_x != x || st.of_y != y) {
		st.of_x = x;
		st.of_y = y;
		st.dirty |= DIRTY_XYOFFSET;
	}
}

void GS_Fog(float start, float end, u32 color)
{
	st.fog_start = start;
	st.fog_end = end;
	if (st.fog_color != (color & 0xffffff)) {
		st.fog_color = color & 0xffffff;
		st.dirty |= DIRTY_FOGCOL;
	}
}

void GS_TexScale(float su, float sv) { st.tex_su = su; st.tex_sv = sv; }
void GS_TexOffset(float ou, float ov) { st.tex_ou = ou; st.tex_ov = ov; }

void GS_FrameMask(u32 fbmsk)
{
	if (st.fbmsk != fbmsk) {
		st.fbmsk = fbmsk;
		st.dirty |= DIRTY_FRAME;
	}
}

// ===========================================================================
// Matrices
// ===========================================================================

#define MATRIX_STACK_DEPTH 16

static gs_mat4_t mstack[3][MATRIX_STACK_DEPTH];
static int       mdepth[3];
static int       mmode = GS_MODEL;
static gs_mat4_t mvp;
static int       mvp_dirty = 1;

static void mat_identity(gs_mat4_t *m)
{
	memset(m, 0, sizeof(*m));
	m->x.x = m->y.y = m->z.z = m->w.w = 1.0f;
}

// r = a * b (column-vector convention, like GU/GL)
static void mat_mul(gs_mat4_t *r, const gs_mat4_t *a, const gs_mat4_t *b)
{
	const float *A = (const float *)a;
	const float *B = (const float *)b;
	float R[16];
	int c, rr;
	for (c = 0; c < 4; c++) {
		for (rr = 0; rr < 4; rr++) {
			R[c * 4 + rr] = A[0 * 4 + rr] * B[c * 4 + 0] + A[1 * 4 + rr] * B[c * 4 + 1] +
			                A[2 * 4 + rr] * B[c * 4 + 2] + A[3 * 4 + rr] * B[c * 4 + 3];
		}
	}
	memcpy(r, R, sizeof(R));
}

static inline gs_mat4_t *mtop(void) { return &mstack[mmode][mdepth[mmode]]; }

static void mat_post(const gs_mat4_t *m)
{
	gs_mat4_t *t = mtop();
	mat_mul(t, t, m);
	mvp_dirty = 1;
}

void GS_MatrixMode(int mode) { if (mode >= 0 && mode < 3) mmode = mode; }
void GS_LoadIdentity(void) { mat_identity(mtop()); mvp_dirty = 1; }
void GS_LoadMatrix(const gs_mat4_t *m) { *mtop() = *m; mvp_dirty = 1; }
void GS_StoreMatrix(gs_mat4_t *m) { *m = *mtop(); }
void GS_MultMatrix(const gs_mat4_t *m) { mat_post(m); }

void GS_PushMatrix(void)
{
	if (mdepth[mmode] + 1 >= MATRIX_STACK_DEPTH)
		PS2_Fatal("GS: matrix stack overflow (mode %d)", mmode);
	mstack[mmode][mdepth[mmode] + 1] = mstack[mmode][mdepth[mmode]];
	mdepth[mmode]++;
}

void GS_PopMatrix(void)
{
	if (mdepth[mmode] == 0)
		PS2_Fatal("GS: matrix stack underflow (mode %d)", mmode);
	mdepth[mmode]--;
	mvp_dirty = 1;
}

void GS_Translate(const gs_vec3_t *v)
{
	gs_mat4_t m;
	mat_identity(&m);
	m.w.x = v->x; m.w.y = v->y; m.w.z = v->z;
	mat_post(&m);
}

void GS_Scale(const gs_vec3_t *v)
{
	gs_mat4_t m;
	mat_identity(&m);
	m.x.x = v->x; m.y.y = v->y; m.z.z = v->z;
	mat_post(&m);
}

void GS_RotateX(float a)
{
	gs_mat4_t m;
	float c = cosf(a), s = sinf(a);
	mat_identity(&m);
	m.y.y = c; m.y.z = s;
	m.z.y = -s; m.z.z = c;
	mat_post(&m);
}

void GS_RotateY(float a)
{
	gs_mat4_t m;
	float c = cosf(a), s = sinf(a);
	mat_identity(&m);
	m.x.x = c; m.x.z = -s;
	m.z.x = s; m.z.z = c;
	mat_post(&m);
}

void GS_RotateZ(float a)
{
	gs_mat4_t m;
	float c = cosf(a), s = sinf(a);
	mat_identity(&m);
	m.x.x = c; m.x.y = s;
	m.y.x = -s; m.y.y = c;
	mat_post(&m);
}

void GS_RotateXYZ(const gs_vec3_t *v) { GS_RotateX(v->x); GS_RotateY(v->y); GS_RotateZ(v->z); }
void GS_RotateZYX(const gs_vec3_t *v) { GS_RotateZ(v->z); GS_RotateY(v->y); GS_RotateX(v->x); }

void GS_Perspective(float fovy, float aspect, float znear, float zfar)
{
	gs_mat4_t m;
	float angle = (fovy * 0.5f) * (3.14159265f / 180.0f);
	float cotangent = cosf(angle) / sinf(angle);
	float delta = znear - zfar;
	memset(&m, 0, sizeof(m));
	m.x.x = cotangent / aspect;
	m.y.y = cotangent;
	m.z.z = (zfar + znear) / delta;
	m.z.w = -1.0f;
	m.w.z = 2.0f * (zfar * znear) / delta;
	mat_post(&m);
}

void GS_Ortho(float left, float right, float bottom, float top, float znear, float zfar)
{
	gs_mat4_t m;
	float dx = right - left, dy = top - bottom, dz = zfar - znear;
	mat_identity(&m);
	m.x.x = 2.0f / dx;
	m.w.x = -(right + left) / dx;
	m.y.y = 2.0f / dy;
	m.w.y = -(top + bottom) / dy;
	m.z.z = -2.0f / dz;
	m.w.z = -(zfar + znear) / dz;
	mat_post(&m);
}

void GS_UpdateMatrix(void)
{
	if (mvp_dirty) {
		gs_mat4_t vm;
		mat_mul(&vm, &mstack[GS_VIEW][mdepth[GS_VIEW]], &mstack[GS_MODEL][mdepth[GS_MODEL]]);
		mat_mul(&mvp, &mstack[GS_PROJECTION][mdepth[GS_PROJECTION]], &vm);
		mvp_dirty = 0;
	}
}

// ===========================================================================
// Transient vertex arena
// ===========================================================================

// Busy world frames can transiently need well over 160 KiB of source vertices.
// Keep this fixed-size (no per-frame malloc) but leave enough headroom for dense
// scenes; 512 KiB is a small bounded cost relative to the PS2 EE free-memory margin.
#define ARENA_SIZE (512 * 1024)
static u8  arena[ARENA_SIZE] __attribute__((aligned(64)));
static int arena_used;
static int arena_outstanding;   // allocations not yet consumed by a draw

void *GS_GetMemory(int size)
{
	void *p;
	size = (size + 15) & ~15;
	if (arena_used + size > ARENA_SIZE) {
		if (arena_outstanding)
			PS2_Fatal("GS: transient vertex arena exhausted (%d bytes requested, %d used)", size, arena_used);
		arena_used = 0;   // everything handed out so far has been drawn
	}
	if (size > ARENA_SIZE)
		PS2_Fatal("GS: transient allocation of %d bytes too large", size);
	p = arena + arena_used;
	arena_used += size;
	arena_outstanding = 1;
	if ((u32)arena_used > gs_stats.arena_peak)
		gs_stats.arena_peak = arena_used;
	return p;
}

// ===========================================================================
// Primitive emission (REGLIST)
// ===========================================================================

// Open REGLIST tag state: list primitives (triangles, sprites, lines) keep
// their tag open across draws with identical PRIM/register layout.
static u64 *gt_tag;
static int  gt_nreg;
static int  gt_count;       // vertices written
static u64  gt_prim;
static u64  gt_regs;
static int  gt_list;        // 1 if this tag can be continued by later draws
static int  gt_multiple;    // vertices per primitive (for splitting)

static void gif_tag_close(void)
{
	if (!gt_tag)
		return;
	if (gt_count == 0) {
		pkt_ptr = gt_tag;
	} else {
		int words = gt_count * gt_nreg;
		// PRE/PRIM are only honoured in PACKED mode; PRIM was written via A+D
		// just before this tag (see gif_tag_begin).
		gt_tag[0] = GIF_SET_TAG(gt_count, 1, 0, 0, GIF_FLG_REGLIST, gt_nreg);
		gt_tag[1] = gt_regs;
		if (words & 1) {
			pkt_ptr[0] = 0;  // pad to a qword; ignored by the GIF
			pkt_ptr++;
		}
		gs_stats.verts_out += gt_count;
	}
	gt_tag = NULL;
	gt_count = 0;
}

// Begin (or continue) a REGLIST tag with room for `nverts` vertices.
static void gif_tag_begin(u64 prim, u64 regs, int nreg, int nverts, int list, int multiple)
{
	int qw = (nverts * nreg + 1) / 2;
	if (gt_tag && gt_list && list && gt_prim == prim && gt_regs == regs &&
	    gt_count + nverts <= 32000 && pkt_ptr + (qw + 2) * 2 <= pkt_end) {
		return;
	}
	gif_tag_close();
	pkt_reserve(qw + 3);
	// The GIF ignores the PRIM field of REGLIST tags, so start the primitive
	// with an explicit A+D write (this also resets the GS vertex queue).
	pkt_ptr[0] = GIF_SET_TAG(1, 1, 0, 0, GIF_FLG_PACKED, 1);
	pkt_ptr[1] = GIF_AD;
	pkt_ptr[2] = prim;
	pkt_ptr[3] = GS_REG_PRIM;
	pkt_ptr += 4;
	gt_tag = pkt_ptr;
	pkt_ptr += 2;
	gt_prim = prim;
	gt_regs = regs;
	gt_nreg = nreg;
	gt_count = 0;
	gt_list = list;
	gt_multiple = multiple;
}

// ---------------------------------------------------------------------------
// Vertex formats
// ---------------------------------------------------------------------------

typedef struct {
	float x, y, z, w;
	float s, t;
	float fog;       // distance used for fog (eye space w)
	u32   color;
	int   out;       // outcodes
} cvert_t;

#define OUT_NEAR   1
#define OUT_LEFT   2
#define OUT_RIGHT  4
#define OUT_BOTTOM 8
#define OUT_TOP    16
#define OUT_FAR    32

typedef struct {
	int stride;
	int tex_off, tex_type;    // 0 none, 1 s16, 2 float
	int col_off;              // -1 none
	int pos_off, pos_type;    // 1 s8, 2 s16, 3 float
} vfmt_t;

static int align_up(int v, int a) { return (v + a - 1) & ~(a - 1); }

static void vfmt_decode(int vtype, vfmt_t *f)
{
	int off = 0, maxalign = 1;
	f->tex_type = 0; f->tex_off = 0; f->col_off = -1; f->pos_type = 0; f->pos_off = 0;
	if (vtype & GS_TEXTURE_16BIT) { off = align_up(off, 2); f->tex_type = 1; f->tex_off = off; off += 4; if (maxalign < 2) maxalign = 2; }
	else if (vtype & GS_TEXTURE_32BITF) { off = align_up(off, 4); f->tex_type = 2; f->tex_off = off; off += 8; maxalign = 4; }
	if (vtype & GS_COLOR_8888) { off = align_up(off, 4); f->col_off = off; off += 4; maxalign = 4; }
	if (vtype & GS_VERTEX_8BIT) { f->pos_type = 1; f->pos_off = off; off += 3; }
	else if (vtype & GS_VERTEX_16BIT) { off = align_up(off, 2); f->pos_type = 2; f->pos_off = off; off += 6; if (maxalign < 2) maxalign = 2; }
	else { off = align_up(off, 4); f->pos_type = 3; f->pos_off = off; off += 12; maxalign = 4; }
	f->stride = align_up(off, maxalign);
}

static inline void vfmt_fetch(const vfmt_t *f, const u8 *v, int is2d, float *px, float *py, float *pz, float *ps, float *pt, u32 *pc)
{
	float tscale = is2d ? 1.0f : (1.0f / 32768.0f);
	if (f->tex_type == 1) {
		const s16 *uv = (const s16 *)(v + f->tex_off);
		*ps = uv[0] * tscale; *pt = uv[1] * tscale;
	} else if (f->tex_type == 2) {
		const float *uv = (const float *)(v + f->tex_off);
		*ps = uv[0]; *pt = uv[1];
	} else {
		*ps = *pt = 0.0f;
	}
	*pc = f->col_off >= 0 ? *(const u32 *)(v + f->col_off) : st.color;
	if (f->pos_type == 3) {
		const float *p = (const float *)(v + f->pos_off);
		*px = p[0]; *py = p[1]; *pz = p[2];
	} else if (f->pos_type == 2) {
		const s16 *p = (const s16 *)(v + f->pos_off);
		float sc = is2d ? 1.0f : (1.0f / 32768.0f);
		*px = p[0] * sc; *py = p[1] * sc; *pz = is2d ? (float)(u16)p[2] : p[2] * sc;
	} else {
		const s8 *p = (const s8 *)(v + f->pos_off);
		float sc = is2d ? 1.0f : (1.0f / 128.0f);
		*px = p[0] * sc; *py = p[1] * sc; *pz = p[2] * sc;
	}
}

// ---------------------------------------------------------------------------
// Colour conversion: GS treats 0x80 as 1.0 for texture modulation and for
// alpha in the blend equation.
// ---------------------------------------------------------------------------
static int color_halve_rgb;

static inline u64 gs_rgbaq(u32 c, float q)
{
	u32 r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff, a = c >> 24;
	union { float f; u32 u; } qq;
	if (color_halve_rgb) { r = (r + 1) >> 1; g = (g + 1) >> 1; b = (b + 1) >> 1; }
	a = (a + 1) >> 1;
	qq.f = q;
	return (u64)(r | (g << 8) | (b << 16) | (a << 24)) | ((u64)qq.u << 32);
}

// ---------------------------------------------------------------------------
// Per-draw setup
// ---------------------------------------------------------------------------
typedef struct {
	int textured;
	int fog;
	int is2d;
	u64 regs;
	int nreg;
	u64 prim_flags;   // IIP/TME/FGE/ABE/FST bits (without primitive type)
	float zscale, zbias;
	float tex_bias_s, tex_bias_t;
} draw_ctx_t;

static draw_ctx_t dc;

#define REG_ST    GIF_REG_ST
#define REG_UV    GIF_REG_UV
#define REG_RGBAQ GIF_REG_RGBAQ
#define REG_XYZ2  GIF_REG_XYZ2
#define REG_XYZF2 GIF_REG_XYZF2

static void draw_ctx_setup(int vtype)
{
	int iip = st.shade == GS_SMOOTH ? 1 : 0;
	dc.textured = st.caps[GS_CAP_TEXTURE_2D] && st.texture && (vtype & (GS_TEXTURE_16BIT | GS_TEXTURE_32BITF));
	dc.is2d = (vtype & GS_TRANSFORM_2D) ? 1 : 0;
	dc.fog = (!dc.is2d && st.caps[GS_CAP_FOG] && st.fog_end > st.fog_start) ? 1 : 0;
	color_halve_rgb = dc.textured && st.tfx == GS_TFX_MODULATE;

	if (dc.textured) {
		dc.nreg = 3;
		dc.regs = (u64)(dc.is2d ? REG_UV : REG_ST) | ((u64)REG_RGBAQ << 4) | ((u64)(dc.fog ? REG_XYZF2 : REG_XYZ2) << 8);
	} else {
		dc.nreg = 2;
		dc.regs = (u64)REG_RGBAQ | ((u64)(dc.fog ? REG_XYZF2 : REG_XYZ2) << 4);
	}
	dc.prim_flags = ((u64)iip << 3) | ((u64)dc.textured << 4) | ((u64)dc.fog << 5) |
	                ((u64)(st.caps[GS_CAP_BLEND] ? 1 : 0) << 6) | ((u64)(dc.is2d && dc.textured ? 1 : 0) << 8);
	// depth range mapping: ndc z in [-1, 1] -> [near, far] (renderer units)
	dc.zscale = (st.depth_far - st.depth_near) * 0.5f;
	dc.zbias  = (st.depth_far + st.depth_near) * 0.5f + (float)st.depth_offset;
	dc.tex_bias_s = 0.0f;
	dc.tex_bias_t = 0.0f;

	if (!dc.textured && st.caps[GS_CAP_TEXTURE_2D])
		;  // texture state is irrelevant for this draw
	state_flush();
}

// Emit one already-clipped, projected vertex.
static inline void emit_vertex(const cvert_t *v)
{
	u64 *p = pkt_ptr;
	float invw = 1.0f / v->w;
	float sx = st.vp_cx + v->x * invw * st.vp_hw;
	float sy = st.vp_cy - v->y * invw * st.vp_hh;
	float d  = v->z * invw * dc.zscale + dc.zbias;
	int ix = (int)(sx * 16.0f + 0.5f);
	int iy = (int)(sy * 16.0f + 0.5f);
	int gz;
	if (d < 0.0f) d = 0.0f;
	if (d > 65535.0f) d = 65535.0f;
	gz = 65535 - (int)d;
	if (dc.textured) {
		union { float f; u32 u; } s, t;
		s.f = v->s * invw;
		t.f = v->t * invw;
		*p++ = (u64)s.u | ((u64)t.u << 32);
	}
	*p++ = gs_rgbaq(v->color, invw);
	if (dc.fog) {
		float f = (st.fog_end - v->fog) / (st.fog_end - st.fog_start);
		int fi;
		if (f < 0.0f) f = 0.0f;
		if (f > 1.0f) f = 1.0f;
		fi = (int)(f * 255.0f);
		*p++ = (u64)(ix & 0xffff) | ((u64)(iy & 0xffff) << 16) | ((u64)(gz & 0xffffff) << 32) | ((u64)fi << 56);
	} else {
		*p++ = (u64)(ix & 0xffff) | ((u64)(iy & 0xffff) << 16) | ((u64)gz << 32);
	}
	pkt_ptr = p;
	gt_count++;
}

static inline void emit_vertex_2d(float x, float y, float z, float s, float t, u32 c)
{
	u64 *p = pkt_ptr;
	int ix = (int)((x + (float)st.of_x) * 16.0f);
	int iy = (int)((y + (float)st.of_y) * 16.0f);
	int gz = 65535 - (int)z;
	if (gz < 0) gz = 0;
	if (gz > 65535) gz = 65535;
	if (dc.textured) {
		int u = (int)(s * 16.0f), vv = (int)(t * 16.0f);
		*p++ = (u64)(u & 0x3fff) | ((u64)(vv & 0x3fff) << 16);
	}
	*p++ = gs_rgbaq(c, 1.0f);
	*p++ = (u64)(ix & 0xffff) | ((u64)(iy & 0xffff) << 16) | ((u64)gz << 32);
	pkt_ptr = p;
	gt_count++;
}

// ---------------------------------------------------------------------------
// Clipping
// ---------------------------------------------------------------------------
#define CLIP_MAX 24

static inline int outcode(const cvert_t *v)
{
	int o = 0;
	float w = v->w;
	if (v->z < -w) o |= OUT_NEAR;
	if (v->z >  w) o |= OUT_FAR;
	if (v->x < -gb_x * w) o |= OUT_LEFT;
	if (v->x >  gb_x * w) o |= OUT_RIGHT;
	if (v->y < -gb_y * w) o |= OUT_BOTTOM;
	if (v->y >  gb_y * w) o |= OUT_TOP;
	return o;
}

static inline float plane_dist(const cvert_t *v, int plane)
{
	switch (plane) {
	case OUT_NEAR:   return v->z + v->w;
	case OUT_FAR:    return v->w - v->z;
	case OUT_LEFT:   return v->x + gb_x * v->w;
	case OUT_RIGHT:  return gb_x * v->w - v->x;
	case OUT_BOTTOM: return v->y + gb_y * v->w;
	default:         return gb_y * v->w - v->y;
	}
}

static inline u32 lerp_color(u32 a, u32 b, float t)
{
	u32 r = 0;
	int sh;
	for (sh = 0; sh < 32; sh += 8) {
		float ca = (float)((a >> sh) & 0xff), cb = (float)((b >> sh) & 0xff);
		int c = (int)(ca + (cb - ca) * t + 0.5f);
		if (c < 0) c = 0;
		if (c > 255) c = 255;
		r |= (u32)c << sh;
	}
	return r;
}

static int clip_polygon(cvert_t *in, int n, int planes, cvert_t *out)
{
	cvert_t tmp[CLIP_MAX];
	cvert_t *src = in, *dst = tmp;
	int p;
	for (p = OUT_NEAR; p <= OUT_FAR; p <<= 1) {
		int i, m = 0;
		if (!(planes & p))
			continue;
		for (i = 0; i < n; i++) {
			const cvert_t *a = &src[i];
			const cvert_t *b = &src[(i + 1) % n];
			float da = plane_dist(a, p), db = plane_dist(b, p);
			if (da >= 0.0f) {
				if (m >= CLIP_MAX - 1) return 0;
				dst[m++] = *a;
			}
			if ((da >= 0.0f) != (db >= 0.0f)) {
				float t = da / (da - db);
				cvert_t *c = &dst[m++];
				if (m >= CLIP_MAX) return 0;
				c->x = a->x + (b->x - a->x) * t;
				c->y = a->y + (b->y - a->y) * t;
				c->z = a->z + (b->z - a->z) * t;
				c->w = a->w + (b->w - a->w) * t;
				c->s = a->s + (b->s - a->s) * t;
				c->t = a->t + (b->t - a->t) * t;
				c->fog = a->fog + (b->fog - a->fog) * t;
				c->color = lerp_color(a->color, b->color, t);
			}
		}
		n = m;
		if (n < 3)
			return 0;
		src = dst;
		dst = (dst == tmp) ? out : tmp;
	}
	if (src != out)
		memcpy(out, src, sizeof(cvert_t) * n);
	return n;
}

// Screen-space signed area of the first non-degenerate triangle.
// Returns >0 / <0 / 0 (degenerate).
static float poly_facing(const cvert_t *v, int n)
{
	int i;
	for (i = 2; i < n; i++) {
		float ax = v[0].x / v[0].w, ay = v[0].y / v[0].w;
		float bx = v[i - 1].x / v[i - 1].w, by = v[i - 1].y / v[i - 1].w;
		float cx = v[i].x / v[i].w, cy = v[i].y / v[i].w;
		float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
		if (area > 1e-9f || area < -1e-9f)
			return area;
	}
	return 0.0f;
}

// Culling convention: the PSP renderer uses sceGuFrontFace(GU_CW) with
// culling enabled. In NDC (y up) GU_CW front faces have negative area.
int gs_cull_sign = 1;

// Draw a convex polygon (vertices in clip space). `ormask` / `andmask` are
// the OR / AND of all vertex outcodes.
static void draw_polygon(cvert_t *v, int n, int ormask, int andmask)
{
	cvert_t clipped[CLIP_MAX];
	int i;

	if (andmask) {
		gs_stats.prims_culled++;
		return;
	}
	if (ormask) {
		n = clip_polygon(v, n, ormask, clipped);
		if (n < 3) {
			gs_stats.prims_culled++;
			return;
		}
		v = clipped;
		gs_stats.prims_clipped++;
	}
	if (st.caps[GS_CAP_CULL_FACE]) {
		float a = poly_facing(v, n);
		if (a == 0.0f || (a * (float)gs_cull_sign) > 0.0f) {
			gs_stats.prims_culled++;
			return;
		}
	}
	if (n == 3) {
		gif_tag_begin(GS_PRIM_TRIANGLE | dc.prim_flags, dc.regs, dc.nreg, 3, 1, 3);
		emit_vertex(&v[0]);
		emit_vertex(&v[1]);
		emit_vertex(&v[2]);
	} else {
		gif_tag_begin(GS_PRIM_TRIANGLE_FAN | dc.prim_flags, dc.regs, dc.nreg, n, 0, 1);
		for (i = 0; i < n; i++)
			emit_vertex(&v[i]);
		gif_tag_close();
	}
}

// ---------------------------------------------------------------------------
// GS_DrawArray
// ---------------------------------------------------------------------------
#define XFORM_BATCH 512
static cvert_t xf[XFORM_BATCH];

static inline void transform_vertex(const vfmt_t *f, const u8 *src, cvert_t *o)
{
	float x, y, z, s, t;
	u32 c;
	const float *m = (const float *)&mvp;
	vfmt_fetch(f, src, 0, &x, &y, &z, &s, &t, &c);
	o->x = m[0] * x + m[4] * y + m[8]  * z + m[12];
	o->y = m[1] * x + m[5] * y + m[9]  * z + m[13];
	o->z = m[2] * x + m[6] * y + m[10] * z + m[14];
	o->w = m[3] * x + m[7] * y + m[11] * z + m[15];
	o->s = s * st.tex_su + st.tex_ou - dc.tex_bias_s;
	o->t = t * st.tex_sv + st.tex_ov - dc.tex_bias_t;
	o->color = c;
	o->fog = o->w;
	o->out = outcode(o);
}

static inline const u8 *vtx_ptr(const vfmt_t *f, const void *verts, const void *indices, int i)
{
	int idx = indices ? ((const u16 *)indices)[i] : i;
	return (const u8 *)verts + idx * f->stride;
}

static void draw_2d(int prim, const vfmt_t *f, int count, const void *indices, const void *vertices)
{
	int i, per;
	u64 gsprim;
	switch (prim) {
	case GS_SPRITES:        gsprim = GS_PRIM_SPRITE; per = 2; break;
	case GS_TRIANGLES:      gsprim = GS_PRIM_TRIANGLE; per = 3; break;
	case GS_TRIANGLE_STRIP: gsprim = GS_PRIM_TRIANGLE_STRIP; per = 0; break;
	case GS_TRIANGLE_FAN:   gsprim = GS_PRIM_TRIANGLE_FAN; per = 0; break;
	case GS_LINES:          gsprim = GS_PRIM_LINE; per = 2; break;
	case GS_LINE_STRIP:     gsprim = GS_PRIM_LINE_STRIP; per = 0; break;
	default:                gsprim = GS_PRIM_POINT; per = 1; break;
	}
	// 2D sprites never need Q; keep flat colour unless gouraud requested.
	i = 0;
	while (i < count) {
		int n = count - i;
		int j;
		if (per) {
			int cap = 30000 - (30000 % per);
			if (n > cap) n = cap;
			n -= n % per;
			if (n <= 0) break;
		} else if (n > 30000) {
			PS2_Fatal("GS: 2D strip/fan too long (%d)", count);
		}
		gif_tag_begin(gsprim | dc.prim_flags, dc.regs, dc.nreg, n, per ? 1 : 0, per ? per : 1);
		for (j = 0; j < n; j++) {
			float x, y, z, s, t;
			u32 c;
			vfmt_fetch(f, vtx_ptr(f, vertices, indices, i + j), 1, &x, &y, &z, &s, &t, &c);
			emit_vertex_2d(x, y, z, s, t, c);
		}
		if (!per)
			gif_tag_close();
		i += n;
	}
}

static void draw_3d_lines(int prim, const vfmt_t *f, int count, const void *indices, const void *vertices)
{
	int i, step = (prim == GS_LINES) ? 2 : 1;
	for (i = 0; i + 1 < count; i += step) {
		cvert_t a, b;
		transform_vertex(f, vtx_ptr(f, vertices, indices, i), &a);
		transform_vertex(f, vtx_ptr(f, vertices, indices, i + 1), &b);
		if (a.out || b.out)
			continue;   // debug lines only; drop anything needing clipping
		gif_tag_begin(GS_PRIM_LINE | dc.prim_flags, dc.regs, dc.nreg, 2, 1, 2);
		emit_vertex(&a);
		emit_vertex(&b);
	}
}

static void draw_3d_sprites(const vfmt_t *f, int count, const void *indices, const void *vertices)
{
	int i;
	for (i = 0; i + 1 < count; i += 2) {
		cvert_t a, b;
		transform_vertex(f, vtx_ptr(f, vertices, indices, i), &a);
		transform_vertex(f, vtx_ptr(f, vertices, indices, i + 1), &b);
		if (a.out || b.out)
			continue;
		gif_tag_begin(GS_PRIM_SPRITE | dc.prim_flags, dc.regs, dc.nreg, 2, 1, 2);
		emit_vertex(&a);
		emit_vertex(&b);
	}
}

void GS_DrawArray(int prim, int vtype, int count, const void *indices, const void *vertices)
{
	vfmt_t f;
	int i;

	arena_outstanding = 0;
	if (count <= 0 || !vertices)
		return;

	vfmt_decode(vtype, &f);
	draw_ctx_setup(vtype);

	// The GS drops the low 8 bits of the ST mantissa. Quake world textures can
	// carry large repeating coordinates, which amplifies that precision loss
	// into visible texture swimming. Rebase repeat-wrapped ST close to zero per
	// draw; subtracting whole texture periods is visually identical under REPEAT.
	if (!dc.is2d && dc.textured && count > 0 &&
	    (st.wrap_u == GS_REPEAT || st.wrap_v == GS_REPEAT)) {
		float x, y, z, s, t;
		u32 color;
		vfmt_fetch(&f, vtx_ptr(&f, vertices, indices, 0), 0,
		           &x, &y, &z, &s, &t, &color);
		s = s * st.tex_su + st.tex_ou;
		t = t * st.tex_sv + st.tex_ov;
		if (st.wrap_u == GS_REPEAT)
			dc.tex_bias_s = floorf(s);
		if (st.wrap_v == GS_REPEAT)
			dc.tex_bias_t = floorf(t);
	}

	gs_stats.verts_in += count;

	if (dc.is2d) {
		draw_2d(prim, &f, count, indices, vertices);
		return;
	}

	GS_UpdateMatrix();

	switch (prim) {
	case GS_LINES:
	case GS_LINE_STRIP:
		draw_3d_lines(prim, &f, count, indices, vertices);
		return;
	case GS_SPRITES:
		draw_3d_sprites(&f, count, indices, vertices);
		return;
	case GS_POINTS:
		return;
	case GS_TRIANGLE_FAN: {
		// Quake polygons are convex fans. Transform all vertices, clip as one
		// polygon when needed.
		cvert_t *v;
		int ormask = 0, andmask = ~0;
		if (count > CLIP_MAX - 8) {
			// very large fan: split into sub fans sharing vertex 0
			int start = 1;
			while (start + 1 < count) {
				int n = count - start;
				if (n > CLIP_MAX - 9) n = CLIP_MAX - 9;
				ormask = 0; andmask = ~0;
				transform_vertex(&f, vtx_ptr(&f, vertices, indices, 0), &xf[0]);
				ormask |= xf[0].out; andmask &= xf[0].out;
				for (i = 0; i < n + 1 && start + i < count; i++) {
					transform_vertex(&f, vtx_ptr(&f, vertices, indices, start + i), &xf[1 + i]);
					ormask |= xf[1 + i].out; andmask &= xf[1 + i].out;
				}
				draw_polygon(xf, 1 + i, ormask, andmask);
				start += n;
			}
			return;
		}
		v = xf;
		for (i = 0; i < count; i++) {
			transform_vertex(&f, vtx_ptr(&f, vertices, indices, i), &v[i]);
			ormask |= v[i].out;
			andmask &= v[i].out;
		}
		draw_polygon(v, count, ormask, andmask);
		return;
	}
	case GS_TRIANGLES: {
		int base = 0;
		while (base + 2 < count) {
			int n = count - base;
			if (n > XFORM_BATCH) n = XFORM_BATCH;
			n -= n % 3;
			for (i = 0; i < n; i++)
				transform_vertex(&f, vtx_ptr(&f, vertices, indices, base + i), &xf[i]);
			for (i = 0; i < n; i += 3) {
				cvert_t tri[3];
				int o = xf[i].out | xf[i + 1].out | xf[i + 2].out;
				int a = xf[i].out & xf[i + 1].out & xf[i + 2].out;
				tri[0] = xf[i]; tri[1] = xf[i + 1]; tri[2] = xf[i + 2];
				draw_polygon(tri, 3, o, a);
			}
			base += n;
		}
		return;
	}
	case GS_TRIANGLE_FAN_TRIS: {
		// Non planar fan: every triangle (v0, vi, vi+1) is culled/clipped on
		// its own. Vertex 0 is kept in slot 0 across batches.
		int base = 1;
		transform_vertex(&f, vtx_ptr(&f, vertices, indices, 0), &xf[0]);
		while (base + 1 < count) {
			int n = count - base;
			if (n > XFORM_BATCH - 1) n = XFORM_BATCH - 1;
			for (i = 0; i < n; i++)
				transform_vertex(&f, vtx_ptr(&f, vertices, indices, base + i), &xf[1 + i]);
			for (i = 1; i < n; i++) {
				cvert_t tri[3];
				int o = xf[0].out | xf[i].out | xf[i + 1].out;
				int a = xf[0].out & xf[i].out & xf[i + 1].out;
				tri[0] = xf[0]; tri[1] = xf[i]; tri[2] = xf[i + 1];
				draw_polygon(tri, 3, o, a);
			}
			if (base + n >= count)
				break;
			base += n - 1;   // share the last vertex with the next batch
		}
		return;
	}
	case GS_TRIANGLE_STRIP: {
		int base = 0;
		int parity = 0;
		while (base + 2 < count) {
			int n = count - base;
			if (n > XFORM_BATCH) n = XFORM_BATCH;
			for (i = 0; i < n; i++)
				transform_vertex(&f, vtx_ptr(&f, vertices, indices, base + i), &xf[i]);
			for (i = 0; i + 2 < n; i++) {
				cvert_t tri[3];
				int o = xf[i].out | xf[i + 1].out | xf[i + 2].out;
				int a = xf[i].out & xf[i + 1].out & xf[i + 2].out;
				if (parity) {
					tri[0] = xf[i + 1]; tri[1] = xf[i]; tri[2] = xf[i + 2];
				} else {
					tri[0] = xf[i]; tri[1] = xf[i + 1]; tri[2] = xf[i + 2];
				}
				parity ^= 1;
				draw_polygon(tri, 3, o, a);
			}
			if (base + n >= count)
				break;
			base += n - 2;   // overlap two vertices; parity already continues
		}
		return;
	}
	default:
		return;
	}
}

// ===========================================================================
// VRAM texture ring
// ===========================================================================

static int ring_base;          // in 64 word blocks
static int ring_end;
static int ring_ptr;
static gs_texture_t *ring_head;   // oldest
static gs_texture_t *ring_tail;   // newest
static int ring_used_blocks;
static int ring_highwater;
static int ring_resident;
static int ring_evictions_frame;
static int ring_evictions_total;
static int ring_wraps;

// Block arrangement within a page (GS manual, "Memory arrangement").
static const u8 block_ct32[4][8] = {
	{  0,  1,  4,  5, 16, 17, 20, 21 },
	{  2,  3,  6,  7, 18, 19, 22, 23 },
	{  8,  9, 12, 13, 24, 25, 28, 29 },
	{ 10, 11, 14, 15, 26, 27, 30, 31 },
};
static const u8 block_ct16[8][4] = {
	{  0,  2,  8, 10 }, {  1,  3,  9, 11 }, {  4,  6, 12, 14 }, {  5,  7, 13, 15 },
	{ 16, 18, 24, 26 }, { 17, 19, 25, 27 }, { 20, 22, 28, 30 }, { 21, 23, 29, 31 },
};

typedef struct { int page_w, page_h, block_w, block_h, cols, rows; } psm_geom_t;

static void psm_geometry(int psm, psm_geom_t *g)
{
	switch (psm) {
	case GS_TEX_CT32: g->page_w = 64;  g->page_h = 32;  g->block_w = 8;  g->block_h = 8;  g->cols = 8; g->rows = 4; break;
	case GS_TEX_CT16: g->page_w = 64;  g->page_h = 64;  g->block_w = 16; g->block_h = 8;  g->cols = 4; g->rows = 8; break;
	case GS_TEX_T8:   g->page_w = 128; g->page_h = 64;  g->block_w = 16; g->block_h = 16; g->cols = 8; g->rows = 4; break;
	default:          g->page_w = 128; g->page_h = 128; g->block_w = 32; g->block_h = 16; g->cols = 4; g->rows = 8; break;
	}
}

static int tex_tbw(int width, int psm)
{
	if (psm == GS_TEX_T8 || psm == GS_TEX_T4) {
		int w = (width + 127) & ~127;
		return w / 64;
	}
	return width < 64 ? 1 : width / 64;
}

// Number of 64 word blocks touched by a width x height texture at TBP0 = 0.
static int tex_footprint_blocks(int width, int height, int psm, int tbw)
{
	psm_geom_t g;
	int bx, by, maxb = 0;
	int buf_w = tbw * 64;
	int pages_across;
	psm_geometry(psm, &g);
	pages_across = (buf_w + g.page_w - 1) / g.page_w;
	for (by = 0; by < (height + g.block_h - 1) / g.block_h; by++) {
		for (bx = 0; bx < (width + g.block_w - 1) / g.block_w; bx++) {
			int page = (by / g.rows) * pages_across + (bx / g.cols);
			int in_page = (g.cols == 8) ? block_ct32[by % g.rows][bx % g.cols] : block_ct16[by % g.rows][bx % g.cols];
			int b = page * 32 + in_page;
			if (b > maxb)
				maxb = b;
		}
	}
	return maxb + 1;
}

int GS_TextureBytes(int width, int height, int psm)
{
	switch (psm) {
	case GS_TEX_CT32: return width * height * 4;
	case GS_TEX_CT16: return width * height * 2;
	case GS_TEX_T8:   return width * height;
	default:          return (width * height) >> 1;
	}
}

void GS_TextureSetup(gs_texture_t *t, void *pixels, const u32 *clut, int width, int height, int psm)
{
	int fp;
	memset(t, 0, sizeof(*t));
	if (((u32)pixels & 15) || (clut && ((u32)clut & 15)))
		PS2_Fatal("GS: texture data not 16 byte aligned");
	if (width < 8 || height < 8 || width > 1024 || height > 1024 || (width & (width - 1)) || (height & (height - 1)))
		PS2_Fatal("GS: invalid texture size %dx%d", width, height);
	t->pixels = pixels;
	t->clut = clut;
	t->width = (short)width;
	t->height = (short)height;
	t->psm = (u8)psm;
	t->tw = (u8)tex_log2(width);
	t->th = (u8)tex_log2(height);
	t->tbw = (u8)tex_tbw(width, psm);
	fp = tex_footprint_blocks(width, height, psm, t->tbw);
	t->clut_offset = -1;
	if (psm == GS_TEX_T8) { t->clut_offset = fp; fp += 4; }
	else if (psm == GS_TEX_T4) { t->clut_offset = fp; fp += 1; }
	t->vram_blocks = fp;
	t->vram_block = -1;
	t->dirty = 1;
}

static void ring_unlink_head(void)
{
	gs_texture_t *t = ring_head;
	ring_head = t->ring_next;
	if (!ring_head)
		ring_tail = NULL;
	t->ring_next = NULL;
	t->vram_block = -1;
	ring_used_blocks -= t->vram_blocks;
	ring_resident--;
	ring_evictions_frame++;
	ring_evictions_total++;
}

static void ring_remove(gs_texture_t *t)
{
	gs_texture_t **pp = &ring_head, *prev = NULL;
	while (*pp && *pp != t) {
		prev = *pp;
		pp = &(*pp)->ring_next;
	}
	if (!*pp)
		return;
	*pp = t->ring_next;
	if (ring_tail == t)
		ring_tail = prev;
	t->ring_next = NULL;
	t->vram_block = -1;
	ring_used_blocks -= t->vram_blocks;
	ring_resident--;
}

static int ring_alloc(gs_texture_t *t)
{
	int n = t->vram_blocks;
	int p = ring_ptr;
	if (n > ring_end - ring_base)
		PS2_Fatal("GS: texture %dx%d psm %d does not fit in VRAM texture region", t->width, t->height, t->psm);
	if (p + n > ring_end) {
		// evict everything from p to the end of the region, then wrap
		while (ring_head && ring_head->vram_block >= p)
			ring_unlink_head();
		p = ring_base;
		ring_wraps++;
	}
	while (ring_head && ring_head->vram_block >= p && ring_head->vram_block < p + n)
		ring_unlink_head();
	t->vram_block = p;
	t->ring_next = NULL;
	if (ring_tail) ring_tail->ring_next = t; else ring_head = t;
	ring_tail = t;
	ring_ptr = p + n;
	ring_used_blocks += n;
	ring_resident++;
	if (ring_used_blocks > ring_highwater)
		ring_highwater = ring_used_blocks;
	return p;
}

static void upload_image(int dbp, int dbw, int psm, int w, int h, const void *data, int bytes)
{
	const u8 *src = (const u8 *)data;
	int qwc = (bytes + 15) >> 4;

	SyncDCache((void *)data, (u8 *)data + bytes);

	ad_begin(4);
	ad_write(GS_REG_BITBLTBUF, GS_SET_BITBLTBUF(0, 0, 0, dbp, dbw, psm));
	ad_write(GS_REG_TRXPOS, GS_SET_TRXPOS(0, 0, 0, 0, 0));
	ad_write(GS_REG_TRXREG, GS_SET_TRXREG(w, h));
	ad_write(GS_REG_TRXDIR, GS_SET_TRXDIR(0));
	ad_end();

	while (qwc > 0) {
		int chunk = qwc > 32767 ? 32767 : qwc;
		pkt_reserve(1);
		pkt_ptr[0] = GIF_SET_TAG(chunk, 1, 0, 0, GIF_FLG_IMAGE, 0);
		pkt_ptr[1] = 0;
		pkt_ptr += 2;
		pkt_ref(src, chunk);
		src += chunk * 16;
		qwc -= chunk;
	}
	gs_stats.upload_bytes += bytes;
}

static void vram_ensure_resident(gs_texture_t *t)
{
	if (t->vram_block >= 0 && !t->dirty)
		return;
	gif_tag_close();
	if (t->vram_block < 0)
		ring_alloc(t);
	upload_image(t->vram_block, t->tbw, t->psm, t->width, t->height, t->pixels, GS_TextureBytes(t->width, t->height, t->psm));
	if (t->clut_offset >= 0 && t->clut) {
		if (t->psm == GS_TEX_T8)
			upload_image(t->vram_block + t->clut_offset, 1, GS_TEX_CT32, 16, 16, t->clut, 256 * 4);
		else
			upload_image(t->vram_block + t->clut_offset, 1, GS_TEX_CT32, 8, 2, t->clut, 16 * 4);
	}
	ad_begin(1);
	ad_write(GS_REG_TEXFLUSH, 0);
	ad_end();
	t->dirty = 0;
	gs_stats.uploads++;
	st.dirty |= DIRTY_TEX0 | DIRTY_CLAMP;
}

void GS_TextureRelease(gs_texture_t *t)
{
	if (st.texture == t)
		st.texture = NULL;
	if (t->vram_block >= 0)
		ring_remove(t);
}

void GS_TextureDirty(gs_texture_t *t) { t->dirty = 1; }

void GS_BindTexture(gs_texture_t *t)
{
	if (st.texture != t) {
		st.texture = t;
		st.dirty |= DIRTY_TEX0 | DIRTY_CLAMP;
	} else if (t && (t->dirty || t->vram_block < 0)) {
		st.dirty |= DIRTY_TEX0;
	}
	if (t)
		t->last_used_frame = gs_stats.frame;
}

void GS_EvictAllTextures(void)
{
	while (ring_head)
		ring_unlink_head();
	ring_ptr = ring_base;
	ring_used_blocks = 0;
	st.dirty |= DIRTY_TEX0;
}

void GS_GetVramStats(gs_vram_stats_t *s)
{
	s->total = 4 * 1024 * 1024;
	s->framebuffers = gs_video.fb_words * 4 * 2;
	s->depth = gs_video.z_words * 4;
	s->tex_region = (ring_end - ring_base) * 256;
	s->tex_used = ring_used_blocks * 256;
	s->tex_highwater = ring_highwater * 256;
	s->resident_textures = ring_resident;
	s->evictions_frame = ring_evictions_frame;
	s->evictions_total = ring_evictions_total;
	s->wraps = ring_wraps;
}

// ===========================================================================
// Video / frame
// ===========================================================================

static int vsync_sema = -1;
static int vsync_handler_id = -1;
static volatile u32 vsync_counter;
static u32 last_flip_vsync;

static s32 vsync_isr(s32 cause)
{
	(void)cause;
	vsync_counter++;
	if (vsync_sema >= 0)
		iSignalSema(vsync_sema);
	ExitHandler();
	return -1;
}

static void wait_vsync_count(u32 target)
{
	ps2_clock_t t0 = ps2_clock();
	while ((s32)(vsync_counter - target) < 0) {
		if (vsync_sema >= 0)
			WaitSema(vsync_sema);
	}
	gs_stats.vsync_wait_ticks += (u32)(ps2_clock() - t0);
}

int GS_Init(int force_mode, int want_z24)
{
	int region, mode;
	ee_sema_t sema;

	memset(&gs_video, 0, sizeof(gs_video));
	memset(&st, 0, sizeof(st));

	region = graph_get_region();
	if (force_mode == 1) mode = GRAPH_MODE_NTSC;
	else if (force_mode == 2) mode = GRAPH_MODE_PAL;
	else mode = (region == GRAPH_MODE_PAL) ? GRAPH_MODE_PAL : GRAPH_MODE_NTSC;

	gs_video.pal = (mode == GRAPH_MODE_PAL);
	gs_video.width = 640;
	gs_video.height = gs_video.pal ? 512 : 448;
	gs_video.interlaced = 1;
	gs_video.refresh_hz = gs_video.pal ? 50 : 60;
	gs_video.fb_psm = GS_PSM_16;
	gs_video.z_psm = want_z24 ? GS_ZBUF_24 : GS_ZBUF_16;

	graph_vram_clear();
	gs_video.fb_addr[0] = graph_vram_allocate(gs_video.width, gs_video.height, gs_video.fb_psm, GRAPH_ALIGN_PAGE);
	gs_video.fb_addr[1] = graph_vram_allocate(gs_video.width, gs_video.height, gs_video.fb_psm, GRAPH_ALIGN_PAGE);
	// graph_vram_allocate sizes by PSM; Z24 is stored as 32 bits.
	gs_video.z_addr = graph_vram_allocate(gs_video.width, gs_video.height,
		want_z24 ? GS_PSM_32 : GS_PSM_16, GRAPH_ALIGN_PAGE);
	if (gs_video.fb_addr[0] < 0 || gs_video.fb_addr[1] < 0 || gs_video.z_addr < 0)
		PS2_Fatal("GS: unable to allocate frame/depth buffers");
	gs_video.fb_words = graph_vram_size(gs_video.width, gs_video.height, gs_video.fb_psm, GRAPH_ALIGN_PAGE);
	gs_video.z_words = graph_vram_size(gs_video.width, gs_video.height, want_z24 ? GS_PSM_32 : GS_PSM_16, GRAPH_ALIGN_PAGE);
	gs_video.tex_base = gs_video.z_addr + gs_video.z_words;
	gs_video.tex_end = GRAPH_VRAM_MAX_WORDS;

	ring_base = gs_video.tex_base >> 6;
	ring_end = gs_video.tex_end >> 6;
	ring_ptr = ring_base;

	graph_set_mode(GRAPH_MODE_INTERLACED, mode, GRAPH_MODE_FIELD, GRAPH_ENABLE);
	graph_set_screen(0, 0, gs_video.width, gs_video.height);
	graph_set_bgcolor(0, 0, 0);
	graph_set_framebuffer_filtered(gs_video.fb_addr[0], gs_video.width, gs_video.fb_psm, 0, 0);
	graph_enable_output();

	// GIF DMA channel: clear any stale state.
	*D2_CHCR = 0;
	pkt_in_flight = 0;
	pkt_select(0);

	sema.init_count = 0;
	sema.max_count = 1;
	sema.option = 0;
	vsync_sema = CreateSema(&sema);
	vsync_handler_id = AddIntcHandler(INTC_VBLANK_S, vsync_isr, 0);
	EnableIntc(INTC_VBLANK_S);

	// default state
	for (mdepth[0] = 0; mdepth[0] < 1; mdepth[0]++) {}
	mdepth[0] = mdepth[1] = mdepth[2] = 0;
	mat_identity(&mstack[0][0]);
	mat_identity(&mstack[1][0]);
	mat_identity(&mstack[2][0]);
	mvp_dirty = 1;

	st.color = 0xffffffff;
	st.tex_su = st.tex_sv = 1.0f;
	st.tfx = GS_TFX_MODULATE;
	st.tcc = GS_TCC_RGBA;
	st.min_filter = st.mag_filter = GS_LINEAR;
	st.wrap_u = st.wrap_v = GS_REPEAT;
	GS_BlendRaw(0, 1, 0, 1, 0);
	st.alpha_func = GS_ALWAYS;
	st.depth_func = GS_LEQUAL;
	st.depth_near = 0.0f;
	st.depth_far = 65535.0f;
	st.shade = GS_SMOOTH;
	st.caps[GS_CAP_DITHER] = 1;
	GS_Viewport(2048, 2048, gs_video.width, gs_video.height);
	GS_Offset(2048 - gs_video.width / 2, 2048 - gs_video.height / 2);
	GS_Scissor(0, 0, gs_video.width, gs_video.height);
	st.dirty = DIRTY_ALL;
	draw_buffer = 1;
	last_flip_vsync = vsync_counter;

	PS2_Log("GS: %s %dx%d interlaced, FB CT16 x2 @%d/%d, Z%s @%d, textures %d KiB\n",
		gs_video.pal ? "PAL" : "NTSC", gs_video.width, gs_video.height,
		gs_video.fb_addr[0], gs_video.fb_addr[1], want_z24 ? "24" : "16", gs_video.z_addr,
		((ring_end - ring_base) * 256) / 1024);
	return 1;
}

void GS_Shutdown(void)
{
	GS_Finish();
	if (vsync_handler_id >= 0) {
		DisableIntc(INTC_VBLANK_S);
		RemoveIntcHandler(INTC_VBLANK_S, vsync_handler_id);
		vsync_handler_id = -1;
	}
	if (vsync_sema >= 0) {
		DeleteSema(vsync_sema);
		vsync_sema = -1;
	}
}

void GS_BeginFrame(void)
{
	memset(&gs_stats, 0, sizeof(gs_stats));
	gs_stats.frame = gs_stats_last.frame + 1;
	ring_evictions_frame = 0;
	arena_used = 0;
	arena_outstanding = 0;
	st.dirty = DIRTY_ALL;
}

static void gs_wait_finish(void)
{
	// FINISH event is raised once every primitive before the FINISH
	// register write has been drawn.
	while (!(*GS_REG_CSR & 2))
		;
	*GS_REG_CSR = 2;
}

void GS_Finish(void)
{
	gif_tag_close();
	*GS_REG_CSR = 2;   // clear pending FINISH event
	ad_begin(1);
	ad_write(GS_REG_FINISH, 1);
	ad_end();
	pkt_kick();
	dma_gif_wait();
	gs_wait_finish();
}

void GS_EndFrame(int vsync_interval)
{
	GS_Finish();

	if (vsync_interval < 1)
		vsync_interval = 1;
	wait_vsync_count(last_flip_vsync + (u32)vsync_interval);
	last_flip_vsync = vsync_counter;

	graph_set_framebuffer_filtered(gs_video.fb_addr[draw_buffer], gs_video.width, gs_video.fb_psm, 0, 0);
	draw_buffer ^= 1;

	gs_stats.vsync_count = vsync_counter;
	gs_stats_last = gs_stats;
}

void GS_ClearColor(u32 color) { clear_color = color; }
void GS_ClearDepth(int depth) { clear_depth = depth; }

void GS_Clear(int flags)
{
	int x0, y0, x1, y1;
	u32 fbmsk;
	int zmsk;
	if (!flags)
		return;
	gif_tag_close();
	state_flush();

	fbmsk = (flags & GS_COLOR_BUFFER_BIT) ? 0 : 0xffffffff;
	zmsk = (flags & GS_DEPTH_BUFFER_BIT) ? 0 : 1;
	x0 = (st.of_x) << 4;
	y0 = (st.of_y) << 4;
	x1 = (st.of_x + gs_video.width) << 4;
	y1 = (st.of_y + gs_video.height) << 4;

	ad_begin(10);
	ad_write(GS_REG_FRAME_1, GS_SET_FRAME(gs_video.fb_addr[draw_buffer] >> 11, gs_video.width >> 6, gs_video.fb_psm, fbmsk));
	ad_write(GS_REG_ZBUF_1, GS_SET_ZBUF(gs_video.z_addr >> 11, gs_video.z_psm & 0xf, zmsk));
	ad_write(GS_REG_TEST_1, GS_SET_TEST(0, 1, 0, 0, 0, 0, 1, 1));
	ad_write(GS_REG_SCISSOR_1, GS_SET_SCISSOR(0, gs_video.width - 1, 0, gs_video.height - 1));
	ad_write(GS_REG_PRIM, GS_SET_PRIM(GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
	ad_write(GS_REG_RGBAQ, GS_SET_RGBAQ(clear_color & 0xff, (clear_color >> 8) & 0xff, (clear_color >> 16) & 0xff, 0x80, 0x3f800000));
	ad_write(GS_REG_XYZ2, GS_SET_XYZ(x0, y0, 65535 - clear_depth));
	ad_write(GS_REG_XYZ2, GS_SET_XYZ(x1, y1, 65535 - clear_depth));
	ad_end();
	st.dirty |= DIRTY_FRAME | DIRTY_ZBUF | DIRTY_TEST | DIRTY_SCISSOR;
}
