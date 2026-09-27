/*
 * Copyright (C) 2023-2026 NZ:P Team
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
// gs_hyena.c -- Hyena (platform rendering interface) on the PS2 GS core

#include "../../../nzportable_def.h"

static int current_vertex_mode = -1;

void
Hyena_SetTextureMode(int texture_mode)
{
    switch (texture_mode) {
        case HYE_MODULATE:
            GS_TexFunc(GS_TFX_MODULATE, GS_TCC_RGBA);
            break;
        case HYE_REPLACE:
            GS_TexFunc(GS_TFX_REPLACE, GS_TCC_RGBA);
            break;
        default:
            Sys_Error("Received unknown texture mode [%d]\n", texture_mode);
            break;
    }
}

void
Hyena_SetColor(float red, float green, float blue, float alpha)
{
    GS_Color(GS_COLOR(red, green, blue, alpha));
}

static int
Hyena_ResolveCapability(int capability)
{
    switch (capability) {
        case HYE_BLEND:      return GS_CAP_BLEND;
        case HYE_CULL_FACE:  return GS_CAP_CULL_FACE;
        case HYE_TEXTURE_2D: return GS_CAP_TEXTURE_2D;
        default:
            Sys_Error("Received unknown capability [%d]\n", capability);
            return -1;
    }
}

void
Hyena_EnableCapability(int capability)
{
    GS_Enable(Hyena_ResolveCapability(capability));
}

void
Hyena_DisableCapability(int capability)
{
    GS_Disable(Hyena_ResolveCapability(capability));
}

void
Hyena_DepthMask(qboolean value)
{
    // GS_DepthMask(true) masks (disables) depth writes.
    GS_DepthMask(value ? GS_FALSE : GS_TRUE);
}

static int
Hyena_ResolveVertexMode(int mode)
{
    switch (mode) {
        case HYE_TRIANGLE_FAN:   return GS_TRIANGLE_FAN;
        case HYE_TRIANGLES:      return GS_TRIANGLES;
        case HYE_TRIANGLE_STRIP: return GS_TRIANGLE_STRIP;
        default:
            Sys_Error("Received mode capability [%d]\n", mode);
            return -1;
    }
}

void
Hyena_BeginVertices(int mode)
{
    current_vertex_mode = Hyena_ResolveVertexMode(mode);
    GS_PushMatrix();
}

void
Hyena_Translate(float x, float y, float z)
{
    const gs_vec3_t translation = { x, y, z };
    GS_Translate(&translation);
}

void
Hyena_Scale(float x, float y, float z)
{
    const gs_vec3_t scale = { x, y, z };
    GS_Scale(&scale);
}

void
Hyena_RotateXYZ(float x, float y, float z)
{
    const gs_vec3_t rotation = { x, y, z };
    GS_RotateXYZ(&rotation);
}

void
Hyena_RotateZYX(float z, float y, float x)
{
    const gs_vec3_t rotation = { x, y, z };
    GS_RotateZYX(&rotation);
}

void
Hyena_FlushMatrices(void)
{
    GS_UpdateMatrix();
}

vertex_t *
Hyena_AllocateMemoryForVertices(int num_vertices)
{
    return (vertex_t *) (GS_GetMemory(sizeof(vertex_t) * num_vertices));
}

void
Hyena_2DTextureCoord(vertex_t * vertex, float u, float v)
{
    vertex->uv.u = u;
    vertex->uv.v = v;
}

void
Hyena_VertexXYZ(vertex_t * vertex, float x, float y, float z)
{
    vertex->xyz.x = x;
    vertex->xyz.y = y;
    vertex->xyz.z = z;
}

void
Hyena_DrawVertices(vertex_t * vertices, int num_vertices, int texture_precision, int vertex_precision)
{
    (void)vertex_precision;   // only HYE_VERTEX_32BITFLOAT exists

    if (texture_precision == HYE_TEXTURE_NOTEXTURE) {
        // The GS core reads [u, v] only when told a texture coordinate
        // exists, so repack positions only.
        vertex_xyz_t *xyz = (vertex_xyz_t *)GS_GetMemory(sizeof(vertex_xyz_t) * num_vertices);
        int i;
        for (i = 0; i < num_vertices; i++)
            xyz[i] = vertices[i].xyz;
        GS_DrawArray(current_vertex_mode, GS_VERTEX_32BITF, num_vertices, 0, xyz);
    } else {
        GS_DrawArray(current_vertex_mode, GS_TEXTURE_32BITF | GS_VERTEX_32BITF, num_vertices, 0, vertices);
    }
}

// Alias models.
//
// The PSP-derived mesh builder (gs_mesh.cpp) emits command lists of
// strips/fans: count, then per vertex a packed uv (2 x s16, value/32767) and,
// for single pose models, a packed position (3 x s8). Multi pose models take
// positions from the trivertx poses (stored signed: v - 128 at load, as the
// GU 8 bit vertex format is signed). The model matrix applies
// scale_origin and scale * 128, so positions are submitted as v / 128.
//
// Pose interpolation is done here on the EE (the PSP used GU morphing).
typedef struct {
    float u, v;
    float x, y, z;
} alias_out_t;

void Hyena_DrawAliasBatch(const alias_batch_t *batch)
{
    const float inv128 = 1.0f / 128.0f;
    const float uvscale = 1.0f / 32767.0f;
    int count;

    if (!batch->command_words) {
        // Generic batch (R_BuildAliasBatch): indexed triangles.
        alias_out_t *out;
        int i;
        if (!batch->num_vertices)
            return;
        out = (alias_out_t *)GS_GetMemory(sizeof(alias_out_t) * batch->num_vertices);
        for (i = 0; i < batch->num_vertices; i++) {
            const alias_vertex_t *a = &batch->vertices[i];
            out[i].u = a->uv[0];
            out[i].v = a->uv[1];
            out[i].x = a->xyz[0] * (inv128 / 128.0f);
            out[i].y = a->xyz[1] * (inv128 / 128.0f);
            out[i].z = a->xyz[2] * (inv128 / 128.0f);
        }
        GS_DrawArray(GS_TRIANGLES, GS_TEXTURE_32BITF | GS_VERTEX_32BITF,
            batch->num_indices, batch->indices, out);
        return;
    }

    {
        const int *commands = batch->commands;
        const trivertx_t *pose1 = batch->pose1;
        const trivertx_t *pose2 = batch->pose2;
        const float blend = batch->blend;

        while ((count = *commands++) != 0) {
            int mode = count < 0 ? GS_TRIANGLE_FAN : GS_TRIANGLE_STRIP;
            alias_out_t *out;
            int i;
            if (count < 0)
                count = -count;
            out = (alias_out_t *)GS_GetMemory(sizeof(alias_out_t) * count);
            for (i = 0; i < count; i++) {
                const short *uv = (const short *)commands;
                out[i].u = uv[0] * uvscale;
                out[i].v = uv[1] * uvscale;
                commands++;
                if (batch->packed_static) {
                    const signed char *p = (const signed char *)commands;
                    out[i].x = p[0] * inv128;
                    out[i].y = p[1] * inv128;
                    out[i].z = p[2] * inv128;
                    commands++;
                } else if (pose2) {
                    const signed char *a = (const signed char *)pose1[i].v, *b = (const signed char *)pose2[i].v;
                    out[i].x = (a[0] + (b[0] - a[0]) * blend) * inv128;
                    out[i].y = (a[1] + (b[1] - a[1]) * blend) * inv128;
                    out[i].z = (a[2] + (b[2] - a[2]) * blend) * inv128;
                } else {
                    const signed char *a = (const signed char *)pose1[i].v;
                    out[i].x = a[0] * inv128;
                    out[i].y = a[1] * inv128;
                    out[i].z = a[2] * inv128;
                }
            }
            GS_DrawArray(mode, GS_TEXTURE_32BITF | GS_VERTEX_32BITF, count, 0, out);
            if (!batch->packed_static) {
                pose1 += count;
                if (pose2)
                    pose2 += count;
            }
        }
    }
}

void
Hyena_DrawSurfaceFan(const float *vertices, int count, int stride,
  int texture_offset, qboolean warp, double time)
{
    (void)stride; (void)texture_offset; (void)warp; (void)time;
    GS_DrawArray(GS_TRIANGLE_FAN, GS_TEXTURE_32BITF | GS_VERTEX_32BITF, count, 0, vertices);
}

void
Hyena_EndVertices(void)
{
    current_vertex_mode = -1;
    GS_PopMatrix();
}

void
Hyena_SetShadeMode(int shade_mode)
{
    GS_ShadeModel(shade_mode == HYE_FLAT ? GS_FLAT : GS_SMOOTH);
}

static int
Hyena_ResolveBlendFunction(int blend_function)
{
    switch (blend_function) {
        case HYE_ONE_MINUS_SRC_ALPHA: return GS_ONE_MINUS_SRC_ALPHA;
        case HYE_ONE:                 return GS_FIX;
        case HYE_ONE_MINUS_SRC_COLOR: return GS_ONE_MINUS_SRC_COLOR;
        case HYE_SRC_ALPHA:           return GS_SRC_ALPHA;
        default:
            Sys_Error("Received blend mode [%d]\n", blend_function);
            return -1;
    }
}

void
Hyena_SetBlendFunction(int source_blend, int dest_blend)
{
    GS_BlendFunc(GS_ADD, Hyena_ResolveBlendFunction(source_blend),
        Hyena_ResolveBlendFunction(dest_blend), 0xFFFFFFFF, 0xFFFFFFFF);
}

void
Hyena_SetDepthRange(float near_value, float far_value)
{
    GS_DepthRange((int) (65535.0f * near_value), (int) (65535.0f * far_value));
}

void
Hyena_SetDepthOffset(float offset)
{
    GS_DepthOffset((int) (offset * 256.0f));
}

void
Hyena_BindTexture(int texture)
{
    GL_Bind(texture);
}

void
Hyena_FogSet(bool is_world_geometry, float start, float end,
  float red, float green, float blue, float alpha)
{
    (void)is_world_geometry;
    GS_Fog(start, end, GS_COLOR(red * 0.01f, green * 0.01f, blue * 0.01f, alpha));
}

void Hyena_FogEnable(void) { GS_Enable(GS_CAP_FOG); }

void Hyena_FogDisable(void) { GS_Disable(GS_CAP_FOG); }

void Hyena_FogInit(void) { }
