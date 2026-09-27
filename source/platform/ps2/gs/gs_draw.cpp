/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2007 Peter Mackay and Chris Swindle.
Copyright (C) 2008-2009 Crow_bar.

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

// draw.c -- this is the only file outside the refresh that touches the
// vid buffer

#include <ctype.h>

extern "C"
{
#include "../../../nzportable_def.h"
}

#include "gs_resample.h"


byte		*draw_chars;				// 8*8 graphic characters

int			translate_texture;
int			char_texture;
//int			zombie_skins[3][8];
int			zombie_skins[2][2];
int         ref_texture;
int         nonetexture;

//=============================================================================
/* Support Routines */

byte		menuplyr_pixels[4096];

byte nontexdt[8][8] =
{
	{0x43,0x43,0x43,0x43, 0x53,0x53,0x53,0x53},
	{0x43,0x43,0x43,0x43, 0x53,0x53,0x53,0x53},
	{0x43,0x43,0x43,0x43, 0x53,0x53,0x53,0x53},
	{0x43,0x43,0x43,0x43, 0x53,0x53,0x53,0x53},

	{0x53,0x53,0x53,0x53, 0x43,0x43,0x43,0x43},
	{0x53,0x53,0x53,0x53, 0x43,0x43,0x43,0x43},
	{0x53,0x53,0x53,0x53, 0x43,0x43,0x43,0x43},
	{0x53,0x53,0x53,0x53, 0x43,0x43,0x43,0x43},

};

/*
================
R_CreateDlightImage
from Quake3
================
*/
#define	DLIGHT_SIZE	16
static void R_CreateDlightImage( void )
{
	int		x,y;
	byte	data[DLIGHT_SIZE][DLIGHT_SIZE];
	int		b;

	// make a centered inverse-square falloff blob for dynamic lighting
	for (x=0 ; x<DLIGHT_SIZE ; x++)
	{
		for (y=0 ; y<DLIGHT_SIZE ; y++)
		{
			float	d;

			d = ( DLIGHT_SIZE/2 - 0.5f - x ) * ( DLIGHT_SIZE/2 - 0.5f - x ) +
				( DLIGHT_SIZE/2 - 0.5f - y ) * ( DLIGHT_SIZE/2 - 0.5f - y );
			b = 4000 / d;

			if (b > 255)
			{

				b = 255;
			}
			else if ( b < 75 )
			{
				b = 0;
			}

			data[y][x] = b;
		}
	}
	ref_texture = GL_LoadTexture ("reftexture", DLIGHT_SIZE, DLIGHT_SIZE, (byte*)data, false, GS_LINEAR, 0);
}

// ! " # $ % & ' ( ) * _ , - . / 0
// 1 2 3 4 5 6 7 8 9 : ; < = > ? @
// A B C D E F G H I J K L M N O P
// Q R S T U V W X Y Z [ \ ] ^ _ `
// a b c d e f g h i j k l m n o p
// q r s t u v w x y z { | } ~
int font_kerningamount[96];

void InitKerningMap(void)
{
	// Initialize the kerning amount as 8px for each
	// char in the event we cant load the file.
	for(int i = 0; i < 96; i++) {
		font_kerningamount[i] = 8;
	}

    FILE *kerning_map = fopen(va("%s/gfx/kerning_map.txt", com_gamedir), "r");
    if (kerning_map == NULL) {
        return;
    }

    char buffer[1024];
    if (fgets(buffer, sizeof(buffer), kerning_map) != NULL) {
        char *token = strtok(buffer, ",");
        int i = 0;
        while (token != NULL && i < 96) {
            font_kerningamount[i++] = atoi(token);
            token = strtok(NULL, ",");
        }
    }

    fclose(kerning_map);
}

/*
===============
Draw_Init
===============
*/
void Draw_Init (void)
{
  	// load the console background and the charset
	// by hand, because we need to write the version
	// string into the background before turning
	// it into a texture

	nonetexture = GL_LoadTexture ("nonetexture", 8, 8, (byte*)nontexdt, false, GS_NEAREST, 0);
	GL_MarkTextureAsPermanent(nonetexture);
	R_CreateDlightImage();

	// now turn them into textures
	char_texture = Image_LoadImage ("gfx/charset", IMAGE_TGA, GS_NEAREST, true, false);
	if (char_texture < 0)// did not find a matching TGA...
		Sys_Error ("Could not load charset, make sure you have every folder and file installed properly\nDouble check that all of your files are in their correct places\nAnd that you have installed the game properly.\nRefer to the readme.txt file for help\n");
	
	Clear_LoadingFill ();

	InitKerningMap();
}



/*
================
Draw_Character

Draws one 8*8 graphics character with 0 being transparent.
It can be clipped to the top of the screen to allow the console to be
smoothly scrolled off.
================
*/
void Draw_Character (int x, int y, int num)
{
   	int	row, col;

	if (num == 32)
		return;		// space

	num &= 255;

	if (y <= -8)
		return;			// totally off screen

	row = num>>4;
	col = num&15;

	GL_Bind (char_texture);

	struct vertex
	{
		short u, v;
		short x, y, z;
	};

	vertex* const vertices = static_cast<vertex*>(GS_GetMemory(sizeof(vertex) * 2));

	vertices[0].u = col * 8;
	vertices[0].v = row * 8;
	vertices[0].x = x;
	vertices[0].y = y;
	vertices[0].z = 0;

	vertices[1].u = (col + 1) * 8;
	vertices[1].v = (row + 1) * 8;
	vertices[1].x = x + 8;
	vertices[1].y = y + 8;
	vertices[1].z = 0;

	GS_DrawArray(GS_SPRITES, GS_TEXTURE_16BIT | GS_VERTEX_16BIT | GS_TRANSFORM_2D, 2, 0, vertices);
}

/*
================
Draw_CharacterRGBA

This is the same as Draw_Character, but with RGBA color codes.
- cypress
================
*/
extern cvar_t scr_coloredtext;
void Draw_CharacterRGBA(int x, int y, int num, float r, float g, float b, float a, float scale)
{
	int	row, col;

	if (num == 32)
		return;		// space

	num &= 255;

	if (y <= -8)
		return;			// totally off screen

	row = num>>4;
	col = num&15;

	GL_Bind (char_texture);

	if (scr_coloredtext.value)
		GS_TexFunc(GS_TFX_MODULATE , GS_TCC_RGBA);

	struct vertex
	{
		short u, v;
		short x, y, z;
	};

	vertex* const vertices = static_cast<vertex*>(GS_GetMemory(sizeof(vertex) * 2));

	vertices[0].u = col * 8;
	vertices[0].v = row * 8;
	vertices[0].x = x;
	vertices[0].y = y;
	vertices[0].z = 0;

	vertices[1].u = (col + 1) * 8;
	vertices[1].v = (row + 1) * 8;
	vertices[1].x = x + 8;
	vertices[1].y = y + 8;
	vertices[1].z = 0;

	GS_Color(GS_COLOR(r/255, g/255, b/255, a/255));
	GS_DrawArray(GS_SPRITES, GS_TEXTURE_16BIT | GS_VERTEX_16BIT | GS_TRANSFORM_2D, 2, 0, vertices);

	if (scr_coloredtext.value)
        GS_TexFunc(GS_TFX_REPLACE , GS_TCC_RGBA);
}

/*
================
Draw_String
================
*/
void Draw_String (int x, int y, char *str)
{
	while (*str)
	{
		Draw_Character (x, y, *str);

		// Hooray for variable-spacing!
		if (*str == ' ')
			x += 4;
        else if ((int)*str < 33 || (int)*str > 126)
            x += 8;
        else
            x += (font_kerningamount[(int)(*str - 33)] + 1);

		str++;
	}
}

/*
=============
Draw_ColorPic
=============
*/
void Draw_ColorPic (int x, int y, int pic, float r, float g , float b, float a)
{
	if (pic < 1) return;

	GS_TexFunc(GS_TFX_MODULATE, GS_TCC_RGBA);

	const gltexture_t &glt = gltextures[pic];
	GL_Bind (glt.texnum);

	struct vertex
	{
		short			u, v;
		short			x, y, z;
	};

	vertex* const vertices = static_cast<vertex*>(GS_GetMemory(sizeof(vertex) * 2));

	vertices[0].u		= 0;
	vertices[0].v		= 0;

	vertices[0].x		= x;
	vertices[0].y		= y;
	vertices[0].z		= 0;

	vertices[1].u 		= glt.width;
	vertices[1].v 		= glt.height;

	vertices[1].x		= x + glt.original_width;
	vertices[1].y		= y + glt.original_height;
	vertices[1].z		= 0;

	GS_Color(GS_RGBA(
		static_cast<unsigned int>(r),
		static_cast<unsigned int>(g),
		static_cast<unsigned int>(b),
		static_cast<unsigned int>(a)));

	GS_DrawArray(
		GS_SPRITES,
		GS_TEXTURE_16BIT | GS_VERTEX_16BIT | GS_TRANSFORM_2D,
		2, 0, vertices);

    GS_Color(0xffffffff);
	GS_TexFunc(GS_TFX_REPLACE, GS_TCC_RGBA);
}

/*
=============
Draw_AlphaPic
=============
*/
void Draw_AlphaPic (int x, int y, int pic, float alpha)
{
	Draw_ColorPic(x, y, pic, 255, 255, 255, alpha);
}

/*
=============
Draw_Pic
=============
*/
void Draw_Pic (int x, int y, int pic)
{
	Draw_AlphaPic(x, y, pic, 255);
}

/*
=============
Draw_StretchPic
=============
*/
void Draw_StretchPic (int x, int y, int pic, int x_value, int y_value)
{
	if (pic < 1) return;

	const gltexture_t &glt = gltextures[pic];
	GL_Bind (glt.texnum);

	struct vertex
	{
		unsigned short			u, v;
		short			x, y, z;
	};


	vertex* const vertices = static_cast<vertex*>(GS_GetMemory(sizeof(vertex) * 2));
	vertices[0].u = 0;
	vertices[0].v = 0;
	vertices[0].x = x;
	vertices[0].y = y;
	vertices[0].z = 0;

	vertices[1].u = glt.width;
	vertices[1].v = glt.height;
	
	vertices[1].x = x + x_value;
	vertices[1].y = y + y_value;
	vertices[1].z = 0;

	GS_DrawArray(GS_SPRITES, GS_TEXTURE_16BIT | GS_VERTEX_16BIT | GS_TRANSFORM_2D, 2, 0, vertices);

}

/*
=============
Draw_ColoredStretchPic
=============
*/
void Draw_ColoredStretchPic (int x, int y, int pic, int x_value, int y_value, int r, int g, int b, int a)
{
	if (pic < 1) return;

	GS_TexFunc(GS_TFX_MODULATE, GS_TCC_RGBA);

	const gltexture_t &glt = gltextures[pic];
	GL_Bind (glt.texnum);

	struct vertex
	{
		unsigned short			u, v;
		short			x, y, z;
	};


	vertex* const vertices = static_cast<vertex*>(GS_GetMemory(sizeof(vertex) * 2));
	vertices[0].u = 0;
	vertices[0].v = 0;
	vertices[0].x = x;
	vertices[0].y = y;
	vertices[0].z = 0;

	vertices[1].u = glt.width;
	vertices[1].v = glt.height;

	vertices[1].x = x + x_value;
	vertices[1].y = y + y_value;
	vertices[1].z = 0;

	GS_Color(GS_RGBA(
		static_cast<unsigned int>(r),
		static_cast<unsigned int>(g),
		static_cast<unsigned int>(b),
		static_cast<unsigned int>(a)));

	GS_DrawArray(GS_SPRITES, GS_TEXTURE_16BIT | GS_VERTEX_16BIT | GS_TRANSFORM_2D, 2, 0, vertices);

	GS_Color(0xffffffff);
	GS_TexFunc(GS_TFX_REPLACE, GS_TCC_RGBA);
}

/*
=============
Draw_MenuPanningPic
=============
*/
void Draw_MenuPanningPic (int x, int y, int pic, int x_value, int y_value, float time)
{
	if (pic < 1) return;

	GS_TexFunc(GS_TFX_MODULATE, GS_TCC_RGBA);

	const gltexture_t &glt = gltextures[pic];
    GL_Bind(glt.texnum);

    struct vertex
    {
        float u, v;
        short x, y, z;
    };

    vertex* const vertices =
        static_cast<vertex*>(GS_GetMemory(sizeof(vertex) * 2));

	// 5% horizonstal crop
	const float zoom = glt.width * 0.05f;
    const float visible = glt.width - (zoom * 2.0f);

    // Pan amount (0 -> 1 over time)
    float duration = 7.0f;
	float t = time / duration;
	if (t > 1.0f) t = 1.0f;

	float offset = t * (zoom * 2.0f);

    float u1 = offset;
    float u2 = u1 + visible;
	float v1 = 0;
    float v2 = glt.height;

    vertices[0].u = u1;
    vertices[0].v = v1;
    vertices[0].x = x;
    vertices[0].y = y;
    vertices[0].z = 0;

    vertices[1].u = u2;
    vertices[1].v = v2;
    vertices[1].x = x + x_value;
    vertices[1].y = y + y_value;
    vertices[1].z = 0;

	GS_TexFilter(GS_LINEAR, GS_LINEAR);

    GS_DrawArray(
        GS_SPRITES,
        GS_TEXTURE_32BITF | GS_VERTEX_16BIT | GS_TRANSFORM_2D,
        2,
        0,
        vertices
    );

	GS_Color(GS_COLOR(1,1,1,1));
	GS_TexFunc(GS_TFX_REPLACE, GS_TCC_RGBA);

	GS_TexFilter(GS_NEAREST, GS_NEAREST);
}

/*
=============
Draw_SubPic
=============
*/
void Draw_SubPic (int x, int y, int pic, float s, float t, float s_coord_size, float t_coord_size, float scale, float r, float g , float b, float a)
{
	if (pic < 1) return;

	GS_TexFunc(GS_TFX_MODULATE, GS_TCC_RGBA);

	const gltexture_t &glt = gltextures[pic];
	GL_Bind (glt.texnum);

	struct vertex
	{
		unsigned short	u, v;
		short			x, y, z;
	};

	float width_scale = scale * (s_coord_size / t_coord_size);

	vertex* const vertices = static_cast<vertex*>(GS_GetMemory(sizeof(vertex) * 2));

	vertices[0].u = (s * glt.width);
	vertices[0].v = (t * glt.height);

	vertices[0].x = x;
	vertices[0].y = y;
	vertices[0].z = 0;

	vertices[1].u = ((s + s_coord_size) * glt.width);
	vertices[1].v = ((t + t_coord_size) * glt.height);

	vertices[1].x = x + (glt.original_width * width_scale);
	vertices[1].y = y + (glt.original_height * scale);
	vertices[1].z = 0;

	GS_Color(GS_RGBA(
		static_cast<unsigned int>(r),
		static_cast<unsigned int>(g),
		static_cast<unsigned int>(b),
		static_cast<unsigned int>(a)));

	GS_DrawArray(
		GS_SPRITES,
		GS_TEXTURE_16BIT | GS_VERTEX_16BIT | GS_TRANSFORM_2D,
		2, 0, vertices);

    GS_Color(0xffffffff);
	GS_TexFunc(GS_TFX_REPLACE, GS_TCC_RGBA);
}

/*
=============
Draw_TransPic
=============
*/
void Draw_TransPic (int x, int y, int pic)
{
	if (pic < 1) return;

	gltexture_t &texture = gltextures[pic];

	if (x < 0 || (unsigned)(x + texture.width) > vid.width || y < 0 ||
		 (unsigned)(y + texture.height) > vid.height)
	{
		Sys_Error ("bad coordinates");
	}

	Draw_Pic (x, y, pic);
}

/*
================
Draw_ConsoleBackground

================
*/
void Draw_ConsoleBackground (int lines)
{
	//int y = (vid.height * 3) >> 2;

	if (con_forcedup)
		Draw_Fill(0, 0, vid.width, lines, 0);
	else
		Draw_Fill(0, 0, vid.width, lines, 0);
}
/*
================
Draw_LoadingFill
By Crow_bar
================
*/
void Draw_LoadingFill(void)
{
	LoadingScreen_DrawProgressBar();
}

void Clear_LoadingFill (void)
{
	LoadingScreen_ClearProgress();
}

/*
=============
Draw_FillByColor

Fills a box of pixels with a single color
=============
*/
void Draw_FillByColor (int x, int y, int w, int h, int r, int g, int b, int a)
{
	unsigned int c = GS_RGBA(r, g, b, a);

	struct vertex
	{
		short x, y, z;
	};

	vertex* const vertices = static_cast<vertex*>(GS_GetMemory(sizeof(vertex) * 2));

	vertices[0].x = x;
	vertices[0].y = y;
	vertices[0].z = 0;

	vertices[1].x = x + w;
	vertices[1].y = y + h;
	vertices[1].z = 0;

	GS_Disable(GS_CAP_TEXTURE_2D);
	GS_Color(c);
	GS_DrawArray(GS_SPRITES, GS_VERTEX_16BIT | GS_TRANSFORM_2D, 2, 0, vertices);
	GS_Color(0xffffffff);
	GS_Enable(GS_CAP_TEXTURE_2D);
}

void Draw_Fill (int x, int y, int w, int h, int c)
{
	struct vertex
	{
		short x, y, z;
	};

	vertex* const vertices = static_cast<vertex*>(GS_GetMemory(sizeof(vertex) * 2));

	vertices[0].x = x;
	vertices[0].y = y;
	vertices[0].z = 0;

	vertices[1].x = x + w;
	vertices[1].y = y + h;
	vertices[1].z = 0;

	GS_Disable(GS_CAP_TEXTURE_2D);
	GS_Color(GS_RGBA(host_basepal[c*3], host_basepal[c*3+1], host_basepal[c*3+2], 0xff));
	GS_DrawArray(GS_SPRITES, GS_VERTEX_16BIT | GS_TRANSFORM_2D, 2, 0, vertices);
	GS_Color(0xffffffff);
	GS_Enable(GS_CAP_TEXTURE_2D);
}

byte *StringToRGB (char *s)
{
	byte		*col;
	static	byte	rgb[4];

	Cmd_TokenizeString ((char*)s);
	if (Cmd_Argc() == 3)
	{
		rgb[0] = (byte)Q_atoi(Cmd_Argv(0));
		rgb[1] = (byte)Q_atoi(Cmd_Argv(1));
		rgb[2] = (byte)Q_atoi(Cmd_Argv(2));
	}
	else
	{
		col = (byte *)&d_8to24table[(byte)Q_atoi((char*)s)];
		rgb[0] = col[0];
		rgb[1] = col[1];
		rgb[2] = col[2];
	}
	rgb[3] = 255;

	return rgb;
}

extern "C"	cvar_t	crosshair;
extern cvar_t crosshair;
extern qboolean croshhairmoving;
//extern cvar_t cl_zoom;
extern int hitmark;


/*
================
Draw_Crosshair
================
*/

extern float crosshair_opacity;
extern cvar_t cl_crosshair_debug;
extern cvar_t crosshair;
extern qboolean crosshair_pulse_grenade;

/*
================
Draw_ColoredString
================
*/
// Creds to UP Team for scale code - cypress
void Draw_ColoredString(int x, int y, char *text, float r, float g, float b, float a, float scale)
{
	int num;
	int scale_int = rint(scale);
	qboolean white = true;

	if (y <= -8)
		return;			// totally off screen

	if (!*text)
		return;
	
	GL_Bind (char_texture);
	
	
	
	if (scr_coloredtext.value)
		GS_TexFunc(GS_TFX_MODULATE , GS_TCC_RGBA);

	for ( ; *text; text++)
	{

		// cypress - Added 0-255 RGBA support (5/26/2020)
		GS_Color(GS_COLOR(r/255, g/255, b/255, a/255));

		num = *text & 255;
		
		if (num != 32 && num != (32 | 128))
		{
            float frow, fcol;

	        frow = num>>4;
	        fcol = num&15;

		 	struct vertex
	        {
		      short u, v;
		      short x, y, z;
	        };

         	vertex* const vertices = static_cast<vertex*>(GS_GetMemory(sizeof(vertex) * 2));

	        vertices[0].u = fcol * 8;
	        vertices[0].v = frow * 8;
	        vertices[0].x = x;
	        vertices[0].y = y;
	        vertices[0].z = 0;

	        vertices[1].u = (fcol + 1) * 8;
	        vertices[1].v = (frow + 1) * 8;
	        vertices[1].x = x + (8*scale_int);
	        vertices[1].y = y + (8*scale_int);
	        vertices[1].z = 0;

	        GS_DrawArray(GS_SPRITES, GS_TEXTURE_16BIT | GS_VERTEX_16BIT | GS_TRANSFORM_2D, 2, 0, vertices);
		}

		// Hooray for variable-spacing!
		if (*text == ' ')
			x += 4 * scale_int;
        else if ((int)*text < 33 || (int)*text > 126)
            x += 8 * scale_int;
        else
            x += (font_kerningamount[(int)(*text - 33)] + 1) * scale_int;
	}

	if (!white)
		GS_Color(GS_COLOR(1,1,1,1));

	if (scr_coloredtext.value)
        GS_TexFunc(GS_TFX_REPLACE , GS_TCC_RGBA);
}

int getTextWidth(char *str, float scale)
{
	int width = 0;

    for (int i = 0; i < strlen(str); i++) {
        // Hooray for variable-spacing!
		if (str[i] == ' ')
			width += 4 * (int)scale;
        else if ((int)str[i] < 33 || (int)str[i] > 126)
            width += 8 * (int)scale;
        else
            width += (font_kerningamount[(int)(str[i] - 33)] + 1) * (int)scale;
    }

	return width;
}


void Draw_ColoredStringCentered(int y, char *str, float r, float g, float b, float a, float scale)
{
	Draw_ColoredString((vid.width - getTextWidth(str, scale))/2, y, str, r, g, b, a, scale);
}

//=============================================================================
#ifdef NAA
//font.c : font.raw
//	bin2c font.raw font.c font
//font.c
extern"C"
{
 #include "font.c"
}
static int fontwidthtab[128] =
{
	10, 10, 10, 10,
	10, 10, 10, 10,
	10, 10, 10, 10,
	10, 10, 10, 10,

	10, 10, 10, 10,
	10, 10, 10, 10,
	10, 10, 10, 10,
	10, 10, 10, 10,

	10,  6,  8, 10, //   ! " #
	10, 10, 10,  6, // $ % & '
	10, 10, 10, 10, // ( ) * +
	 6, 10,  6, 10, // , - . /

	10, 10, 10, 10, // 0 1 2 3
	10, 10, 10, 10, // 6 5 8 7
	10, 10,  6,  6, // 10 9 : ;
	10, 10, 10, 10, // < = > ?

	16, 10, 10, 10, // @ A B C
	10, 10, 10, 10, // D E F G
	10,  6,  8, 10, // H I J K
	 8, 10, 10, 10, // L M N O

	10, 10, 10, 10, // P Q R S
	10, 10, 10, 12, // T U V W
	10, 10, 10, 10, // X Y Z [
	10, 10,  8, 10, // \ ] ^ _

	 6,  8,  8,  8, // ` a b c
	 8,  8,  6,  8, // d e f g
	 8,  6,  6,  8, // h i j k
	 6, 10,  8,  8, // l m n o

	 8,  8,  8,  8, // p q r s
	 8,  8,  8, 12, // t u v w
	 8,  8,  8, 10, // x y z {
	 8, 10,  8, 12  // | } ~
};

void Draw_FrontText(const char* text, int x, int y, unsigned int color, int fw) //Crow_bar
{
	int len = (int)strlen(text);

	if(!len)
	{
		return;
	}

	// Set the texture mode.
	GS_TexMode(GS_TEX_CT32, 0, 0, 0);
	GS_TexImage(0, 256, 128, 256, font);
    GS_TexFilter(GS_NEAREST, GS_NEAREST);

	GS_ShadeModel(GS_SMOOTH);

    GS_TexFunc(GS_TFX_MODULATE, GS_TCC_RGBA);
	GS_DepthMask(GS_TRUE);

	typedef struct
	{
		float s, t;
		unsigned int c;
		float x, y, z;
	} vertex;

	vertex* const v = static_cast<vertex*>(GS_GetMemory(sizeof(vertex) * 2 * len));

	int i;
	for(i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char)text[i];
		if(c < 32)
		{
			c = 0;
		}
		else if(c >= 128)
		{
			c = 0;
		}

		int tx = (c & 0x0F) << 4;
		int ty = (c & 0xF0);

		vertex* v0 = &v[i*2+0];
		vertex* v1 = &v[i*2+1];

		v0->s = (float)(tx + (fw ? ((16 - fw) >> 1) : ((16 - fontwidthtab[c]) >> 1)));
		v0->t = (float)(ty);
		v0->c = color;
		v0->x = (float)(x);
		v0->y = (float)(y);
		v0->z = 0.0f;

		v1->s = (float)(tx + 16 - (fw ? ((16 - fw) >> 1) : ((16 - fontwidthtab[c]) >> 1)));
		v1->t = (float)(ty + 16);
		v1->c = color;
		v1->x = (float)(x + (fw ? fw : fontwidthtab[c]));
		v1->y = (float)(y + 16);
		v1->z = 0.0f;

		x += (fw ? fw : fontwidthtab[c]);
	}
   	GS_DrawArray(GS_SPRITES, GS_TEXTURE_32BITF | GS_COLOR_8888 | GS_VERTEX_32BITF | GS_TRANSFORM_2D, len * 2, 0, v);

	GS_ShadeModel(GS_FLAT);

	GS_DepthMask(GS_FALSE);
	GS_TexFunc(GS_TFX_REPLACE, GS_TCC_RGBA);
}

void Draw_FrontTest (void)
{
   	Draw_FrontText("red",   0,  0, 0xFF0000FF, 0);
	Draw_FrontText("green", 0, 16, 0xFF00FF00, 0);
	Draw_FrontText("blue",  0, 32, 0xFFFF0000, 0);

	Draw_FrontText("free char width",     0, 64, 0xFFFFFFFF, 0);
	Draw_FrontText("block char width 10", 0, 80, 0xFFFFFFFF, 10);
	Draw_FrontText("block char width 16", 0, 96, 0xFFFFFFFF, 16);

	Draw_FrontText("opacity 100%", 0, 128, 0xFFFFFFFF, 0);
	Draw_FrontText("opacity  50%", 0, 144, 0x7FFFFFFF, 0);
	Draw_FrontText("opacity  10%", 0, 160, 0x18FFFFFF, 0);

	Draw_FrontText("I'm Crow_bar", 2, 194, 0x40FFFFFF, 0);
	Draw_FrontText("I'm Crow_bar", 0, 192, 0xFFFFFFFF, 0);

	static float t = 0.0f;
	t += 0.1f;

	unsigned int c = 0xFF000000 |
		(unsigned int)((sinf(t * 0.393f + 0.086f) / 2.0f + 0.5f) * 255.0f) << 16 |
		(unsigned int)((sinf(t * 0.444f + 0.854f) / 2.0f + 0.5f) * 255.0f) <<  8 |
		(unsigned int)((sinf(t * 0.117f + 1.337f) / 2.0f + 0.5f) * 255.0f) <<  0;

	Draw_FrontText("Hello World from pspdev", 0, 224, c, 0);
}
#endif
//=============================================================================

/*
================
Draw_FadeScreen

================
*/
void Draw_FadeScreen (void)
{
	struct vertex
	{
		short	x, y, z;
	};

	vertex* const vertices = static_cast<vertex*>(GS_GetMemory(sizeof(vertex) * 2));

	vertices[0].x		= 0;
	vertices[0].y		= 0;
	vertices[0].z		= 0;
	vertices[1].x		= vid.width;
	vertices[1].y		= vid.height;
	vertices[1].z		= 0;

	GS_Disable(GS_CAP_TEXTURE_2D);

	GS_Color(GS_RGBA(0, 0, 0, 0x80));
	GS_DrawArray(
		GS_SPRITES,
		GS_VERTEX_16BIT | GS_TRANSFORM_2D,
		2, 0, vertices);

	GS_Enable(GS_CAP_TEXTURE_2D);
}

//=============================================================================

/*
================
GL_Set2D

Setup as if the screen was 320*200
================
*/
void GL_Set2D (void)
{
	GS_Viewport (glx, gly, glwidth, glheight);
	GS_Scissor(0, 0, glwidth, glheight);
	// 2D pictures address whole textures in texel space: clamp so bilinear
	// filtering does not wrap the opposite edge in.
	GS_TexWrap(GS_CLAMP, GS_CLAMP);

	GS_Enable(GS_CAP_BLEND);
	GS_TexFunc(GS_TFX_REPLACE, GS_TCC_RGBA);
}

