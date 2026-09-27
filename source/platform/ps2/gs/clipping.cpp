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
// clipping.cpp -- PS2: world space polygon clipping is not needed.
//
// The PSP GU clips poorly, so the PSP renderer clipped polygons against the
// view frustum on the CPU before submission. The PS2 GS core (gs_core.c)
// already performs trivial accept/reject with outcodes and clips in clip
// space only when required, so this interface is kept for the shared
// surface code but never requests clipping.

#include "clipping.hpp"

namespace quake
{
	namespace clipping
	{
		void begin_frame(float regularfov, float wideclippingfov, float screenaspect)
		{
			(void)regularfov; (void)wideclippingfov; (void)screenaspect;
		}

		void begin_brush_model() {}
		void end_brush_model() {}

		bool is_clipping_required(const struct glvert_s* vertices, std::size_t vertex_count)
		{
			(void)vertices; (void)vertex_count;
			return false;
		}

		void clip(const struct glvert_s* unclipped_vertices, std::size_t unclipped_vertex_count,
			const struct glvert_s** clipped_vertices, std::size_t* clipped_vertex_count)
		{
			*clipped_vertices = unclipped_vertices;
			*clipped_vertex_count = unclipped_vertex_count;
		}
	}
}
