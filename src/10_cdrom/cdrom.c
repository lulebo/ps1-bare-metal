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
 *
 * IMPORTANT: a pending interrupt must always be acknowledged, even when it is
 * not the one we expected. If a stale interrupt is left set, the controller
 * will refuse to raise a new one for the next command and every subsequent
 * wait will time out. _expectIRQ() below enforces this.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "ps1/cdrom.h"
#include "ps1/registers.h"
#include "cdrom.h"

/*
 * Timeout loops. Each iteration polls a CD-ROM I/O register, which sits on a
 * slow peripheral bus. The values must be generous enough for a real physical
 * drive — motor spin-up from a dead stop and a full-stroke seek before the
 * first sector can each take the better part of a second — yet small enough
 * that a genuinely unresponsive drive (e.g. no disc) fails in a few seconds
 * rather than freezing for tens of seconds. TIMEOUT_SEEK therefore covers the
 * worst-case spin-up + seek + first read; the command/stop timeouts are short
 * because those interrupts arrive promptly once the drive is spinning.
 *   TIMEOUT_CMD  — command acknowledgement, normal operations
 *   TIMEOUT_SEEK — seek + first sector, motor spinup (Init)
 *   TIMEOUT_STOP — Pause completion
 */
#define TIMEOUT_CMD    500000
#define TIMEOUT_SEEK 10000000
#define TIMEOUT_STOP  1000000

/*
 * Diagnostics. These record how far the last operation progressed and what
 * interrupt it last saw, so the example can show the failure point on screen.
 * See the CDROMStep enum in cdrom.h for the meaning of the step codes.
 */
volatile uint8_t cdrom_lastStep = CDROM_STEP_IDLE;
volatile uint8_t cdrom_lastIRQ  = CDROM_IRQ_NONE;

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
	// The result FIFO is 16 bytes deep; the guard stops us spinning forever if
	// the controller ever leaves RSLRRDY stuck set.
	for (int guard = 16; (CDROM_HSTS & CDROM_HSTS_RSLRRDY) && guard; guard--)
		(void)CDROM_RESULT;
}

// Discard any interrupt and response left pending from the BIOS or a previous
// command, so the controller will raise a clean interrupt for our next one.
static void _flushPending(void) {
	if (_waitIRQ(TIMEOUT_STOP) != CDROM_IRQ_NONE) {
		_drainResult();
		_ackIRQ();
	}
}

/*
 * Wait for an interrupt and check it matches what we expect. The pending
 * interrupt is always drained and acknowledged (when one arrived) regardless
 * of whether it matched, so a wrong/error interrupt can never wedge the
 * controller for the following command. Records diagnostics on the way.
 */
static bool _expectIRQ(CDROMIRQType expected, int timeout, CDROMStep step) {
	cdrom_lastStep = (uint8_t)step;

	CDROMIRQType irq = _waitIRQ(timeout);
	cdrom_lastIRQ = (uint8_t)irq;

	if (irq != CDROM_IRQ_NONE) {
		_drainResult();
		_ackIRQ();
	}
	return irq == expected;
}

// Wait for the drive to assert DRQSTS, i.e. its data FIFO is loaded and it is
// requesting a DMA transfer. Returns false on timeout.
static bool _waitDRQ(int timeout) {
	for (; timeout > 0; timeout--) {
		if (CDROM_HSTS & CDROM_HSTS_DRQSTS)
			return true;
	}
	return false;
}

static void _sendCmd(CDROMCommand cmd, const uint8_t *params, int n) {
	// Wait for the controller to finish any in-flight operation. Bounded so a
	// wedged controller cannot hang the whole program here.
	for (int timeout = TIMEOUT_CMD;
	     (CDROM_HSTS & CDROM_HSTS_BUSYSTS) && timeout;
	     timeout--)
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

	// The BIOS that booted this executable may have left an interrupt pending
	// (e.g. from its own GetID). Clear it first, otherwise our Init command
	// will not produce a fresh interrupt and every wait below will time out.
	_flushPending();

	/*
	 * Init (0x0a): reset internal state, spin the motor on, and restore
	 * default settings. This is the only command that returns two responses:
	 *   INT3 (acknowledge)  — arrives almost immediately
	 *   INT2 (complete)     — arrives ~900 ms later once the motor is up
	 */
	_sendCmd(CDROM_CMD_INIT, NULL, 0);
	if (!_expectIRQ(CDROM_IRQ_ACKNOWLEDGE, TIMEOUT_CMD, CDROM_STEP_INIT_ACK))
		return false;
	if (!_expectIRQ(CDROM_IRQ_COMPLETE, TIMEOUT_SEEK, CDROM_STEP_INIT_DONE))
		return false;

	// Setmode: 2x speed, 2048-byte data-only sectors (no sync/header bytes)
	uint8_t mode = CDROM_MODE_SPEED_2X | CDROM_MODE_SIZE_2048;
	_sendCmd(CDROM_CMD_SETMODE, &mode, 1);
	if (!_expectIRQ(CDROM_IRQ_ACKNOWLEDGE, TIMEOUT_CMD, CDROM_STEP_SETMODE))
		return false;

	cdrom_lastStep = CDROM_STEP_IDLE;
	return true;
}

bool cdrom_readSector(uint32_t lba, void *buf) {
	CDROMIRQType irq;

	// Setloc: tell the controller where to seek before the next read command
	CDROMMSF msf;
	cdrom_convertLBAToMSF(&msf, lba);
	uint8_t loc[3] = { msf.minute, msf.second, msf.frame };

	_sendCmd(CDROM_CMD_SETLOC, loc, 3);
	if (!_expectIRQ(CDROM_IRQ_ACKNOWLEDGE, TIMEOUT_CMD, CDROM_STEP_SETLOC))
		return false;

	/*
	 * ReadN (0x06): seek to the Setloc position and start reading sectors
	 * continuously. INT3 arrives when the command is accepted; INT1 fires
	 * once each time a sector has been buffered and is ready to transfer.
	 */
	_sendCmd(CDROM_CMD_READ_N, NULL, 0);
	if (!_expectIRQ(CDROM_IRQ_ACKNOWLEDGE, TIMEOUT_CMD, CDROM_STEP_READ_ACK))
		return false;

	// Wait for the first sector (includes seek time). Note: do NOT acknowledge
	// this INT1 yet — the sector data buffer is read out first, below.
	cdrom_lastStep = CDROM_STEP_READ_DATA;
	irq = _waitIRQ(TIMEOUT_SEEK);
	cdrom_lastIRQ = (uint8_t)irq;
	if (irq != CDROM_IRQ_DATA_READY) {
		if (irq != CDROM_IRQ_NONE) {
			_drainResult();
			_ackIRQ();
		}
		return false;
	}
	_drainResult();

	/*
	 * Transfer the sector from the drive's internal buffer to RAM via DMA
	 * channel 3 (CDROM). Steps:
	 *   1. Set BFRD in HCHPCTL (bank 0) to open the data buffer for reading.
	 *      This makes the controller load its data FIFO from the sector buffer.
	 *   2. Wait for DRQSTS: the FIFO is ready and the drive is asserting its
	 *      DMA request line (DREQ). The CD-ROM is a DREQ-driven DMA device, so
	 *      triggering the channel before DREQ is asserted leaves the channel's
	 *      enable bit stuck set forever — a 0 fps hang.
	 *   3. Programme DMA: destination = physical address of _dmaBuf,
	 *      length = 512 words (2048 bytes), burst mode, device→RAM.
	 *   4. Wait (bounded) for the DMA enable bit to clear (transfer complete).
	 *   5. Clear BFRD and acknowledge the sector interrupt.
	 */
	cdrom_lastStep = CDROM_STEP_DRQ;
	CDROM_ADDRESS = 0;
	CDROM_HCHPCTL = CDROM_HCHPCTL_BFRD;

	if (!_waitDRQ(TIMEOUT_CMD)) {
		CDROM_HCHPCTL = 0;
		_ackIRQ();
		return false;
	}

	cdrom_lastStep = CDROM_STEP_DMA;
	uint32_t physAddr        = (uint32_t)_dmaBuf & 0x00ffffff;
	DMA_MADR(DMA_CDROM)      = physAddr;
	DMA_BCR(DMA_CDROM)       = CDROM_SECTOR_SIZE / 4; // words
	DMA_CHCR(DMA_CDROM)      = DMA_CHCR_READ           // bit 0 = 0: device→RAM
	                          | DMA_CHCR_MODE_BURST
	                          | DMA_CHCR_ENABLE
	                          | DMA_CHCR_TRIGGER;

	int dmaTimeout = TIMEOUT_CMD;
	while ((DMA_CHCR(DMA_CDROM) & DMA_CHCR_ENABLE) && dmaTimeout)
		dmaTimeout--;

	CDROM_ADDRESS = 0;
	CDROM_HCHPCTL = 0;

	// Acknowledge the sector interrupt now that its data has been read out.
	_ackIRQ();

	if (!dmaTimeout)
		return false;

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
	cdrom_lastStep = CDROM_STEP_PAUSE_ACK;
	_sendCmd(CDROM_CMD_PAUSE, NULL, 0);

	do {
		irq = _waitIRQ(TIMEOUT_CMD);
		if (irq == CDROM_IRQ_DATA_READY) {
			_drainResult();
			_ackIRQ();
		}
	} while (irq == CDROM_IRQ_DATA_READY);

	cdrom_lastIRQ = (uint8_t)irq;
	if (irq != CDROM_IRQ_ACKNOWLEDGE) {
		if (irq != CDROM_IRQ_NONE) {
			_drainResult();
			_ackIRQ();
		}
		return false;
	}
	_drainResult();
	_ackIRQ();

	// INT2 = drive has fully stopped
	if (!_expectIRQ(CDROM_IRQ_COMPLETE, TIMEOUT_STOP, CDROM_STEP_PAUSE_DONE))
		return false;

	cdrom_lastStep = CDROM_STEP_IDLE;
	return true;
}
