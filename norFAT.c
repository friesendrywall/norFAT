#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "norFAT.h"

uint32_t crc32_for_byte(uint32_t r) {
	for (int j = 0; j < 8; ++j)
		r = (r & 1 ? 0 : (uint32_t)0xEDB88320L) ^ r >> 1;
	return r ^ (uint32_t)0xFF000000L;
}

void crc32(const void* data, size_t n_bytes, uint32_t* crc) {
	static uint32_t table[0x100];
	if (!*table)
		for (size_t i = 0; i < 0x100; ++i)
			table[i] = crc32_for_byte(i);
	for (size_t i = 0; i < n_bytes; ++i)
		* crc = table[(uint8_t)* crc ^ ((uint8_t*)data)[i]] ^ *crc >> 8;
}

static uint32_t findCrcIndex(norFAT_FS* fs) {
	uint32_t i;
	for (i = 0; i < NORFAT_CRC_COUNT; i++) {
		if (fs->fat->commit[i].crc[0] != 0xFF) {
			break;
		}
	}
	return i;
}

static void fillCommitCRC(uint32_t crc, uint8_t* ascii) {

}

static uint32_t validateTable(norFAT_FS* fs, uint32_t tableIndex) {
	uint32_t crcRes, j;
	uint8_t cr[9];
	if (fs->read_block_device(
		fs->addressStart + (NORFAT_SECTOR_SIZE * tableIndex),
		(uint8_t*)fs->fat, NORFAT_SECTOR_SIZE)) {
		return NORFAT_ERR_IO;
	}
	crcRes = 0xFFFFFFFF;
	j = findCrcIndex(fs);
	memcpy(cr, fs->fat->commit, 8);
	cr[8] = 0;

	crc32(&fs->fat->commit[j + 1], sizeof(_FAT) - (sizeof(_commit) * (j + 1)), &crcRes);
	if (crcRes != strtoul(cr, NULL, 0x10)) {
		NORFAT_DEBUG(("Table %i crc failure\r\n", tableIndex));
		return NORFAT_ERR_CRC;
	}
	NORFAT_DEBUG(("Table %i crc match\r\n", tableIndex));
	return NORFAT_OK;
}

static void garbageCollect(norFAT_FS* fs) {
	uint32_t i;
	for (i = NORFAT_TABLE_COUNT; i < NORFAT_SECTORS; i++) {
		if (!fs->fat->sector[i].active) {
			fs->fat->sector[i].base = NORFAT_EMPTY_MSK;
		}
	}
}

static int32_t findEmptySector(norFAT_FS* fs) {
	uint32_t i;
	for (i = NORFAT_TABLE_COUNT; i < NORFAT_SECTORS; i++) {
		if (fs->fat->sector[i].base & NORFAT_EMPTY_MSK) {
			fs->fat->sector[i].available = 0;
			return i;
		}
	}
	garbageCollect(fs);
	for (i = NORFAT_TABLE_COUNT; i < NORFAT_SECTORS; i++) {
		if (fs->fat->sector[i].base & NORFAT_EMPTY_MSK) {
			fs->fat->sector[i].available = 0;
			return i;
		}
	}
	return NORFAT_ERR_FULL;
}

static norFAT_fileHeader * fileSearch(norFAT_FS* fs, uint8_t* name, uint32_t * sector) {
	uint32_t i;
	norFAT_fileHeader* f = NULL;
	for (i = NORFAT_TABLE_COUNT; i < NORFAT_SECTORS; i++) {
		if (fs->fat->sector[i].base & NORFAT_SOF_MSK) {
			if (fs->read_block_device(i * NORFAT_SECTOR_SIZE,
				fs->buff, sizeof(norFAT_fileHeader))) {
				return NULL;
			}
			//Somewhat wasteful, but we need to allow read function to call
			//cache routines on a safe buffer
			if (strcmp(fs->buff, name) == 0) {
				*sector = i;
				f = NORFAT_MALLOC(sizeof(norFAT_fileHeader));
				if (f) {
					memcpy(f, fs->buff, sizeof(norFAT_fileHeader));
				}
				break;
			}
		}
	}
	return f;
}

int32_t norfat_mount(norFAT_FS* fs) {
	int32_t i, j;
	int32_t empty = 1;
	uint16_t blank = 0xFFFF;
	uint16_t match = 0xBEEF;
	uint32_t sectorState[NORFAT_TABLE_COUNT];
	NORFAT_ASSERT(fs->erase_block_sector);
	NORFAT_ASSERT(fs->program_block_page);
	NORFAT_ASSERT(fs->read_block_device);
	NORFAT_ASSERT(fs->fat);
	NORFAT_ASSERT(fs->buff);
	/* Search for last valid records, top first*/
	for (i = 0; i < NORFAT_TABLE_COUNT; i++) {
		if (fs->read_block_device(
			fs->addressStart + (NORFAT_SECTOR_SIZE * i),
			fs->buff, 4)) {
			return NORFAT_ERR_IO;
		}
		if (memcmp(&blank, fs->buff, 2) == 0) {
			sectorState[i] = 0;
		}
		else {
			sectorState[i] = 1;
			empty = 0;
		}
	}
	if (empty) {
		NORFAT_DEBUG(("Mounted volume is empty\r\n"));
		return NORFAT_EMPTY;
	}
	/* Search through records until we find the first one following an empty location*/
	j = 0;
	for (i = 0; i < NORFAT_TABLE_COUNT * 2; i++) {
		if (j && sectorState[i % NORFAT_TABLE_COUNT] == 1) {
			if (j && sectorState[i % NORFAT_TABLE_COUNT] == 1) {
				fs->firstFAT = i % NORFAT_TABLE_COUNT;
				NORFAT_DEBUG(("Validating table %i\r\n", fs->firstFAT));
				break;
			}
		}
		if (!sectorState[i % NORFAT_TABLE_COUNT] && !j) {
			j = 1;
		}
	}
	/* Find the first valid record */
	uint32_t res = validateTable(fs, fs->firstFAT);
	if (res == NORFAT_ERR_CRC) {
		res = validateTable(fs, (fs->firstFAT + 1) % NORFAT_TABLE_COUNT);
		if (res) {
			return res;
		}
	}

	return 0;
}

int32_t norfat_format(norFAT_FS* fs) {
	uint32_t i, j;
	uint8_t cr[9];
	uint32_t crcRes = 0xFFFFFFFF;
	for (i = 0; i < fs->flashSize; i += NORFAT_SECTOR_SIZE) {
		if (fs->read_block_device(
			fs->addressStart + i,
			fs->buff, NORFAT_SECTOR_SIZE)) {
			return NORFAT_ERR_IO;
		}
		for (j = 0; j < NORFAT_SECTOR_SIZE; j++) {
			if (fs->buff[j] != 0xFF) {
				break;
			}
		}
		if (j != NORFAT_SECTOR_SIZE) {
			if (fs->erase_block_sector(fs->addressStart + i)) {
				return NORFAT_ERR_IO;
			}
		}
	}
	/* Build up an initial _FAT on the first two sectors */
	memset(fs->fat, 0xFF, NORFAT_SECTOR_SIZE);
		//crc32(fs->fat, sizeof(_FAT) - (sizeof(_commit) * (j - 1)), &crcRes);
	crc32(&fs->buff[8], sizeof(_FAT) - sizeof(_commit), &crcRes);
	snprintf(cr, 9, "%08X", crcRes);
	memcpy(&fs->fat->commit[0], cr, 8);
	if (fs->program_block_page(fs->addressStart, (uint8_t*)fs->fat,
		NORFAT_SECTOR_SIZE / NORFAT_PAGE_SIZE)) {
		return NORFAT_ERR_IO;
	}
	if (fs->program_block_page(NORFAT_SECTOR_SIZE + fs->addressStart,
		(uint8_t*)fs->fat, NORFAT_SECTOR_SIZE / NORFAT_PAGE_SIZE)) {
		return NORFAT_ERR_IO;
	}

	return 0;
}

int32_t norfat_finfo(norFAT_FS* fs) {

	return 0;
}

norfat_FILE* norfat_fopen(norFAT_FS* fs, uint8_t* filename, uint32_t flags) {
	uint32_t sector;
	norFAT_fileHeader* f = fileSearch(fs, filename, &sector);
	norfat_FILE* file;
	if (flags & NORFAT_FLAG_READ) {
		if (f) {
			file = NORFAT_MALLOC(sizeof(norfat_FILE));
			if (!file) {
				return NULL;
			}
			memset(file, 0, sizeof(norfat_FILE));
			file->fh = f;
			file->startSector = sector;
			return file;
		}
		else {
			return NULL;//File not found
		}
	}
	else if (flags & NORFAT_FLAG_WRITE) {
		file = NORFAT_MALLOC(sizeof(norfat_FILE));
		if (!file) {
			if (f) {
				NORFAT_FREE(f);
			}
			return NULL;
		}
		memset(file, 0, sizeof(norfat_FILE));
		file->oldFileSector = -1;
		file->currentSector = -1;
		if (f) {
			file->fh = f;
			file->oldFileSector = sector;//Mark for removal
		}
		else {
			file->fh = NORFAT_MALLOC(sizeof(norFAT_fileHeader));
			if (!file->fh) {
				NORFAT_FREE(file);
				return NULL;
			}
			memset(file->fh, 0, sizeof(norFAT_fileHeader));
			strncpy(file->fh->fileName, filename, 32);
		}
		return file;
	}

	return NULL;
}

int32_t norfat_fclose(norFAT_FS* fs, norfat_FILE* file) {
	//Write header to page
	memset(fs->buff, 0xFF, NORFAT_PAGE_SIZE);
	memcpy(fs->buff, file->fh, sizeof(norFAT_fileHeader));
	if (fs->program_block_page(
		file->startSector * NORFAT_SECTOR_SIZE, fs->buff, NORFAT_PAGE_SIZE)) {
		return NORFAT_ERR_IO;
	}
	//Commit to _FAT table

	return 0;
}

int32_t norfat_fwrite(norFAT_FS* fs, norfat_FILE* file, uint8_t* out, uint32_t len) {
	int32_t nextSector;
	int32_t res;
	if (file->currentSector == -1) {
		file->currentSector = findEmptySector(fs);
		if (file->currentSector == NORFAT_ERR_FULL) {
			return NORFAT_ERR_FULL;
		}
		//New file
		file->writePosInSector = NORFAT_PAGE_SIZE;
		file->fh->crc = 0xFFFFFFFF;
	}
	//At this point we should have a writeable area
	while (len) {
		uint32_t writeable = NORFAT_SECTOR_SIZE - file->writePosInSector;
		if (writeable == 0) {
			nextSector = findEmptySector(fs);
			if (nextSector == NORFAT_ERR_FULL) {
				return NORFAT_ERR_FULL;
			}
			fs->fat->sector[file->currentSector].next = nextSector;
			fs->fat->sector[nextSector].sof = 0;
			file->currentSector = nextSector;
			writeable = NORFAT_SECTOR_SIZE;
			file->writePosInSector = 0;
		}

		uint32_t offset = file->writePosInSector % NORFAT_PAGE_SIZE;
		uint32_t wlen = len > writeable ? writeable : len;
		uint32_t rawlen = wlen;
		if (rawlen < NORFAT_PAGE_SIZE && rawlen % NORFAT_PAGE_SIZE) {
			//TODO: validate this here
			rawlen += (NORFAT_PAGE_SIZE - (rawlen % NORFAT_PAGE_SIZE));
			memset(&fs->buff[offset + wlen], 0xFF, (NORFAT_PAGE_SIZE - (rawlen % NORFAT_PAGE_SIZE)));
		}
		rawlen += offset;
		//Is start address aligned?  If no, shift and insert 0xFF's
		if (offset) {
			memset(fs->buff, 0xFF, offset);
		}
		memcpy(&fs->buff[offset], out, wlen);
		if (fs->program_block_page(
			file->currentSector * NORFAT_SECTOR_SIZE, fs->buff, rawlen)) {
			return NORFAT_ERR_IO;
		}
		crc32(out, wlen, &file->fh->crc);
		file->position += wlen;
		out += wlen;
		len -= wlen;
		
	}

	return NORFAT_OK;
}

int32_t norfat_fread(norFAT_FS* fs, norfat_FILE* file, uint8_t* in, uint32_t len) {
	return 0;
}

int32_t norfat_flength(norfat_FILE* f) {
	if (f == NULL) {
		return -1;
	}
	return f->fh->fileLen;
}