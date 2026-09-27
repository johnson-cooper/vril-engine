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
// gs_core.h -- PlayStation 2 Graphics Synthesizer backend core
//
// This is the only layer of the PS2 renderer that talks to the GS. It is a
// deliberately small, Quake-shaped immediate API (matrix stack + draw arrays +
// a handful of render states), NOT a general purpose graphics API. The
// renderer above it (gs_*.cpp) was derived from the PSP GU renderer, so the
// state/primitive vocabulary intentionally mirrors what that code needs.
//
// Design notes (see README_PS2.md "Renderer" for the long version):
//  * The GS performs no transform, clipping or culling. All vertices are
//    transformed on the EE into clip space, trivially accepted/rejected with
//    outcodes, polygon-clipped against the near plane and the guard band only
//    when required, then projected to 12.4 fixed point GS coordinates.
//  * Geometry and register writes are written into a double-buffered DMA
//    source chain (CNT tags). Texture uploads are REF tags pointing at the
//    EE-side texture backing store, so texel data is never memcpy'd into the
//    command stream. The two chain buffers are fixed-size and allocated once.
//  * The GIF stream is processed strictly in order, which is what makes the
//    VRAM texture ring (gs_vram_*) safe: overwriting a texture that an earlier
//    draw in the same frame used is fine because that draw already happened.
//  * GS depth test only supports GEQUAL/GREATER, so depth is stored inverted
//    relative to the (PSP style) "smaller is closer" convention used by the
//    renderer: gs_z = 0xFFFF - z.
#ifndef GS_CORE_H
#define GS_CORE_H

#include <tamtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Primitive types (GS_DrawArray prim)
// ---------------------------------------------------------------------------
#define GS_POINTS          0
#define GS_LINES           1
#define GS_LINE_STRIP      2
#define GS_TRIANGLES       3
#define GS_TRIANGLE_STRIP  4
#define GS_TRIANGLE_FAN    5
#define GS_SPRITES         6
// Fan of independent triangles (non planar, e.g. alias models): culled and
// clipped per triangle. GS_TRIANGLE_FAN assumes a planar convex polygon.
#define GS_TRIANGLE_FAN_TRIS 7

// ---------------------------------------------------------------------------
// Vertex type flags (GS_DrawArray vtype). Vertex element order in memory is
// always: [texture] [color] [position], matching the PSP GU layout the
// renderer was written against.
// ---------------------------------------------------------------------------
#define GS_TEXTURE_16BIT   (1 << 0)   // 2 x s16
#define GS_TEXTURE_32BITF  (1 << 1)   // 2 x float
#define GS_COLOR_8888      (1 << 2)   // u32 ABGR
#define GS_VERTEX_8BIT     (1 << 3)   // 3 x s8
#define GS_VERTEX_16BIT    (1 << 4)   // 3 x s16
#define GS_VERTEX_32BITF   (1 << 5)   // 3 x float
#define GS_TRANSFORM_2D    (1 << 6)   // positions/texcoords already in screen space / texels
#define GS_TRANSFORM_3D    0

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------
#define GS_CAP_BLEND        0
#define GS_CAP_TEXTURE_2D   1
#define GS_CAP_DEPTH_TEST   2
#define GS_CAP_ALPHA_TEST   3
#define GS_CAP_CULL_FACE    4
#define GS_CAP_FOG          5
#define GS_CAP_SCISSOR_TEST 6
#define GS_CAP_DITHER       7
#define GS_CAP_CLIP_PLANES  8
#define GS_CAP_COUNT        9

// Texture functions
#define GS_TFX_MODULATE  0
#define GS_TFX_DECAL     1
#define GS_TFX_REPLACE   2
#define GS_TCC_RGB       0
#define GS_TCC_RGBA      1

// Blend factors (subset expressible by the GS blend unit, see GS_BlendFunc)
#define GS_SRC_COLOR            0
#define GS_ONE_MINUS_SRC_COLOR  1
#define GS_SRC_ALPHA            2
#define GS_ONE_MINUS_SRC_ALPHA  3
#define GS_DST_ALPHA            4
#define GS_ONE_MINUS_DST_ALPHA  5
#define GS_DST_COLOR            6
#define GS_ONE_MINUS_DST_COLOR  7
#define GS_FIX                  10
#define GS_ADD                  0

// Compare functions (renderer convention: smaller depth = closer)
#define GS_NEVER    0
#define GS_ALWAYS   1
#define GS_EQUAL    2
#define GS_NOTEQUAL 3
#define GS_LESS     4
#define GS_LEQUAL   5
#define GS_GREATER  6
#define GS_GEQUAL   7

// Shade models
#define GS_FLAT   0
#define GS_SMOOTH 1

// Texture filters / wrap
#define GS_NEAREST 0
#define GS_LINEAR  1
#define GS_REPEAT  0
#define GS_CLAMP   1

// Matrix modes
#define GS_PROJECTION 0
#define GS_VIEW       1
#define GS_MODEL      2

// Clear flags
#define GS_COLOR_BUFFER_BIT 1
#define GS_DEPTH_BUFFER_BIT 2

// Texture pixel storage formats (GS PSM codes)
#define GS_TEX_CT32 0x00
#define GS_TEX_CT16 0x02
#define GS_TEX_T8   0x13
#define GS_TEX_T4   0x14

#define GS_RGBA(r, g, b, a) \
	((u32)(((u32)(r) & 0xff) | (((u32)(g) & 0xff) << 8) | (((u32)(b) & 0xff) << 16) | (((u32)(a) & 0xff) << 24)))
// float components in [0, 1]; saturates (e.g. fullbright model light 1.28)
static inline u32 gs_color_channel(float v)
{
	return v <= 0.0f ? 0u : v >= 1.0f ? 255u : (u32)(v * 255.0f);
}
#define GS_COLOR(r, g, b, a) \
	GS_RGBA(gs_color_channel(r), gs_color_channel(g), gs_color_channel(b), gs_color_channel(a))

#define GS_TRUE  1
#define GS_FALSE 0
#define GS_PI    3.141593f

typedef struct { float x, y, z; } gs_vec3_t;
typedef struct { float x, y, z, w; } gs_vec4_t;
typedef struct { gs_vec4_t x, y, z, w; } gs_mat4_t;   // column vectors, same layout as ScePspFMatrix4

// ---------------------------------------------------------------------------
// Video / frame
// ---------------------------------------------------------------------------
typedef struct {
	int width, height;        // render target (== display) size in pixels
	int pal;                  // 1 = PAL timing
	int interlaced;
	int fb_psm, z_psm;
	int fb_words;             // size of one colour buffer in VRAM words
	int z_words;
	int fb_addr[2];           // VRAM word addresses
	int z_addr;
	int tex_base;             // first VRAM word available for textures
	int tex_end;              // one past the last word available for textures
	int refresh_hz;           // 50 / 60
} gs_video_t;

extern gs_video_t gs_video;

typedef struct {
	u32 frame;                // frame counter
	u32 vsync_count;          // vblanks since init (from interrupt handler)
	u32 kicks;                // DMA chain kicks this frame
	u32 verts_in;             // vertices submitted this frame
	u32 verts_out;            // vertices emitted to GS this frame
	u32 prims_culled;         // polygons rejected (cull / trivial reject)
	u32 prims_clipped;        // polygons that required clipping
	u32 packet_bytes;         // bytes of GIF data generated this frame
	u32 upload_bytes;         // texture bytes uploaded this frame
	u32 uploads;              // texture uploads this frame
	u32 dma_wait_ticks;       // ps2_clock ticks spent waiting for DMA
	u32 vsync_wait_ticks;     // ps2_clock ticks spent waiting for vsync
	u32 arena_peak;           // transient vertex arena high-water (bytes)
} gs_stats_t;

extern gs_stats_t gs_stats;       // current frame
extern gs_stats_t gs_stats_last;  // last completed frame

int  GS_Init(int force_mode, int want_z24);   // force_mode: 0 auto, 1 NTSC, 2 PAL
void GS_Shutdown(void);
void GS_BeginFrame(void);
void GS_EndFrame(int vsync_interval);          // flush + wait + flip
void GS_Finish(void);                          // flush + wait for GS idle
void GS_Clear(int flags);
void GS_ClearColor(u32 color);
void GS_ClearDepth(int depth);

// Transient per-frame vertex memory. Consumed immediately by GS_DrawArray,
// so it is recycled once no allocation is outstanding.
void *GS_GetMemory(int size);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
void GS_Enable(int cap);
void GS_Disable(int cap);
int  GS_IsEnabled(int cap);
void GS_Color(u32 abgr);
void GS_TexFunc(int tfx, int tcc);
void GS_TexFilter(int min, int mag);
void GS_TexWrap(int u, int v);
void GS_BlendFunc(int op, int src, int dst, u32 srcfix, u32 dstfix);
void GS_AlphaFunc(int func, int ref, int mask);
void GS_DepthFunc(int func);
void GS_DepthMask(int mask);            // non-zero disables depth writes (GU semantics)
void GS_DepthRange(int near_z, int far_z);
void GS_DepthOffset(int offset);
void GS_ShadeModel(int mode);
void GS_Scissor(int x0, int y0, int x1, int y1);
void GS_Viewport(int cx, int cy, int w, int h);
void GS_Offset(int x, int y);
void GS_Fog(float start, float end, u32 color);
void GS_FrameMask(u32 fbmsk);           // PS2 only: per-channel framebuffer write mask
// Texture coordinate transform for 3D draws: s' = s * su + ou (GU semantics).
void GS_TexScale(float su, float sv);
void GS_TexOffset(float ou, float ov);

// Raw GS blend equation: ((A - B) * C >> 7) + D (see GS manual ALPHA register).
// Used by passes that have no GL/GU equivalent (lightmap multiply).
void GS_BlendRaw(int a, int b, int c, int d, int fix);

// ---------------------------------------------------------------------------
// Matrices
// ---------------------------------------------------------------------------
void GS_MatrixMode(int mode);
void GS_LoadIdentity(void);
void GS_LoadMatrix(const gs_mat4_t *m);
void GS_StoreMatrix(gs_mat4_t *m);
void GS_MultMatrix(const gs_mat4_t *m);
void GS_PushMatrix(void);
void GS_PopMatrix(void);
void GS_Translate(const gs_vec3_t *v);
void GS_Scale(const gs_vec3_t *v);
void GS_RotateX(float a);
void GS_RotateY(float a);
void GS_RotateZ(float a);
void GS_RotateXYZ(const gs_vec3_t *v);
void GS_RotateZYX(const gs_vec3_t *v);
void GS_Perspective(float fovy, float aspect, float znear, float zfar);
void GS_Ortho(float left, float right, float bottom, float top, float znear, float zfar);
void GS_UpdateMatrix(void);

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void GS_DrawArray(int prim, int vtype, int count, const void *indices, const void *vertices);

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
// A texture's EE-side backing store is owned by the renderer. The GS copy is
// a cache entry in the VRAM ring, uploaded on bind when not resident.
typedef struct gs_texture_s {
	void *pixels;            // 16 byte aligned, EE backing (never NULL while registered)
	const u32 *clut;         // T8: 256 entries, T4: 16 entries (CT32, already CSM1-swizzled for T8), 16 byte aligned
	short width, height;     // power of two (GS requirement)
	unsigned char psm;       // GS_TEX_*
	unsigned char tw, th;    // log2 sizes
	unsigned char tbw;       // buffer width in 64 texel units
	int vram_blocks;         // footprint incl. CLUT in 64-word blocks
	int clut_offset;         // block offset of CLUT inside allocation (or -1)
	// residency
	int vram_block;          // -1 when not resident
	u32 vram_serial;         // ring generation stamp
	struct gs_texture_s *ring_next;
	u32 last_used_frame;
	u32 dirty;               // backing changed since last upload
} gs_texture_t;

void GS_TextureSetup(gs_texture_t *t, void *pixels, const u32 *clut, int width, int height, int psm);
void GS_TextureRelease(gs_texture_t *t);    // evicts from VRAM, does not free backing
void GS_TextureDirty(gs_texture_t *t);      // backing changed, re-upload on next bind
void GS_BindTexture(gs_texture_t *t);
void GS_EvictAllTextures(void);
int  GS_TextureBytes(int width, int height, int psm);

// VRAM accounting (bytes)
typedef struct {
	int total, framebuffers, depth, tex_region, tex_used, tex_highwater;
	int resident_textures, evictions_frame, evictions_total, wraps;
} gs_vram_stats_t;
void GS_GetVramStats(gs_vram_stats_t *s);

#ifdef __cplusplus
}
#endif

#endif // GS_CORE_H
