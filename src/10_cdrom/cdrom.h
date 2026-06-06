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
 * Minimal CD-ROM drive driver.
 *
 * This is the driver API for example 10. The register layout, command opcodes
 * and helper inlines it is built on top of live in the shared hardware header
 * "ps1/cdrom.h"; this header only declares the high-level driver entry points.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Bytes in one ISO 9660 Mode 1 / Mode 2 Form 1 data sector.
#define CDROM_SECTOR_SIZE 2048

// Operation step codes, used for diagnostics (see cdrom_lastStep). When a
// cdrom_* call fails, cdrom_lastStep names the stage it stopped at and
// cdrom_lastIRQ holds the interrupt value it saw there (a CDROMIRQType).
typedef enum {
	CDROM_STEP_IDLE       =  0, // not in an operation / last one succeeded
	CDROM_STEP_INIT_ACK   =  1, // Init: waiting for INT3 acknowledge
	CDROM_STEP_INIT_DONE  =  2, // Init: waiting for INT2 complete (motor up)
	CDROM_STEP_SETMODE    =  3, // Setmode: waiting for INT3 acknowledge
	CDROM_STEP_SETLOC     = 10, // Setloc: waiting for INT3 acknowledge
	CDROM_STEP_READ_ACK   = 11, // ReadN: waiting for INT3 acknowledge
	CDROM_STEP_READ_DATA  = 12, // ReadN: waiting for INT1 first sector
	CDROM_STEP_DRQ        = 13, // waiting for DRQSTS before DMA
	CDROM_STEP_DMA        = 14, // waiting for DMA transfer to finish
	CDROM_STEP_PAUSE_ACK  = 15, // Pause: waiting for INT3 acknowledge
	CDROM_STEP_PAUSE_DONE = 16  // Pause: waiting for INT2 complete
} CDROMStep;

// Diagnostics for the last (or in-progress) operation. cdrom_lastStep is a
// CDROMStep; cdrom_lastIRQ is the CDROMIRQType last observed at that step.
extern volatile uint8_t cdrom_lastStep;
extern volatile uint8_t cdrom_lastIRQ;

// Initialize the CD-ROM drive: spins up the motor, waits for ready, sets
// 2x speed and 2048-byte sector mode. Returns false if no disc is present
// or the drive does not respond within the timeout.
bool cdrom_init(void);

// Read one 2048-byte data sector at logical block address lba into buf.
// buf must be 4-byte aligned. Returns false on seek/read error or timeout.
bool cdrom_readSector(uint32_t lba, void *buf);
