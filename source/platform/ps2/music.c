/*
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
// music.c -- PlayStation 2 background music
//
// NOT IMPLEMENTED YET. NZ:P ships its soundtrack as MP3. Decoding MP3 on
// the EE (libmad is available) would cost a significant slice of the frame
// budget, so the intended PS2 design is to convert the tracks offline to
// SPU2 ADPCM (.vag style) and stream them from storage into SPU2 RAM voices,
// which the SPU2 decodes for free. Until then music_start_play() reports
// failure and the game runs without background music.

#include "../../nzportable_def.h"

volatile int music_job_started;
int music_volume;
qboolean music_paused;

int music_init(void)
{
	// snd_music.c treats 0 as a fatal init failure; music is optional.
	Con_Printf("PS2 music: not available in this build (see README_PS2.md)\n");
	return 1;
}

void music_deinit(void) {}

int music_start_play(char *fname, int pos)
{
	(void)fname;
	(void)pos;
	music_job_started = 0;
	return 2;   // "not playing" for snd_music.c
}

void music_pause(void) {}
void music_resume(void) {}
void music_stop(void) { music_job_started = 0; }
void music_update(void) {}
