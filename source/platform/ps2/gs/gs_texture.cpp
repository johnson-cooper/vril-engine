/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2007 Peter Mackay and Chris Swindle.
Copyright (C) 2008-2009 Crow_bar.
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
// gs_texture.cpp -- PS2 texture manager
//
// Keeps the same entry points as the PSP renderer (GL_LoadTexture,
// GL_LoadPalTex, GL_LoadImages, ...) so the renderer and shared code are
// unchanged, but stores textures in GS friendly formats:
//
//   Quake palette 8 bit   -> PSMT8, shared global CLUT (gs_quake_clut)
//   own palette 8 bit     -> PSMT8 + private 256 entry CLUT (HL/WAD3 world
//                            textures, skins). The PSP quantises these to 4
//                            bit; on the PS2 there is enough RAM to keep 8 bit.
//   pre-quantised 4 bit   -> PSMT4 + 16 entry CLUT
//   true colour images    -> PSMCT16 (opaque / 1 bit alpha) or PSMCT32
//                            (translucent) - see GL_LoadImages
//
// Texel data lives only in EE RAM; the GS copy is a cache entry in the VRAM
// ring (gs_core.c) that is uploaded on bind when not resident.

extern "C"
{
#include "../../../nzportable_def.h"
}

#include "gs_resample.h"

gltexture_t	gltextures[MAX_GLTEXTURES];
bool 		gltextures_used[MAX_GLTEXTURES];
bool 		gltextures_is_permanent[MAX_GLTEXTURES];
int			numgltextures;
static int	gltextures_bytes;

u32 gs_quake_clut[256] __attribute__((aligned(64)));

static cvar_t r_ps2_maxtexsize = {"r_ps2_maxtexsize", "512"};
static cvar_t r_ps2_worldtex4bit = {"r_ps2_worldtex4bit", "0"};

void GL_InitTextureUsage ()
{
	for (int i = 0; i < MAX_GLTEXTURES; i++) {
		gltextures_used[i] = false;
		gltextures_is_permanent[i] = false;
	}
	numgltextures = 0;
	gltextures_bytes = 0;
	Cvar_RegisterVariable(&r_ps2_maxtexsize);
	Cvar_RegisterVariable(&r_ps2_worldtex4bit);
}

void GL_TextureStats(int *count, int *bytes)
{
	*count = numgltextures;
	*bytes = gltextures_bytes;
}

/*
For marking textures as something that should never be cleaned up, like fonts, zombie, etc.
There should never be need to change anything back from permanent.
*/
void GL_MarkTextureAsPermanent(int texture_index)
{
	if (gltextures_used[texture_index] == false)
		Sys_Error("Tried to mark an empty texture as permanent!\n");
	gltextures_is_permanent[texture_index] = true;
}

// ---------------------------------------------------------------------------
// CLUTs
// ---------------------------------------------------------------------------

// GS CSM1 256 entry CLUTs are stored with entries 8-15 and 16-23 of every
// 32 entry group swapped.
static inline int clut_swizzle(int i)
{
	return (i & 0xe7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
}

static inline u32 gs_color(int r, int g, int b, int a)
{
	// GS alpha: 0x80 == 1.0
	return (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)((a + 1) >> 1) << 24);
}

void GS_BuildClut256(u32 *out, const byte *pal, int pal_bpp, int transparent_255)
{
	for (int i = 0; i < 256; i++) {
		int r = pal[i * pal_bpp + 0];
		int g = pal[i * pal_bpp + 1];
		int b = pal[i * pal_bpp + 2];
		int a = pal_bpp == 4 ? pal[i * pal_bpp + 3] : 255;
		if (transparent_255 && i == 255)
			r = g = b = a = 0;
		out[clut_swizzle(i)] = gs_color(r, g, b, a);
	}
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

void GL_Bind (int texture_index)
{
	if (texture_index < 0 || texture_index >= MAX_GLTEXTURES || !gltextures_used[texture_index])
		texture_index = 0;

	if (currenttexture == texture_index)
		return;
	currenttexture = texture_index;

	gltexture_t &texture = gltextures[texture_index];
	if (!gltextures_used[texture_index] || !texture.ram)
		return;

	GS_BindTexture(&texture.gs);
	if (r_retro.value)
		GS_TexFilter(GS_NEAREST, GS_NEAREST);
	else
		GS_TexFilter(texture.filter, texture.filter);
}

void GL_BindDET (int texture_index)
{
	GL_Bind(texture_index);
}

// Screen to texture copies are not used by the PS2 renderer.
void GL_Copy(int texture_index, int dx, int dy, int sx, int sy, int w, int h)
{
	(void)texture_index; (void)dx; (void)dy; (void)sx; (void)sy; (void)w; (void)h;
}

// ---------------------------------------------------------------------------
// Allocation helpers
// ---------------------------------------------------------------------------

static int round_up_pow2(int v)
{
	int p = 1;
	while (p < v)
		p <<= 1;
	return p;
}

static int round_down_pow2(int v)
{
	int p = 1;
	while ((p << 1) <= v)
		p <<= 1;
	return p;
}

static void pick_size(int width, int height, qboolean stretch, int min_size, int *out_w, int *out_h)
{
	int maxsize = (int)r_ps2_maxtexsize.value;
	int w, h;
	if (maxsize < 64) maxsize = 64;
	if (maxsize > 1024) maxsize = 1024;
	if (stretch && r_tex_scale_down.value) {
		w = round_down_pow2(width);
		h = round_down_pow2(height);
	} else {
		w = round_up_pow2(width);
		h = round_up_pow2(height);
	}
	if (w < min_size) w = min_size;
	if (h < min_size) h = min_size;
	while (w > maxsize) w >>= 1;
	while (h > maxsize) h >>= 1;
	*out_w = w;
	*out_h = h;
}

int Image_FindImage(const char *identifier)
{
	if (identifier[0]) {
		for (int i = 0; i < MAX_GLTEXTURES; ++i) {
			if (gltextures_used[i] && !strcmp(identifier, gltextures[i].identifier))
				return i;
		}
	}
	return -1;
}

int GL_GetTextureIndex()
{
	if (numgltextures == MAX_GLTEXTURES)
		Sys_Error("Out of gl textures");
	for (int i = 1; i < MAX_GLTEXTURES; ++i) {
		if (!gltextures_used[i]) {
			gltextures_used[i] = true;
			numgltextures++;
			return i;
		}
	}
	Sys_Error("Could not find a free gl texture!\n");
	return -1;
}

static gltexture_t *texture_begin(int index, const char *identifier, int orig_w, int orig_h,
	int width, int height, int format, int filter, qboolean keep)
{
	gltexture_t &t = gltextures[index];
	memset(&t, 0, sizeof(t));
	strlcpy(t.identifier, identifier, sizeof(t.identifier));
	t.original_width = orig_w;
	t.original_height = orig_h;
	t.stretch_to_power_of_two = (width != orig_w) || (height != orig_h);
	t.format = format;
	t.filter = filter;
	t.width = width;
	t.height = height;
	t.keep = keep;
	t.texnum = index;
	t.ram_bytes = GS_TextureBytes(width, height, format);
	t.ram = (texel *)PS2_MemAlloc(PS2_MEM_TEXTURES, t.ram_bytes, identifier);
	if (!t.ram)
		Sys_Error("Out of RAM for texture %s (%dx%d, %d bytes)", identifier, width, height, t.ram_bytes);
	gltextures_bytes += t.ram_bytes;
	return &t;
}

static void texture_finish(gltexture_t *t, const u32 *clut)
{
	GS_TextureSetup(&t->gs, t->ram, clut, t->width, t->height, t->format);
}

// nearest neighbour 8 bit resample (indices must not be blended)
static void resample8(const byte *in, int inwidth, int inheight, byte *out, int outwidth, int outheight)
{
	const unsigned int fracstep = inwidth * 0x10000 / outwidth;
	for (int i = 0; i < outheight; ++i, out += outwidth) {
		const byte *inrow = in + inwidth * (i * inheight / outheight);
		unsigned int frac = fracstep >> 1;
		for (int j = 0; j < outwidth; ++j, frac += fracstep)
			out[j] = inrow[frac >> 16];
	}
}

void GL_UnloadTexture(int texture_index)
{
	if (texture_index <= 0 || texture_index >= MAX_GLTEXTURES)
		return;
	if (!gltextures_used[texture_index]) return;
	if (gltextures[texture_index].keep) return;
	if (gltextures_is_permanent[texture_index]) return;

	gltexture_t &texture = gltextures[texture_index];
	GS_TextureRelease(&texture.gs);
	if (texture.palette)
		PS2_MemFree(PS2_MEM_TEXTURES, texture.palette);
	if (texture.ram) {
		PS2_MemFree(PS2_MEM_TEXTURES, texture.ram);
		gltextures_bytes -= texture.ram_bytes;
	}
	memset(&texture, 0, sizeof(texture));
	texture.texnum = -1;
	gltextures_used[texture_index] = false;
	numgltextures--;
	if (currenttexture == texture_index)
		currenttexture = -1;
}

void GL_UnloadAllTextures()
{
	for (int i = 0; i < MAX_GLTEXTURES; i++)
		GL_UnloadTexture(i);
}

// Called when the global palette changes: Quake palette textures reference
// gs_quake_clut and must re-upload their CLUT.
extern "C" void GL_QuakePaletteChanged(void)
{
	for (int i = 0; i < MAX_GLTEXTURES; i++)
		if (gltextures_used[i] && gltextures[i].gs.clut == gs_quake_clut)
			GS_TextureDirty(&gltextures[i].gs);
}

// ---------------------------------------------------------------------------
// Loaders
// ---------------------------------------------------------------------------

/*
================
GL_LoadTexture

8 bit texture in the Quake palette.
================
*/
int GL_LoadTexture (const char *identifier, int width, int height, byte *data, qboolean stretch_to_power_of_two, int filter, int mipmap_level)
{
	int texture_index = Image_FindImage(identifier);
	if (texture_index >= 0) return texture_index;
	(void)mipmap_level;

	int w, h;
	pick_size(width, height, stretch_to_power_of_two, 8, &w, &h);
	texture_index = GL_GetTextureIndex();
	gltexture_t *t = texture_begin(texture_index, identifier, width, height, w, h, GS_TEX_T8, filter, false);
	t->bpp = 1;
	if (w != width || h != height)
		resample8(data, width, height, t->ram, w, h);
	else
		memcpy(t->ram, data, w * h);
	texture_finish(t, gs_quake_clut);
	return texture_index;
}

/*
================
GL_LoadPalTex / GL_LoadTexture8to4

8 bit indexed texture with its own palette (WAD3 world textures, alias
skins, PCX images). Kept as PSMT8 with a private CLUT. Setting
r_ps2_worldtex4bit 1 falls back to the PSP 4 bit quantisation to halve
the memory.
================
*/
static int load_8bit_own_palette(const char *identifier, int width, int height, const byte *data,
	const byte *pal, int pal_bpp, int filter)
{
	int w, h;
	int transparent = identifier[0] == '{';
	pick_size(width, height, true, 8, &w, &h);

	int texture_index = GL_GetTextureIndex();
	gltexture_t *t = texture_begin(texture_index, identifier, width, height, w, h, GS_TEX_T8, filter, false);
	t->bpp = 1;
	if (w != width || h != height)
		resample8(data, width, height, t->ram, w, h);
	else
		memcpy(t->ram, data, w * h);

	t->palette = (u32 *)PS2_MemAlloc(PS2_MEM_TEXTURES, 256 * sizeof(u32), identifier);
	if (!t->palette)
		Sys_Error("Out of RAM for palette of %s", identifier);
	GS_BuildClut256(t->palette, pal, pal_bpp, transparent);
	texture_finish(t, t->palette);
	return texture_index;
}

int GL_LoadTexture4(const char *identifier, unsigned int width, unsigned int height, byte *data, int filter, qboolean swizzled)
{
	int texture_index = Image_FindImage(identifier);
	if (texture_index >= 0) return texture_index;
	(void)swizzled;   // PS2 data is always linear; the GS swizzles on upload

	texture_index = GL_GetTextureIndex();
	gltexture_t *t = texture_begin(texture_index, identifier, width, height, width, height, GS_TEX_T4, filter, false);
	t->bpp = 0;
	std::size_t buffer_size = (width * height) / 2;
	memcpy(t->ram, data, buffer_size);

	// 16 entry palette follows the texels (RGBA8888)
	t->palette = (u32 *)PS2_MemAlloc(PS2_MEM_TEXTURES, 16 * sizeof(u32), identifier);
	if (!t->palette)
		Sys_Error("Out of RAM for palette of %s", identifier);
	const byte *p = data + buffer_size;
	for (int i = 0; i < 16; i++)
		t->palette[i] = gs_color(p[i * 4], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]);
	texture_finish(t, t->palette);
	return texture_index;
}

int GL_LoadTexture8to4(const char *identifier, unsigned int width, unsigned int height, byte *data, const byte *pal, int filter, int inpal_bpp, const byte *palhint)
{
	int texture_index = Image_FindImage(identifier);
	if (texture_index >= 0) return texture_index;

	if (!r_ps2_worldtex4bit.value)
		return load_8bit_own_palette(identifier, width, height, data, pal, inpal_bpp, filter);

	// PSP compatible 4 bit path (memory saving option)
	int new_width, new_height;
	pick_size(width, height, true, 32, &new_width, &new_height);
	std::size_t resamp_size = new_width * new_height;
	byte *resamp_data = (byte *)PS2_MemAlloc(PS2_MEM_TEMP, resamp_size, "8to4 resample");
	byte *clut4data = (byte *)PS2_MemAlloc(PS2_MEM_TEMP, resamp_size / 2 + 16 * 4, "8to4 out");
	if (!resamp_data || !clut4data)
		Sys_Error("Out of RAM converting %s", identifier);
	if ((new_width != (int)width) || (new_height != (int)height))
		resample8(data, width, height, resamp_data, new_width, new_height);
	else
		memcpy(resamp_data, data, resamp_size);

	byte *clut4pal = &clut4data[resamp_size / 2];
	if (palhint != NULL) {
		memcpy(clut4pal, palhint, 16 * 4);
		convert_8bpp_to_4bpp_with_hint(resamp_data, pal, inpal_bpp, new_width, new_height, clut4data, palhint);
	} else {
		convert_8bpp_to_4bpp(resamp_data, pal, inpal_bpp, new_width, new_height, clut4data, clut4pal);
	}
	int id = GL_LoadTexture4(identifier, new_width, new_height, clut4data, filter, false);
	PS2_MemFree(PS2_MEM_TEMP, clut4data);
	PS2_MemFree(PS2_MEM_TEMP, resamp_data);
	return id;
}

int GL_LoadPalTex (const char *identifier, int width, int height, byte *data, qboolean stretch_to_power_of_two, int filter, int mipmap_level, byte *palette, int paltype)
{
	(void)stretch_to_power_of_two;
	(void)mipmap_level;
	int texture_index = Image_FindImage(identifier);
	if (texture_index >= 0) return texture_index;
	return GL_LoadTexture8to4(identifier, width, height, data, palette, filter, paltype == PAL_RGBA ? 4 : 3, NULL);
}

/*
================
GL_LoadImages

True colour images (TGA/PNG/JPG). Opaque and 1 bit alpha images are stored
as PSMCT16 (2 bytes/texel), images with real translucency as PSMCT32.
TODO: offline conversion of the PS2 asset profile to 8 bit indexed images.
================
*/
int GL_LoadImages (const char *identifier, int width, int height, byte *data, qboolean stretch_to_power_of_two, int filter, int mipmap_level, int bpp, qboolean keep)
{
	int texture_index = Image_FindImage(identifier);
	if (texture_index >= 0) return texture_index;
	(void)mipmap_level;

	if (bpp == 1)
		return GL_LoadTexture(identifier, width, height, data, stretch_to_power_of_two, filter, 0);

	int w, h;
	pick_size(width, height, stretch_to_power_of_two, 8, &w, &h);

	// Normalise to RGBA at the target size (one temporary).
	byte *rgba = data;
	byte *temp = NULL;
	if (bpp != 4 || w != width || h != height) {
		temp = (byte *)PS2_MemAlloc(PS2_MEM_TEMP, w * h * 4, "image resample");
		if (!temp)
			Sys_Error("Out of RAM resampling %s", identifier);
		if (bpp == 4 && (w != width || h != height)) {
			Image_Resample(data, width, height, temp, w, h, 4, 1);
		} else {
			// bpp 3 -> 4 (and resample with nearest if needed)
			for (int y = 0; y < h; y++) {
				const byte *row = data + (y * height / h) * width * bpp;
				for (int x = 0; x < w; x++) {
					const byte *s = row + (x * width / w) * bpp;
					byte *d = temp + (y * w + x) * 4;
					d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
					d[3] = bpp == 4 ? s[3] : 255;
				}
			}
		}
		rgba = temp;
	}

	// Classify alpha.
	int translucent = 0;
	for (int i = 0; i < w * h; i++) {
		byte a = rgba[i * 4 + 3];
		if (a != 0 && a != 255) {
			translucent = 1;
			break;
		}
	}

	texture_index = GL_GetTextureIndex();
	gltexture_t *t = texture_begin(texture_index, identifier, width, height, w, h,
		translucent ? GS_TEX_CT32 : GS_TEX_CT16, filter, keep);
	t->bpp = translucent ? 4 : 2;
	if (translucent) {
		u32 *out = (u32 *)t->ram;
		for (int i = 0; i < w * h; i++) {
			const byte *s = rgba + i * 4;
			out[i] = gs_color(s[0], s[1], s[2], s[3]);
		}
	} else {
		u16 *out = (u16 *)t->ram;
		for (int i = 0; i < w * h; i++) {
			const byte *s = rgba + i * 4;
			out[i] = (u16)((s[0] >> 3) | ((s[1] >> 3) << 5) | ((s[2] >> 3) << 10) | ((s[3] >= 128) << 15));
		}
	}
	if (temp)
		PS2_MemFree(PS2_MEM_TEMP, temp);
	texture_finish(t, NULL);
	return texture_index;
}

int GL_LoadTexture8Pal32 (char *identifier, int width, int height, byte *data, byte *pal)
{
	int texture_index = Image_FindImage(identifier);
	if (texture_index >= 0) return texture_index;
	return load_8bit_own_palette(identifier, width, height, data, pal, 4, GS_LINEAR);
}

int GL_LoadTexture8Pal24 (char *identifier, int width, int height, byte *data, byte *pal)
{
	int texture_index = Image_FindImage(identifier);
	if (texture_index >= 0) return texture_index;
	return load_8bit_own_palette(identifier, width, height, data, pal, 3, GS_LINEAR);
}
