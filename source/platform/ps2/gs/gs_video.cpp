/*
Copyright (C) 2007 Peter Mackay and Chris Swindle.
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
// gs_video.cpp -- PS2 video mode, palette and frame begin/end

#include <limits.h>

extern "C"
{
#include "../../../nzportable_def.h"
#include "../ps2_profile.h"
}

// Regular globals.
u32 d_8to24table[256] __attribute__((aligned(16)));
int reloaded_pallete = 1;

extern cvar_t r_vsync;
extern cvar_t r_dithering;
extern "C" void GL_QuakePaletteChanged(void);

// 30 fps target on 60 Hz (25 on 50 Hz): flip every second vblank.
cvar_t r_ps2_vsyncinterval = {"r_ps2_vsyncinterval", "2", true};
cvar_t r_ps2_z24 = {"r_ps2_z24", "0", true};

void VID_SetPaletteTX() {}
void VID_SetPaletteLM() {}

void VID_SetPalette(unsigned char* palette)
{
	unsigned char *p = palette;
	for (int i = 0; i < 256; i++, p += 3)
		d_8to24table[i] = GS_RGBA(p[0], p[1], p[2], 0xff);
	// Color 255 is transparent black (Quake convention).
	d_8to24table[255] = 0;

	GS_BuildClut256(gs_quake_clut, palette, 3, 1);
	GL_QuakePaletteChanged();
	reloaded_pallete = 1;
}

void VID_ShiftPalette(unsigned char* palette)
{
	VID_SetPalette(palette);
}

void VID_Init(unsigned char* palette)
{
	int force = COM_CheckParm("-ntsc") ? 1 : COM_CheckParm("-pal") ? 2 : 0;

	Cvar_RegisterVariable(&r_ps2_vsyncinterval);
	Cvar_RegisterVariable(&r_ps2_z24);

	GS_Init(force, COM_CheckParm("-z24") ? 1 : 0);

	// initial render state (mirrors the PSP GU setup)
	GS_Offset(2048 - (gs_video.width / 2), 2048 - (gs_video.height / 2));
	GS_Viewport(2048, 2048, gs_video.width, gs_video.height);
	GS_Enable(GS_CAP_SCISSOR_TEST);
	GS_Scissor(0, 0, gs_video.width, gs_video.height);
	GS_Enable(GS_CAP_TEXTURE_2D);
	GS_ClearDepth(65535);
	GS_ClearColor(GS_RGBA(0x00, 0x00, 0x00, 0xff));
	GS_DepthRange(0, 65535);
	GS_DepthFunc(GS_LEQUAL);
	GS_Enable(GS_CAP_DEPTH_TEST);
	GS_MatrixMode(GS_PROJECTION);
	GS_LoadIdentity();
	GS_MatrixMode(GS_VIEW);
	GS_LoadIdentity();
	GS_MatrixMode(GS_MODEL);
	GS_LoadIdentity();
	GS_UpdateMatrix();
	GS_Enable(GS_CAP_CULL_FACE);
	GS_BlendFunc(GS_ADD, GS_SRC_ALPHA, GS_ONE_MINUS_SRC_ALPHA, 0, 0);

	vid.aspect			= ((float)gs_video.height / (float)gs_video.width) * (4.0f / 3.0f);
	vid.buffer			= 0;
	vid.colormap		= host_colormap;
	vid.colormap16		= 0;
	vid.conbuffer		= 0;
	vid.conheight		= gs_video.height;
	vid.conrowbytes		= 0;
	vid.conwidth		= gs_video.width;
	vid.direct			= 0;
	vid.fullbright		= 256 - LittleLong(*((int *) vid.colormap + 2048));
	vid.height			= gs_video.height;
	vid.maxwarpheight	= gs_video.width;
	vid.maxwarpwidth	= gs_video.height;
	vid.numpages		= INT_MAX;
	vid.recalc_refdef	= 0;
	vid.rowbytes		= 0;
	vid.width			= gs_video.width;
	vid.scale			= gs_video.height / STD_UI_HEIGHT;

	VID_SetPalette(palette);

	GS_BeginFrame();
	GS_Clear(GS_COLOR_BUFFER_BIT | GS_DEPTH_BUFFER_BIT);

	Con_Printf("GS video: %s %dx%d, 16 bit colour, Z%d, UI scale %d\n",
		gs_video.pal ? "PAL" : "NTSC", gs_video.width, gs_video.height,
		(gs_video.z_psm & 0xf) == 1 ? 24 : 16, vid.scale);
}

void VID_Shutdown(void)
{
	GS_Shutdown();
}

void GL_BeginRendering (int *x, int *y, int *width, int *height)
{
	*x = 2048;
	*y = 2048;
	*width = gs_video.width;
	*height = gs_video.height;
	if (r_dithering.value)
		GS_Enable(GS_CAP_DITHER);
	else
		GS_Disable(GS_CAP_DITHER);
}

void GL_EndRendering (void)
{
	int interval = (int)r_ps2_vsyncinterval.value;
	PS2_ProfileBegin(PROF_GS_SUBMIT);
	GS_EndFrame(interval);
	PS2_ProfileEnd(PROF_GS_SUBMIT);
	GS_BeginFrame();
}

/*
==================
SCR_ScreenShot_f
==================
*/
extern "C" void SCR_ScreenShot_f (void)
{
	Con_Printf ("Screenshots are not supported on PS2 yet\n");
}

// Frame buffer read back is not supported (the GS frame buffer is 16 bit and
// reading it back stalls the pipeline). Used by the envmap tool only.
void GL_GetPixelsRGBA(byte *buffer, int width, int height, int i)
{
	memset(buffer + i, 0, (size_t)width * height * 4);
}
