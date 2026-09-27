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
// nzpsnd_rpc.h -- protocol between the EE sound backend and nzpsnd.irx
//
// The EE mixes Quake audio, upsamples it to 48 kHz and converts it into the
// SPU2 core input ("ADMA") block format: repeated 1024 byte blocks of
// 256 left samples followed by 256 right samples (signed 16 bit).
// nzpsnd.irx queues these blocks in an IOP ring and streams them to SPU2
// core 0 with a looping sceSdBlockTrans. When the ring runs dry the IOP
// outputs silence (never stale data).
#ifndef NZPSND_RPC_H
#define NZPSND_RPC_H

#define NZPSND_RPC_ID        0x4e5a5053   // 'NZPS'

#define NZPSND_CMD_INIT      1
#define NZPSND_CMD_PUSH      2   // send: n * NZPSND_BLOCK_BYTES, recv: nzpsnd_status_t
#define NZPSND_CMD_STATUS    3   // recv: nzpsnd_status_t
#define NZPSND_CMD_VOLUME    4   // send: int volume 0..0x3fff
#define NZPSND_CMD_PAUSE     5   // send: int paused

#define NZPSND_BLOCK_FRAMES  256
#define NZPSND_BLOCK_BYTES   1024          // 256 L + 256 R, s16
#define NZPSND_RING_BLOCKS   40            // ~213 ms at 48 kHz
#define NZPSND_MAX_PUSH_BLOCKS 12          // max blocks per RPC push (12 KiB)

typedef struct {
	int free_blocks;     // ring space available after this call
	int queued_blocks;   // blocks waiting to be played
	int underruns;       // total half-buffers that were filled with silence
	int played_blocks;   // total blocks consumed by the SPU2
} nzpsnd_status_t;

#endif
