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
// ps2_sound.c -- Quake SNDDMA backend feeding nzpsnd.irx
//
// The engine mixer runs at 24 kHz stereo 16 bit into `dma_ring`. Every frame
// SNDDMA_Submit takes the next painted samples, upsamples them 2x (linear)
// to 48 kHz, converts to the SPU2 block layout and pushes whole 256-frame
// blocks to the IOP. The "DMA position" reported to the mixer is the
// position up to which samples have been handed to the IOP, so the mixer
// always paints ahead of what has been sent (snd_mixahead).
//
// Memory: dma_ring 32 KiB + staging 12 KiB on the EE; ~57 KiB on the IOP.
// Sound effects are resampled to 24 kHz at load time (snd_mem.c), which is
// what bounds the sound cache footprint.

#include "../../nzportable_def.h"
#include "ps2_iop.h"
#include "ps2_profile.h"
#include "iop/nzpsnd/nzpsnd_rpc.h"

#include <kernel.h>
#include <sifrpc.h>
#include <ps2sdkapi.h>

#define PS2_SND_SPEED      24000
#define DMA_RING_FRAMES    8192        // power of two, stereo frames (341 ms)
#define TARGET_QUEUE_BLOCKS 20         // keep ~107 ms queued on the IOP

static short dma_ring[DMA_RING_FRAMES * 2] __attribute__((aligned(64)));
static short push_buf[NZPSND_MAX_PUSH_BLOCKS * NZPSND_BLOCK_BYTES / 2] __attribute__((aligned(64)));
static nzpsnd_status_t snd_status __attribute__((aligned(64)));

extern int soundtime;       // snd_dma.c

static SifRpcClientData_t snd_client __attribute__((aligned(64)));
static int snd_bound;
static int sent_frames;     // 24 kHz frames handed to the IOP (mod ring)
static int snd_last_underruns;

int ps2_audio_bytes(void)
{
	return (int)(sizeof(dma_ring) + sizeof(push_buf));
}

static int snd_rpc(int cmd, void *send, int send_size)
{
	if (!snd_bound)
		return -1;
	if (send && send_size)
		SyncDCache(send, (u8 *)send + send_size);
	if (SifCallRpc(&snd_client, cmd, 0, send, send_size, &snd_status, sizeof(snd_status), NULL, NULL) < 0)
		return -1;
	return 0;
}

qboolean SNDDMA_Init(void)
{
	int tries = 0;

	if (!PS2_IOP_GroupReady(PS2_IOP_AUDIO)) {
		Con_Printf("PS2 audio: libsd/nzpsnd not loaded, sound disabled\n");
		return false;
	}

	memset(&snd_client, 0, sizeof(snd_client));
	while (SifBindRpc(&snd_client, NZPSND_RPC_ID, 0) < 0 || snd_client.server == NULL) {
		if (++tries > 100) {
			Con_Printf("PS2 audio: could not bind nzpsnd RPC\n");
			return false;
		}
		// server thread may not be running yet
		{
			ps2_clock_t t = ps2_clock();
			while (ps2_clock() - t < PS2_CLOCKS_PER_MSEC * 5)
				;
		}
	}
	snd_bound = 1;
	snd_rpc(NZPSND_CMD_INIT, NULL, 0);

	memset(dma_ring, 0, sizeof(dma_ring));
	shm = &sn;
	shm->splitbuffer = 0;
	shm->channels = 2;
	shm->samplebits = 16;
	shm->speed = PS2_SND_SPEED;
	shm->samples = DMA_RING_FRAMES * 2;
	shm->samplepos = 0;
	shm->submission_chunk = 1;
	shm->soundalive = true;
	shm->buffer = (unsigned char *)dma_ring;
	sent_frames = 0;

	Con_Printf("PS2 audio: %d Hz stereo via SPU2 core 0 (IOP ring %d ms)\n",
		PS2_SND_SPEED, NZPSND_RING_BLOCKS * NZPSND_BLOCK_FRAMES * 1000 / 48000);
	return true;
}

void SNDDMA_Shutdown(void)
{
	if (snd_bound) {
		int paused = 1;
		snd_rpc(NZPSND_CMD_PAUSE, &paused, sizeof(paused));
	}
}

int SNDDMA_GetDMAPos(void)
{
	shm->samplepos = (sent_frames & (DMA_RING_FRAMES - 1)) * 2;
	return shm->samplepos;
}

// Convert `blocks` * 128 source frames (24 kHz) starting at ring frame
// `src` into SPU2 blocks of 256 frames (48 kHz).
static void convert_blocks(int src, int blocks, short *out)
{
	int b, i;
	for (b = 0; b < blocks; b++) {
		short *l = out + b * 512;
		short *r = l + 256;
		for (i = 0; i < 128; i++) {
			int f0 = (src + i) & (DMA_RING_FRAMES - 1);
			int f1 = (f0 + 1) & (DMA_RING_FRAMES - 1);
			int l0 = dma_ring[f0 * 2], r0 = dma_ring[f0 * 2 + 1];
			int l1 = dma_ring[f1 * 2], r1 = dma_ring[f1 * 2 + 1];
			l[i * 2] = (short)l0;
			r[i * 2] = (short)r0;
			l[i * 2 + 1] = (short)((l0 + l1) >> 1);
			r[i * 2 + 1] = (short)((r0 + r1) >> 1);
		}
		src += 128;
	}
}

void SNDDMA_Submit(void)
{
	int want, blocks;
	ps2_clock_t t0;

	if (!snd_bound)
		return;
	t0 = ps2_clock();

	// last reply tells how much the IOP still has queued
	want = TARGET_QUEUE_BLOCKS - snd_status.queued_blocks;
	if (want > snd_status.free_blocks && snd_status.free_blocks >= 0)
		want = snd_status.free_blocks;
	if (want > NZPSND_MAX_PUSH_BLOCKS)
		want = NZPSND_MAX_PUSH_BLOCKS;

	// Never send samples the mixer has not painted yet. soundtime is derived
	// from SNDDMA_GetDMAPos (== sent_frames) and paintedtime runs ahead of it
	// by snd_mixahead, both in 24 kHz frames.
	{
		int painted_ahead = paintedtime - soundtime;
		if (painted_ahead < 0)
			painted_ahead = 0;
		if (want > painted_ahead / 128)
			want = painted_ahead / 128;
	}

	blocks = want;
	if (blocks > 0) {
		convert_blocks(sent_frames & (DMA_RING_FRAMES - 1), blocks, push_buf);
		snd_rpc(NZPSND_CMD_PUSH, push_buf, blocks * NZPSND_BLOCK_BYTES);
		sent_frames += blocks * 128;
	} else {
		snd_rpc(NZPSND_CMD_STATUS, NULL, 0);
	}

	if (snd_status.underruns != snd_last_underruns) {
		if (developer.value)
			Con_DPrintf("PS2 audio: %d underruns\n", snd_status.underruns - snd_last_underruns);
		snd_last_underruns = snd_status.underruns;
	}
	PS2_ProfileAdd(PROF_AUDIO, (unsigned int)(ps2_clock() - t0));
}
