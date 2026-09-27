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
// nzpsnd.c -- IOP PCM streaming module for NZ:P
//
// Why a custom module: the audsrv package installed with PS2BUILD is the
// voice-only build (no PCM streaming thread), and a small purpose-built
// streamer keeps IOP RAM use low (~60 KiB total).
//
// Data path: EE --SIF RPC--> ring (IOP RAM) --feeder thread--> SPU2 core 0
// block transfer buffer (two halves, looped by libsd).

#include "irx_imports.h"
#include "nzpsnd_rpc.h"

IRX_ID("nzpsnd", 1, 0);

// libsd parameter entries select the SPU2 core with bit 0.
#ifndef SD_CORE_0
#define SD_CORE_0 0
#define SD_CORE_1 1
#endif

#define TRANS_HALF_BYTES  (NZPSND_BLOCK_BYTES * 2)      // 512 frames per half (10.7 ms)
#define TRANS_BYTES       (TRANS_HALF_BYTES * 2)

static unsigned char trans_buf[TRANS_BYTES] __attribute__((aligned(64)));
static unsigned char ring[NZPSND_RING_BLOCKS * NZPSND_BLOCK_BYTES] __attribute__((aligned(64)));
static unsigned char rpc_buf[NZPSND_MAX_PUSH_BLOCKS * NZPSND_BLOCK_BYTES + 64] __attribute__((aligned(64)));

static volatile int ring_read;    // block index
static volatile int ring_write;   // block index
static volatile int ring_count;   // queued blocks
static volatile int underruns;
static volatile int played;
static volatile int paused;

static int feed_sema;
static SifRpcDataQueue_t rpc_queue;
static SifRpcServerData_t rpc_server;
static nzpsnd_status_t status_reply;

static int trans_callback(int channel, void *arg)
{
	(void)channel;
	(void)arg;
	iSignalSema(feed_sema);
	return 1;
}

static void fill_half(unsigned char *dst)
{
	int i, oldstat;
	for (i = 0; i < TRANS_HALF_BYTES / NZPSND_BLOCK_BYTES; i++) {
		unsigned char *out = dst + i * NZPSND_BLOCK_BYTES;
		CpuSuspendIntr(&oldstat);
		if (ring_count > 0 && !paused) {
			int blk = ring_read;
			ring_read = (ring_read + 1) % NZPSND_RING_BLOCKS;
			ring_count--;
			played++;
			CpuResumeIntr(oldstat);
			memcpy(out, ring + blk * NZPSND_BLOCK_BYTES, NZPSND_BLOCK_BYTES);
		} else {
			if (!paused)
				underruns++;
			CpuResumeIntr(oldstat);
			memset(out, 0, NZPSND_BLOCK_BYTES);
		}
	}
}

static void feeder_thread(void *arg)
{
	(void)arg;
	for (;;) {
		int half;
		WaitSema(feed_sema);
		// bit 24 tells which half the DMA is currently reading; refill the other.
		half = 1 - (sceSdBlockTransStatus(0, 0) >> 24);
		fill_half(trans_buf + half * TRANS_HALF_BYTES);
	}
}

static void fill_status(void)
{
	int oldstat;
	CpuSuspendIntr(&oldstat);
	status_reply.queued_blocks = ring_count;
	status_reply.free_blocks = NZPSND_RING_BLOCKS - ring_count;
	status_reply.underruns = underruns;
	status_reply.played_blocks = played;
	CpuResumeIntr(oldstat);
}

static void set_volume(int vol)
{
	if (vol < 0) vol = 0;
	if (vol > 0x3fff) vol = 0x3fff;
	sceSdSetParam(SD_CORE_0 | SD_PARAM_BVOLL, vol * 2 > 0x7fff ? 0x7fff : vol * 2);
	sceSdSetParam(SD_CORE_0 | SD_PARAM_BVOLR, vol * 2 > 0x7fff ? 0x7fff : vol * 2);
}

static void *rpc_handler(int cmd, void *data, int size)
{
	switch (cmd) {
	case NZPSND_CMD_INIT:
		break;
	case NZPSND_CMD_PUSH: {
		int blocks = size / NZPSND_BLOCK_BYTES;
		int i;
		for (i = 0; i < blocks; i++) {
			int oldstat, slot;
			CpuSuspendIntr(&oldstat);
			if (ring_count >= NZPSND_RING_BLOCKS) {
				CpuResumeIntr(oldstat);
				break;  // ring full: drop the rest (EE regulates, should not happen)
			}
			slot = ring_write;
			CpuResumeIntr(oldstat);
			memcpy(ring + slot * NZPSND_BLOCK_BYTES, (unsigned char *)data + i * NZPSND_BLOCK_BYTES, NZPSND_BLOCK_BYTES);
			CpuSuspendIntr(&oldstat);
			ring_write = (ring_write + 1) % NZPSND_RING_BLOCKS;
			ring_count++;
			CpuResumeIntr(oldstat);
		}
		break;
	}
	case NZPSND_CMD_STATUS:
		break;
	case NZPSND_CMD_VOLUME:
		if (size >= 4)
			set_volume(*(int *)data);
		break;
	case NZPSND_CMD_PAUSE:
		if (size >= 4)
			paused = *(int *)data;
		break;
	default:
		break;
	}
	fill_status();
	return &status_reply;
}

static void rpc_thread(void *arg)
{
	(void)arg;
	sceSifInitRpc(0);
	sceSifSetRpcQueue(&rpc_queue, GetThreadId());
	sceSifRegisterRpc(&rpc_server, NZPSND_RPC_ID, rpc_handler, rpc_buf, NULL, NULL, &rpc_queue);
	sceSifRpcLoop(&rpc_queue);
}

int _start(int argc, char *argv[])
{
	iop_thread_t thread;
	iop_sema_t sema;
	int tid;
	(void)argc;
	(void)argv;

	if (sceSdInit(0) < 0) {
		printf("nzpsnd: sceSdInit failed\n");
		return MODULE_NO_RESIDENT_END;
	}

	// Core 0 receives our PCM (ADMA input); its output is mixed into core 1.
	sceSdSetParam(SD_CORE_0 | SD_PARAM_MVOLL, 0x3fff);
	sceSdSetParam(SD_CORE_0 | SD_PARAM_MVOLR, 0x3fff);
	sceSdSetParam(SD_CORE_1 | SD_PARAM_MVOLL, 0x3fff);
	sceSdSetParam(SD_CORE_1 | SD_PARAM_MVOLR, 0x3fff);
	sceSdSetParam(SD_CORE_1 | SD_PARAM_AVOLL, 0x7fff);
	sceSdSetParam(SD_CORE_1 | SD_PARAM_AVOLR, 0x7fff);
	set_volume(0x3fff);

	sema.attr = 0;
	sema.option = 0;
	sema.initial = 0;
	sema.max = 2;
	feed_sema = CreateSema(&sema);

	memset(trans_buf, 0, sizeof(trans_buf));
	ring_read = ring_write = ring_count = 0;

	thread.attr = TH_C;
	thread.option = 0;
	thread.thread = feeder_thread;
	thread.stacksize = 0x800;
	thread.priority = 30;
	tid = CreateThread(&thread);
	if (tid <= 0)
		return MODULE_NO_RESIDENT_END;
	StartThread(tid, NULL);

	sceSdSetTransIntrHandler(0, trans_callback, NULL);
	sceSdBlockTrans(0, SD_TRANS_LOOP, trans_buf, TRANS_BYTES);

	thread.thread = rpc_thread;
	thread.stacksize = 0x800;
	thread.priority = 40;
	tid = CreateThread(&thread);
	if (tid <= 0)
		return MODULE_NO_RESIDENT_END;
	StartThread(tid, NULL);

	printf("nzpsnd: streaming to SPU2 core 0 (ring %d KiB)\n", (int)(sizeof(ring) / 1024));
	return MODULE_RESIDENT_END;
}
