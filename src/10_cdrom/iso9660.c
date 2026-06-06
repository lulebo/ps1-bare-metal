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
 * Minimal ISO 9660 filesystem reader
 *
 * An ISO 9660 disc begins with 16 sectors of "system area" that the
 * filesystem standard leaves unspecified — on PS1 discs these hold the
 * licence/region data. Sector 16 is always the Primary Volume Descriptor
 * (PVD), which acts as the root of the directory tree.
 *
 * PVD layout (partial):
 *   Offset   0 : Type code (1 = Primary VD)
 *   Offset   1 : "CD001" identifier + version byte
 *   Offset  40 : Volume identifier (32 bytes, space-padded)
 *   Offset 156 : Root directory record (34 bytes)
 *
 * Each directory record uses the following layout:
 *   Offset  0 : Total record length (uint8)
 *   Offset  1 : Extended attribute record length (uint8, usually 0)
 *   Offset  2 : Location of extent, little-endian (uint32)
 *   Offset  6 : Location of extent, big-endian (uint32) — redundant
 *   Offset 10 : Data length, little-endian (uint32)
 *   Offset 14 : Data length, big-endian (uint32) — redundant
 *   Offset 18 : Recording date and time (7 bytes)
 *   Offset 25 : File flags (bit 1 = directory)
 *   Offset 26 : File unit size
 *   Offset 27 : Interleave gap size
 *   Offset 28 : Volume sequence number (uint16 LE)
 *   Offset 30 : Volume sequence number (uint16 BE) — redundant
 *   Offset 32 : File identifier length (uint8)
 *   Offset 33 : File identifier (uppercase ASCII; files end with ";N" version)
 *
 * The first two entries in every directory are always "." (current, name
 * byte 0x00) and ".." (parent, name byte 0x01); we skip both.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "iso9660.h"
#include "ps1/cdrom.h"

#define PVD_LBA             16
#define PVD_VOL_ID_OFFSET   40
#define PVD_VOL_ID_LEN      32
#define PVD_ROOT_OFFSET    156

// Directory record field byte offsets
#define DR_RECLEN   0
#define DR_LBA      2   // uint32 little-endian
#define DR_SIZE     10  // uint32 little-endian
#define DR_FLAGS    25
#define DR_NAMELEN  32
#define DR_NAME     33

#define DR_FLAG_DIR (1 << 1)

// Sector buffers. Two are needed because listRoot reads the PVD and a
// directory sector in sequence and we do not want to overwrite one with the
// other while it is still being parsed.
static uint8_t _pvdBuf[CDROM_SECTOR_SIZE] __attribute__((aligned(4)));
static uint8_t _dirBuf[CDROM_SECTOR_SIZE] __attribute__((aligned(4)));

static uint32_t _rd32le(const uint8_t *p) {
	return (uint32_t)p[0]
	     | ((uint32_t)p[1] <<  8)
	     | ((uint32_t)p[2] << 16)
	     | ((uint32_t)p[3] << 24);
}

// Copy at most dstSize-1 bytes from src, stop early at ';' (version suffix).
static void _copyName(char *dst, const uint8_t *src, int srcLen, int dstSize) {
	int i = 0;
	for (; i < srcLen && i < dstSize - 1; i++) {
		if (src[i] == ';')
			break;
		dst[i] = (char)src[i];
	}
	dst[i] = '\0';
}

bool iso9660_readPVD(char *volLabel) {
	if (!cdrom_readSector(PVD_LBA, _pvdBuf))
		return false;

	// Verify Primary Volume Descriptor signature
	if (_pvdBuf[0] != 1 || memcmp(&_pvdBuf[1], "CD001", 5) != 0)
		return false;

	if (volLabel) {
		// Strip trailing spaces from the 32-byte volume identifier
		int end = PVD_VOL_ID_LEN;
		while (end > 0 && _pvdBuf[PVD_VOL_ID_OFFSET + end - 1] == ' ')
			end--;
		memcpy(volLabel, &_pvdBuf[PVD_VOL_ID_OFFSET], end);
		volLabel[end] = '\0';
	}

	return true;
}

int iso9660_listRoot(ISODirEntry *entries, int maxEntries) {
	// Re-read the PVD to locate the root directory
	if (!cdrom_readSector(PVD_LBA, _pvdBuf))
		return 0;
	if (_pvdBuf[0] != 1 || memcmp(&_pvdBuf[1], "CD001", 5) != 0)
		return 0;

	const uint8_t *rootRec = &_pvdBuf[PVD_ROOT_OFFSET];
	uint32_t dirLBA        = _rd32le(&rootRec[DR_LBA]);
	uint32_t dirSize       = _rd32le(&rootRec[DR_SIZE]);

	int found = 0;

	// Iterate over directory sectors. The root directory usually fits in a
	// single sector, but large directories may span more than one.
	for (uint32_t byteOffset = 0;
	     byteOffset < dirSize && found < maxEntries;
	     byteOffset += CDROM_SECTOR_SIZE)
	{
		uint32_t sectorLBA = dirLBA + (byteOffset / CDROM_SECTOR_SIZE);
		if (!cdrom_readSector(sectorLBA, _dirBuf))
			break;

		// How many bytes of this sector belong to the directory?
		uint32_t validBytes = dirSize - byteOffset;
		if (validBytes > CDROM_SECTOR_SIZE)
			validBytes = CDROM_SECTOR_SIZE;

		uint32_t pos = 0;
		while (pos < validBytes && found < maxEntries) {
			const uint8_t *rec    = &_dirBuf[pos];
			uint8_t        recLen = rec[DR_RECLEN];

			// A zero-length record marks the end of entries in this sector;
			// any remaining bytes are padding to the next sector boundary.
			if (recLen == 0)
				break;

			uint8_t nameLen = rec[DR_NAMELEN];

			// Skip the mandatory "." (0x00) and ".." (0x01) entries that ISO
			// 9660 requires at the start of every directory sector.
			bool isSpecial = (nameLen == 1)
			              && (rec[DR_NAME] == 0x00 || rec[DR_NAME] == 0x01);

			if (!isSpecial) {
				ISODirEntry *e = &entries[found++];
				e->lba   = _rd32le(&rec[DR_LBA]);
				e->size  = _rd32le(&rec[DR_SIZE]);
				e->isDir = (rec[DR_FLAGS] & DR_FLAG_DIR) != 0;
				_copyName(e->name, &rec[DR_NAME], nameLen, ISO_MAX_NAME);
			}

			pos += recLen;
		}
	}

	return found;
}
