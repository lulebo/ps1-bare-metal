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
 * CD-ROM controller driver
 *
 * The controller exposes four 8-bit I/O registers at 0x1f801800-0x1f801803.
 * The first byte (CDROM_ADDRESS / CDROM_HSTS) doubles as an index register
 * on write and a status register on read. Writing 0-3 to it selects which
 * "bank" the remaining three registers present:
 *
 *   Bank 0 write: COMMAND (0x801), PARAMETER fifo (0x802), REQUEST (0x803)
 *   Bank 1 read:  RESPONSE fifo (0x801), INTERRUPT FLAG (0x803)
 *   Bank 1 write: INTERRUPT MASK (0x802), INTERRUPT CLEAR (0x803)
 *
 * All reads from 0x800 return the status register regardless of bank.
 *
 * Typical command flow:
 *   1. Wait for BUSYSTS in HSTS to clear (controller not busy).
 *   2. Bank 1: write 0x40 to HCLRCTL to flush the parameter FIFO.
 *   3. Bank 0: write parameters to PARAMETER, then the opcode to COMMAND.
 *   4. Poll HINTSTS (bank 1, 0x803) until bits [2:0] are non-zero.
 *   5. Read response bytes from RESULT (bank 1, 0x801).
 *   6. Acknowledge: write 0x1f to HCLRCTL to clear the interrupt flags.
 *
 * Multi-response commands (e.g. ReadN) produce INT3 immediately and INT1
 * per sector; INT3 must be acknowledged before INT1 can arrive.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "cdrom.h"
#include "registers.h"

/*
 * Timeout loops. Each inner iteration is ~2 CPU cycles at 33.8688 MHz,
 * giving roughly 60 ns per loop.
 *   TIMEOUT_CMD  ~600 ms — command acknowledgement, normal operations
 *   TIMEOUT_SEEK ~3 s    — seek + first sector, motor spinup (Init)
 *   TIMEOUT_STOP ~300 ms — Pause completion
 */
#define TIMEOUT_CMD   10000000
#define TIMEOUT_SEEK  50000000
#define TIMEOUT_STOP   5000000

/*
 * Internal DMA target. The DMA engine writes directly to physical RAM,
 * bypassing the CPU data cache. We keep a dedicated buffer here so we can
 * read the result back through the uncached KSEG1 mirror after the transfer.
 * Must be 4-byte aligned for DMA.
 */
static uint32_t _dmaBuf[CDROM_SECTOR_SIZE / 4] __attribute__((aligned(4)));

/* --------------------------------------------------------------------------
 * Low-level register helpers
 * -------------------------------------------------------------------------- */

static CDROMIRQType _waitIRQ(int timeout) {
	for (; timeout > 0; timeout--) {
		CDROM_ADDRESS = 1;
		uint8_t f = CDROM_HINTSTS & 7;
		if (f)
			return (CDROMIRQType)f;
	}
	return CDROM_IRQ_NONE;
}

static void _ackIRQ(void) {
	CDROM_ADDRESS = 1;
	CDROM_HCLRCTL = 0x1f; // clear all interrupt and buffer flags
}

static void _drainResult(void) {
	CDROM_ADDRESS = 1;
	while (CDROM_HSTS & CDROM_HSTS_RSLRRDY)
		(void)CDROM_RESULT;
}

static void _sendCmd(CDROMCommand cmd, const uint8_t *params, int n) {
	// Wait for the controller to finish any in-flight operation
	while (CDROM_HSTS & CDROM_HSTS_BUSYSTS)
		__asm__ volatile("");

	// Reset the parameter FIFO so leftover bytes from a previous command
	// do not get mixed in with our new parameters
	CDROM_ADDRESS = 1;
	CDROM_HCLRCTL = CDROM_HCLRCTL_CLRPRM;

	CDROM_ADDRESS = 0;
	for (int i = 0; i < n; i++)
		CDROM_PARAMETER = params[i];
	CDROM_COMMAND = (uint8_t)cmd;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

bool cdrom_init(void) {
	// Enable CD-ROM DMA channel in the global DMA priority register
	DMA_DPCR |= DMA_DPCR_CH_ENABLE(DMA_CDROM);

	/*
	 * Init (0x0a): reset internal state, spin the motor on, and restore
	 * default settings. This is the only command that returns two responses:
	 *   INT3 (acknowledge)  — arrives almost immediately
	 *   INT2 (complete)     — arrives ~900 ms later once the motor is up
	 */
	_sendCmd(CDROM_CMD_INIT, NULL, 0);

	if (_waitIRQ(TIMEOUT_CMD) != CDROM_IRQ_ACKNOWLEDGE)
		return false;
	_drainResult();
	_ackIRQ();

	if (_waitIRQ(TIMEOUT_SEEK) != CDROM_IRQ_COMPLETE)
		return false;
	_drainResult();
	_ackIRQ();

	// Setmode: 2x speed, 2048-byte data-only sectors (no sync/header bytes)
	uint8_t mode = CDROM_MODE_SPEED_2X | CDROM_MODE_SIZE_2048;
	_sendCmd(CDROM_CMD_SETMODE, &mode, 1);

	if (_waitIRQ(TIMEOUT_CMD) != CDROM_IRQ_ACKNOWLEDGE)
		return false;
	_drainResult();
	_ackIRQ();

	return true;
}

bool cdrom_readSector(uint32_t lba, void *buf) {
	CDROMIRQType irq;

	// Setloc: tell the controller where to seek before the next read command
	CDROMMSF msf;
	cdrom_convertLBAToMSF(&msf, lba);
	uint8_t loc[3] = { msf.minute, msf.second, msf.frame };

	_sendCmd(CDROM_CMD_SETLOC, loc, 3);
	if (_waitIRQ(TIMEOUT_CMD) != CDROM_IRQ_ACKNOWLEDGE)
		return false;
	_drainResult();
	_ackIRQ();

	/*
	 * ReadN (0x06): seek to the Setloc position and start reading sectors
	 * continuously. INT3 arrives when the command is accepted; INT1 fires
	 * once each time a sector has been buffered and is ready to transfer.
	 * INT3 must be acknowledged before INT1 can arrive.
	 */
	_sendCmd(CDROM_CMD_READ_N, NULL, 0);

	if (_waitIRQ(TIMEOUT_CMD) != CDROM_IRQ_ACKNOWLEDGE)
		return false;
	_drainResult();
	_ackIRQ();

	// Wait for the first sector (includes seek time)
	if (_waitIRQ(TIMEOUT_SEEK) != CDROM_IRQ_DATA_READY)
		return false;
	_drainResult();
	_ackIRQ();

	/*
	 * Transfer the sector from the drive's internal buffer to RAM via DMA
	 * channel 3 (CDROM). Steps:
	 *   1. Set BFRD in HCHPCTL (bank 0) to open the data buffer for reading.
	 *   2. Programme DMA: destination = physical address of _dmaBuf,
	 *      length = 512 words (2048 bytes), burst mode, device→RAM.
	 *   3. Wait for the DMA enable bit to clear (transfer complete).
	 *   4. Clear BFRD to let the drive know we are done with the buffer.
	 */
	CDROM_ADDRESS = 0;
	CDROM_HCHPCTL = CDROM_HCHPCTL_BFRD;

	uint32_t physAddr        = (uint32_t)_dmaBuf & 0x00ffffff;
	DMA_MADR(DMA_CDROM)      = physAddr;
	DMA_BCR(DMA_CDROM)       = CDROM_SECTOR_SIZE / 4; // words
	DMA_CHCR(DMA_CDROM)      = DMA_CHCR_READ           // bit 0 = 0: device→RAM
	                          | DMA_CHCR_MODE_BURST
	                          | DMA_CHCR_ENABLE
	                          | DMA_CHCR_TRIGGER;

	while (DMA_CHCR(DMA_CDROM) & DMA_CHCR_ENABLE)
		__asm__ volatile("");

	CDROM_ADDRESS = 0;
	CDROM_HCHPCTL = 0;

	/*
	 * The DMA wrote directly to physical RAM, bypassing the CPU data cache.
	 * Reading _dmaBuf through its KSEG0 address could return stale cached
	 * bytes. Instead read through the uncached KSEG1 mirror (0xa0000000 base)
	 * so the CPU is forced to fetch from RAM.
	 */
	const uint8_t *src = (const uint8_t *)(0xa0000000u | physAddr);
	memcpy(buf, src, CDROM_SECTOR_SIZE);

	/*
	 * Pause (0x09): stop the continuous read that ReadN started.
	 * ReadN may have already queued INT1 for the next sector by the time we
	 * get here, so discard any stale data-ready interrupts before waiting
	 * for Pause's own INT3 acknowledgement.
	 */
	_sendCmd(CDROM_CMD_PAUSE, NULL, 0);

	do {
		irq = _waitIRQ(TIMEOUT_CMD);
		if (irq == CDROM_IRQ_DATA_READY) {
			_drainResult();
			_ackIRQ();
		}
	} while (irq == CDROM_IRQ_DATA_READY);

	if (irq != CDROM_IRQ_ACKNOWLEDGE)
		return false;
	_drainResult();
	_ackIRQ();

	// INT2 = drive has fully stopped
	if (_waitIRQ(TIMEOUT_STOP) != CDROM_IRQ_COMPLETE)
		return false;
	_drainResult();
	_ackIRQ();

	return true;
}
