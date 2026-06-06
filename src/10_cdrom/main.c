/*
 * ps1-bare-metal - (C) 2023-2025 spicyjpeg
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This example shows how to read data from the CD-ROM drive at the bare-
 * metal level, with no BIOS assistance.
 *
 * On startup the drive is initialised, the ISO 9660 Primary Volume Descriptor
 * is read from sector 16, and the root directory is listed on screen.  You
 * can test it in DuckStation by attaching any ISO 9660 disc image via
 * File → Change Disc.
 *
 * The CD-ROM controller communicates through a bank-switched 4-byte I/O
 * window at 0x1f801800.  Writing 0-3 to the first byte selects the active
 * bank and changes what the remaining three bytes expose.  See cdrom.c for a
 * full explanation of the protocol.
 *
 * DMA channel 3 is used to burst-transfer a full 2048-byte sector from the
 * drive's internal ring buffer to RAM in one shot, which is far faster than
 * reading the DATA register byte by byte in a loop.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "cdrom.h"
#include "iso9660.h"
#include "font.h"
#include "gpu.h"
#include "ps1/gpucmd.h"
#include "ps1/registers.h"

#define SCREEN_WIDTH     320
#define SCREEN_HEIGHT    240
#define FONT_WIDTH        96
#define FONT_HEIGHT       56
#define FONT_COLOR_DEPTH GP0_COLOR_4BPP

#define MAX_ENTRIES 24

extern const uint8_t fontTexture[], fontPalette[];

// Static so it does not eat stack space
static ISODirEntry _entries[MAX_ENTRIES];

static GPUDMAChain _dmaChains[2];
static bool        _usingSecondFrame = false;

static void renderFrame(const TextureInfo *font, const char *text) {
	int bufferX = _usingSecondFrame ? SCREEN_WIDTH : 0;
	int bufferY = 0;

	GPUDMAChain *chain = &_dmaChains[_usingSecondFrame];
	_usingSecondFrame  = !_usingSecondFrame;

	uint32_t *ptr;

	GPU_GP1 = gp1_fbOffset(bufferX, bufferY);
	chain->nextPacket = chain->data;

	ptr    = allocateGP0Packet(chain, 4);
	ptr[0] = gp0_setPage(0, true, false);
	ptr[1] = gp0_fbOffset1(bufferX, bufferY);
	ptr[2] = gp0_fbOffset2(
		bufferX + SCREEN_WIDTH  - 1,
		bufferY + SCREEN_HEIGHT - 1
	);
	ptr[3] = gp0_fbOrigin(bufferX, bufferY);

	ptr    = allocateGP0Packet(chain, 3);
	ptr[0] = gp0_rgb(0, 0, 48) | gp0_vramFill();
	ptr[1] = gp0_xy(bufferX, bufferY);
	ptr[2] = gp0_xy(SCREEN_WIDTH, SCREEN_HEIGHT);

	printString(chain, font, 8, 8, text);

	*(chain->nextPacket) = gp0_endTag(0);

	waitForGP0Ready();
	waitForVSync();
	sendGPULinkedList(chain->data);
}

int main(int argc, const char **argv) {
	initSerialIO(115200);

	if ((GPU_GP1 & GP1_STAT_FB_MODE_BITMASK) == GP1_STAT_FB_MODE_PAL)
		setupGPU(GP1_MODE_PAL, SCREEN_WIDTH, SCREEN_HEIGHT);
	else
		setupGPU(GP1_MODE_NTSC, SCREEN_WIDTH, SCREEN_HEIGHT);

	TextureInfo font;
	uploadIndexedTexture(
		&font,
		fontTexture, fontPalette,
		SCREEN_WIDTH * 2, 0,
		SCREEN_WIDTH * 2, FONT_HEIGHT,
		FONT_WIDTH, FONT_HEIGHT,
		FONT_COLOR_DEPTH
	);

	// Phase 1: wait for disc. Render a holding screen between each attempt
	// so the display is live while cdrom_init() blocks waiting for the motor.
	// The first frame is drawn *before* the initial cdrom_init() call so the
	// screen is never blank, even when a disc is already present at boot.
	renderFrame(&font, "CD-ROM Example\n\nInitializing drive...");

	while (!cdrom_init()) {
		char waitBuf[256];
		sprintf(waitBuf,
			"CD-ROM Example\n\nWaiting for disc...\n\n"
			"Insert/boot a disc image, then\n"
			"reset (no disc needed at boot).\n\n"
			"init: step=%d lastIRQ=%d",
			cdrom_lastStep, cdrom_lastIRQ);
		renderFrame(&font, waitBuf);
	}

	renderFrame(&font, "CD-ROM Example\n\nReading disc...");

	// Phase 2: disc is ready — do all blocking reads before entering the
	// render loop, then display the result.
	char displayBuf[1280];
	char *p = displayBuf;

	p += sprintf(p, "CD-ROM Example\n\n");

	char volLabel[33];
	if (!iso9660_readPVD(volLabel)) {
		p += sprintf(p,
			"FAILED: could not read PVD.\n"
			"step=%d lastIRQ=%d\n"
			"Disc may not be ISO 9660.",
			cdrom_lastStep, cdrom_lastIRQ);
	} else {
		p += sprintf(p, "Disc : %s\n\nRoot directory:\n", volLabel);

		int n = iso9660_listRoot(_entries, MAX_ENTRIES);

		if (n == 0) {
			p += sprintf(p, "  (empty)\n");
		} else {
			for (int i = 0; i < n && (p - displayBuf) < 1200; i++) {
				p += sprintf(
					p, "  %s%s\n",
					_entries[i].name,
					_entries[i].isDir ? "/" : ""
				);
			}
		}
	}

	// Phase 3: render loop — text is static, just redraw every frame.
	for (;;)
		renderFrame(&font, displayBuf);

	return 0;
}
