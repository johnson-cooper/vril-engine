/*
Copyright (C) 1996-1997 Id Software, Inc.
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
// platform_def.h -- PlayStation 2 specific header to compliment primary definitions
//                   (../nzportable_def.h). Keep this file as small as possible.

#ifndef PS2_PLATFORM_DEF_H
#define PS2_PLATFORM_DEF_H

// EE fixed-width types (u8/u16/u32/u64/u128). zone.c and the renderer use them.
#include <tamtypes.h>

// Memory accounting / diagnostics shared with the renderer.
#include "ps2_mem.h"

#endif // PS2_PLATFORM_DEF_H
