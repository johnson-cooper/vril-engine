//gu_fullbright.c

extern "C"
{
#include "../../../nzportable_def.h"
}

void DrawGLPoly_ex (glpoly_t *p);

int FindFullbrightTexture (byte *pixels, int num_pix)
{
    int i;
    for (i = 0; i < num_pix; i++)
        if (pixels[i] > 223)
            return 1;


    return 0;
}


void ConvertPixels (byte *pixels, int num_pixels)
{
    int i;

    for (i = 0; i < num_pixels; i++)
        if (pixels[i] < 224)
            pixels[i] = 255;
}

void DrawFullBrightTextures (msurface_t *first_surf, int num_surfs)
{
    // gl_texsort 1 version
    int i;
    msurface_t *fa;
    texture_t *t;

    if (r_fullbright.value)
        return;

    for (fa = first_surf, i = 0; i < num_surfs; fa++, i++)
    {
        // find the correct texture
        t = R_TextureAnimation (fa->texinfo->texture);

        if (t->fullbright != -1 && fa->draw_this_frame)
        {
            GS_Disable(GS_CAP_FOG); //Disable fog for fullbright textures
		    GS_Enable(GS_CAP_BLEND);
            GS_TexFunc(GS_TFX_REPLACE, GS_TCC_RGBA);
			GL_Bind (t->fullbright);
            DrawGLPoly_ex (fa->polys);
            GS_Disable (GS_CAP_BLEND);
            GS_TexFunc(GS_TFX_REPLACE, GS_TCC_RGBA);
            GS_Enable(GS_CAP_FOG);
		}

        fa->draw_this_frame = 0;
    }
}


