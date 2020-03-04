#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "norFAT.h"

static int32_t commitChanges(norFAT_FS* fs, uint32_t forceSwap);

#ifndef NORFAT_CRC
static uint32_t crc32_table[256];
void init_crc32(void);
static uint32_t crc32(void* buf, int len, uint32_t Seed);

#define CRC32_POLY 0x04c11db7     /* AUTODIN II, Ethernet, & FDDI 0x04C11DB7 */
#define NORFAT_CRC crc32

uint32_t crc32(void* buf, int len, uint32_t Seed) {
	unsigned char* p;
	uint32_t crc = Seed;
	if (!crc32_table[1]) /* if not already done, */
		init_crc32(); /* build table */
	for (p = buf; len > 0; ++p, --len)
		crc = (crc << 8) ^ crc32_table[(crc >> 24) ^ *p];
	return crc; /* transmit complement, per CRC-32 spec */
}

void init_crc32(void) {
	int i, j;
	uint32_t c;
	for (i = 0; i < 256; ++i) {
		for (c = i << 24, j = 8; j > 0; --j)
			c = c & 0x80000000 ? (c << 1) ^ CRC32_POLY : (c << 1);
		crc32_table[i] = c;
	}
}

#endif

#if 0
uint32_t crc32_table[256];


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
#endif

static uint32_t findCrcIndex(_FAT* fat) {
	uint32_t i;
	for (i = NORFAT_CRC_COUNT - 1; i > 0; i--) {
		if (fat->commit[i].crc[0] != 0xFF) {
			break;
		}
	}
	return i;
}

static int32_t scanTable(_FAT * fat) {
	uint32_t i;
	uint32_t wasRepaired = 0;
	for (i = NORFAT_TABLE_COUNT; i < NORFAT_SECTORS; i++) {
		if (fat->sector[i].write && !fat->sector[i].available) {
			NORFAT_INFO(("Sector %i recovered\r\n", i));
			fat->sector[i].base = NORFAT_EMPTY_MSK;
			wasRepaired = 1;
		}
	}
	return wasRepaired;
}

static int32_t copyTable(norFAT_FS* fs, uint32_t toIndex, uint32_t fromIndex) {
	toIndex %= NORFAT_TABLE_COUNT;
	fromIndex %= NORFAT_TABLE_COUNT;
	if (fs->read_block_device(
		fs->addressStart + (NORFAT_SECTOR_SIZE * fromIndex),
		(uint8_t*)fs->buff, NORFAT_SECTOR_SIZE)) {
		return NORFAT_ERR_IO;
	}
	if (fs->program_block_page(
		fs->addressStart + (NORFAT_SECTOR_SIZE * toIndex),
		(uint8_t*)fs->buff, NORFAT_SECTOR_SIZE)) {
		return NORFAT_ERR_IO;
	}
	return NORFAT_OK;
}

static int32_t eraseTable(norFAT_FS* fs, uint32_t tableIndex) {
	tableIndex %= NORFAT_TABLE_COUNT;
	if (fs->erase_block_sector(fs->addressStart +
		(tableIndex * NORFAT_SECTOR_SIZE))) {
		return NORFAT_ERR_IO;
	}
	return NORFAT_OK;
}

static uint32_t loadTable(norFAT_FS* fs, uint32_t tableIndex) {
	uint32_t crcRes, j;
	uint8_t cr[9];
	tableIndex %= NORFAT_TABLE_COUNT;
	if (fs->read_block_device(
		fs->addressStart + (NORFAT_SECTOR_SIZE * tableIndex),
		(uint8_t*)fs->fat, NORFAT_SECTOR_SIZE)) {
		return NORFAT_ERR_IO;
	}
	j = findCrcIndex(fs->fat);
	NORFAT_DEBUG(("Using CRC location %i\r\n", j));
	memcpy(cr, &fs->fat->commit[j], 8);
	cr[8] = 0;
	uint32_t crclen = sizeof(_FAT) - (sizeof(_commit) * (j + 1ULL));
	crcRes = NORFAT_CRC(&fs->fat->commit[j + 1], crclen, 0xFFFFFFFF);
	if (crcRes != strtoul(cr, NULL, 0x10)) {
		NORFAT_ERROR(("Table %i crc failure\r\n", tableIndex));
		return NORFAT_ERR_CRC;
	}
	NORFAT_INFO(("Table %i crc match\r\n", tableIndex));
	return NORFAT_OK;
}

static int32_t validateTable(norFAT_FS* fs, uint32_t tableIndex, uint32_t* crc) {
	uint32_t crcRes, j;
	int32_t res = NORFAT_TABLE_GOOD;
	uint8_t cr[9];
	tableIndex %= NORFAT_TABLE_COUNT;
	_FAT* fat = (_FAT*)fs->buff;
	if (fs->read_block_device(
		fs->addressStart + (NORFAT_SECTOR_SIZE * tableIndex),
		(uint8_t*)fat, NORFAT_SECTOR_SIZE)) {
		return NORFAT_ERR_IO;
	}
	//See if table is completely empty
	for (j = 0; j < NORFAT_SECTOR_SIZE; j++) {
		if (fs->buff[j] != 0xFF) {
			break;
		}
	}
	if (j == NORFAT_SECTOR_SIZE) {
		return NORFAT_TABLE_EMPTY;
	}
	j = findCrcIndex(fat);
	memcpy(cr, &fat->commit[j], 8);
	cr[8] = 0;
	uint32_t crclen = sizeof(_FAT) - (sizeof(_commit) * (j + 1ULL));
	crcRes = NORFAT_CRC(&fs->fat->commit[j + 1], crclen, 0xFFFFFFFF);
	if (crcRes != strtoul(cr, NULL, 0x10)) {
		NORFAT_ERROR(("Table %i crc failure\r\n", tableIndex));
		return NORFAT_TABLE_CRC;
	}
	if (crc) {
		*crc = crcRes;
	}
	NORFAT_INFO(("Table %i crc match\r\n", tableIndex));
	return res;
}

static int32_t garbageCollect(norFAT_FS* fs) {
	uint32_t i;
	uint32_t collected = 0;
	for (i = NORFAT_TABLE_COUNT; i < NORFAT_SECTORS; i++) {
		if (!fs->fat->sector[i].active) {
			fs->fat->sector[i].base = NORFAT_EMPTY_MSK;
			NORFAT_VERBOSE(("[%i], ", i));
			collected = 1;
		}
	}
	NORFAT_DEBUG(("Garbage collected\r\n"));
	if (collected) {
		fs->fat->garbageCount++;
		return commitChanges(fs, 1);
	}
	else {
		return NORFAT_ERR_FULL;
	}
}

static int32_t findEmptySector(norFAT_FS* fs) {

	uint32_t i;
	int32_t res;
	uint32_t sp = rand() % NORFAT_SECTORS;
	if (sp < NORFAT_TABLE_COUNT) {
		sp = NORFAT_SECTORS / 2;
	}
	for (i = sp; i < NORFAT_SECTORS; i++) {
		if (fs->fat->sector[i].available) {
			fs->fat->sector[i].available = 0;
			NORFAT_VERBOSE(("%i, ", i));
			return i;
		}
	}
	for (i = NORFAT_TABLE_COUNT; i < sp; i++) {
		if (fs->fat->sector[i].available) {
			fs->fat->sector[i].available = 0;
			NORFAT_VERBOSE(("%i, ", i));
			return i;
		}
	}

	res = garbageCollect(fs);
	if (res) {
		return res;
	}
	for (i = NORFAT_TABLE_COUNT; i < NORFAT_SECTORS; i++) {
		if (fs->fat->sector[i].available) {
			fs->fat->sector[i].available = 0;
			NORFAT_VERBOSE(("%i, ", i));
			return i;
		}
	}
	return NORFAT_ERR_FULL;
}

static norFAT_fileHeader * fileSearch(norFAT_FS* fs, uint8_t* name, uint32_t * sector) {
	uint32_t i;
	norFAT_fileHeader* f = NULL;
	for (i = NORFAT_TABLE_COUNT; i < NORFAT_SECTORS; i++) {
		if ((fs->fat->sector[i].base & NORFAT_SOF_MSK) == NORFAT_SOF_MATCH) {
			if (fs->read_block_device(fs->addressStart + (i * NORFAT_SECTOR_SIZE),
				fs->buff, sizeof(norFAT_fileHeader))) {
				return NULL;
			}
			//Somewhat wasteful, but we need to allow read function to call
			//cache routines on a safe buffer
			if (strcmp(fs->buff, name) == 0) {
				*sector = i;
				NORFAT_DEBUG(("File %s found at sector %i\r\n", name, i));
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

static int32_t commitChanges(norFAT_FS* fs, uint32_t forceSwap) {
	uint8_t cr[9];
	uint32_t crcRes;
	uint32_t i;
	uint32_t index = findCrcIndex(fs->fat);
	//Is current table set full?
	if (index == NORFAT_CRC_COUNT - 1 || forceSwap) {
		NORFAT_DEBUG(("Incrementing _FAT tables %i\r\n", fs->firstFAT));
		uint32_t swap1old = fs->firstFAT;
		uint32_t swap2old = (fs->firstFAT + 1) % NORFAT_TABLE_COUNT;
		uint32_t swap1new = (fs->firstFAT + 2) % NORFAT_TABLE_COUNT;
		uint32_t swap2new = (fs->firstFAT + 3) % NORFAT_TABLE_COUNT;
		fs->fat->swapCount++;
		// Refresh the FAT table and calculate crc
		memset(fs->fat->commit, 0xFF, sizeof(_commit) * NORFAT_CRC_COUNT);
		crcRes = NORFAT_CRC(&fs->fat->commit[1], sizeof(_FAT) - sizeof(_commit), 0xFFFFFFFF);
		snprintf(cr, 9, "%08X", crcRes);
		memcpy(&fs->fat->commit[0], cr, 8);

		//Erase #1 old block
		if (fs->erase_block_sector(fs->addressStart +
			(swap1old * NORFAT_SECTOR_SIZE))) {
			return NORFAT_ERR_IO;
		}

		//Program #1 new block
		if (fs->program_block_page(fs->addressStart +
			(swap1new * NORFAT_SECTOR_SIZE), (uint8_t*)fs->fat, NORFAT_SECTOR_SIZE)) {
			return NORFAT_ERR_IO;
		}

		//Erase #2 old block
		if (fs->erase_block_sector(fs->addressStart +
			(swap2old * NORFAT_SECTOR_SIZE))) {
			return NORFAT_ERR_IO;
		}

		//Program #2 new block
		if (fs->program_block_page(fs->addressStart +
			(swap2new * NORFAT_SECTOR_SIZE), (uint8_t*)fs->fat, NORFAT_SECTOR_SIZE)) {
			return NORFAT_ERR_IO;
		}

		fs->firstFAT += 2;
		fs->firstFAT %= NORFAT_TABLE_COUNT;
		NORFAT_DEBUG(("_FAT tables now at %i %ir\n", fs->firstFAT, ((fs->firstFAT + 1) % NORFAT_TABLE_COUNT)));
		return NORFAT_OK;
	}
	NORFAT_DEBUG(("Committing _FAT tables %i %i\r\n", fs->firstFAT, ((fs->firstFAT + 1) % NORFAT_TABLE_COUNT)));
	//Prep for write
	//CRC
	NORFAT_ASSERT(index < NORFAT_CRC_COUNT - 1);
	memset(&fs->fat->commit[index], 0, sizeof(_commit));
	uint32_t crclen = sizeof(_FAT) - (sizeof(_commit) * (index + 2ULL));
	crcRes = NORFAT_CRC(&fs->fat->commit[index + 2], crclen, 0xFFFFFFFF);
	snprintf(cr, 9, "%08X", crcRes);
	memcpy(&fs->fat->commit[index + 1], cr, 8);

	memset(fs->buff, 0xFF, NORFAT_SECTOR_SIZE);
	uint8_t* fat = (uint8_t*)fs->fat;
	for (i = 0; i < NORFAT_SECTOR_SIZE; i++) {
		fs->buff[i] &= fat[i];
	}
	if (fs->program_block_page(fs->addressStart +
		(fs->firstFAT * NORFAT_SECTOR_SIZE), (uint8_t*)fs->fat, NORFAT_SECTOR_SIZE)) {
		return NORFAT_ERR_IO;
	}
	if (fs->program_block_page(fs->addressStart +
		(((fs->firstFAT + 1) % NORFAT_TABLE_COUNT) * NORFAT_SECTOR_SIZE),
		(uint8_t*)fs->fat, NORFAT_SECTOR_SIZE)) {
		return NORFAT_ERR_IO;
	}
	NORFAT_DEBUG(("_FAT tables %i %i commited crc 0x%X\r\n",
		fs->firstFAT, ((fs->firstFAT + 1) % NORFAT_TABLE_COUNT), crcRes));
	return NORFAT_OK;
}

int32_t norfat_mount(norFAT_FS* fs) {
	int32_t i, j;
	uint32_t ui;
	int32_t empty = 1;
	uint16_t blank = 0xFFFF;
	uint16_t match = 0xBEEF;
	uint32_t sectorState[NORFAT_TABLE_COUNT];
	uint32_t sectorCRC[NORFAT_TABLE_COUNT];
	uint32_t scenario;
	NORFAT_ASSERT(fs->erase_block_sector);
	NORFAT_ASSERT(fs->program_block_page);
	NORFAT_ASSERT(fs->read_block_device);
	NORFAT_ASSERT(fs->fat);
	NORFAT_ASSERT(fs->buff);
	NORFAT_ASSERT(sizeof(_FAT) == NORFAT_SECTOR_SIZE);
	/* Scan tables for valid records */
	for (i = 0; i < NORFAT_TABLE_COUNT; i++) {
		sectorState[i] = validateTable(fs, i, &sectorCRC[i]);
		if (sectorState[i] == NORFAT_TABLE_GOOD) {
			empty = 0;
		}
		//Check old state
		//If Both sectors are good but crc's don't match, the odd number 
		//was programmed prior to the current operation, and is the older.
		if (i % 2 == 1) {
			if (sectorState[i] == NORFAT_TABLE_GOOD &&
				sectorState[i - 1] == NORFAT_TABLE_GOOD &&
				sectorCRC[i] != sectorCRC[i - 1]) {
				sectorState[i] = NORFAT_TABLE_OLD;
			}
		}
	}
	if (empty) {
		NORFAT_DEBUG(("Mounted volume is empty\r\n"));
		return NORFAT_EMPTY;
	}
#if 0
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
#endif
	/* There are 4 states, Good, old, empty, and bad 
	 * Tables are kept in mirrored copies in even pairs
	 * Like 0-1, 2-3, etc. We never attempt to do any
	 * repairs to a presently valid table.
	 */
	uint32_t res;
	uint32_t tablesValid = 0;

	for (ui = 0; ui < NORFAT_TABLE_COUNT; ui += 2) {
		//Build a scenario
		// |N|N|N|N|
		scenario = sectorState[ui] << 12;
		scenario += sectorState[(ui + 1) % NORFAT_TABLE_COUNT] << 8;
		scenario += sectorState[(ui + 2) % NORFAT_TABLE_COUNT] << 4;
		scenario += sectorState[(ui + 3) % NORFAT_TABLE_COUNT] << 0;
		printf("Now presenting scenario %04X\r\n", scenario);
		switch (scenario) {
		case 0x0022:/* |EMPTY|EMPTY|GOOD |GOOD | (ideal conditions) */
			res = loadTable(fs, ui);
			if (res) {
				return res;
			}
			fs->firstFAT = ui;
			NORFAT_DEBUG(("FAT tables %i %i loaded\r\n", ui, (ui + 1) % NORFAT_TABLE_COUNT));
			tablesValid = 1;
			break;
		case 0x2032:/* |EMPTY|GOOD | BAD | EMPTY|*/
			res = eraseTable(fs, ui + 2);
			if (res) {
				return res;
			}
			/* fallthrough to finish up */
		case 0x3022:/* | BAD |GOOD |EMPTY|EMPTY| (Re write table) */
			/* fallthrough same treatment*/
		case 0x2022:/* |EMPTY|GOOD |EMPTY|EMPTY| */
			res = loadTable(fs, ui + 1);
			if (res) {
				return res;
			}
			fs->firstFAT = ui;
			res = copyTable(fs, ui, ui + 1);
			if (res) {
				return res;
			}
			NORFAT_ASSERT(validateTable(fs, ui, NULL) == NORFAT_OK);//TODO: TEST and remove
			NORFAT_DEBUG(("FAT table %i rebuilt from %i\r\n", ui, (ui + 1) % NORFAT_TABLE_COUNT));
			tablesValid = 1;
			break;
		case 0x2002:/* |EMPTY|GOOD |GOOD |EMPTY| */
			//Delete the first good because its probably older
			/* fallthrough */
		case 0x2102:/* |EMPTY| OLD |GOOD |EMPTY| */
			/* fallthrough */
		case 0x2302:/* |EMPTY| BAD |GOOD |EMPTY| */
			res = eraseTable(fs, ui + 1);
			if (res) {
				return res;
			}
		case 0x2202:/* |EMPTY|EMPTY|GOOD |EMPTY|*/
			res = loadTable(fs, ui + 2);
			if (res) {
				return res;
			}
			fs->firstFAT = (ui + 2) % NORFAT_TABLE_COUNT;
			res = copyTable(fs, ui + 3, ui + 2);
			if (res) {
				return res;
			}
			NORFAT_ASSERT(validateTable(fs, ui + 3, NULL) == NORFAT_OK);//TODO: TEST and remove
			NORFAT_DEBUG(("FAT table %i updated from %i\r\n", (ui + 3) % NORFAT_TABLE_COUNT, (ui + 2) % NORFAT_TABLE_COUNT));
			tablesValid = 1;
			break;
		case 0x2203:/* |EMPTY|EMPTY|GOOD | BAD |*/
			res = eraseTable(fs, ui + 3);
			if (res) {
				return res;
			}
			res = loadTable(fs, ui + 2);
			if (res) {
				return res;
			}
			fs->firstFAT = (ui + 2) % NORFAT_TABLE_COUNT;
			res = copyTable(fs, ui + 3, ui + 2);
			if (res) {
				return res;
			}
			NORFAT_ASSERT(validateTable(fs, ui + 3, NULL) == NORFAT_OK);//TODO: TEST and remove
			NORFAT_DEBUG(("FAT table %i updated from %i\r\n", 
				(ui + 3) % NORFAT_TABLE_COUNT, (ui + 2) % NORFAT_TABLE_COUNT));
			tablesValid = 1;
			break;
			/* Inverse actions that mean we are on the wrong cog */
		case 0x2200:
		case 0x2220:
		case 0x2230:
		case 0x3220:
		case 0x0220:
		case 0x0221:
		case 0x0223:
		case 0x0222:
		case 0x0322:
			NORFAT_DEBUG(("Inverse, no action on %04x\r\n", scenario));
			break;
		default:
			NORFAT_DEBUG(("No suitable action for %04x\r\n", scenario));
			NORFAT_ASSERT(0);
			break;
		}

		if (tablesValid) {
			break;
		}

	}



	/* If there are any valid tables left, use them */
	if (!tablesValid) {

	}

	if (!tablesValid) {
		return NORFAT_ERR_CORRUPT;
	}
	/* scan for unclosed files */
	if (scanTable(fs->fat)) {
		commitChanges(fs, 0);
		NORFAT_DEBUG(("Tables repaired\r\n"));
	}
	fs->volumeMounted = 1;
	NORFAT_DEBUG(("Volume is mounted\r\n"));
	return 0;
}

int32_t norfat_format(norFAT_FS* fs) {
	uint32_t i, j;
	uint8_t cr[9];
	uint32_t crcRes;
	NORFAT_ASSERT(fs);
	for (i = 0; i < NORFAT_TABLE_COUNT; i++) {
		if (fs->read_block_device(
			fs->addressStart + (i * NORFAT_SECTOR_SIZE),
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
	fs->fat->garbageCount = 0;
	fs->fat->swapCount = 0;
	crcRes = NORFAT_CRC(&fs->fat->commit[1], sizeof(_FAT) - sizeof(_commit), 0xFFFFFFFF);
	snprintf(cr, 9, "%08X", crcRes);
	memcpy(&fs->fat->commit[0], cr, 8);
	if (fs->program_block_page(fs->addressStart, (uint8_t*)fs->fat,
		NORFAT_SECTOR_SIZE)) {
		return NORFAT_ERR_IO;
	}
	if (fs->program_block_page(NORFAT_SECTOR_SIZE + fs->addressStart,
		(uint8_t*)fs->fat, NORFAT_SECTOR_SIZE)) {
		return NORFAT_ERR_IO;
	}
	fs->firstFAT = 0;
	NORFAT_DEBUG(("Volume formatted\r\n"));
	return 0;
}

int32_t norfat_finfo(norFAT_FS* fs) {
	NORFAT_ASSERT(fs);
	NORFAT_ASSERT(fs->volumeMounted);
	uint32_t i;
	uint32_t bytesFree = 0;
	uint32_t bytesUsed = 0;
	uint32_t bytesUncollected = 0;
	uint32_t bytesAvailable = 0;
	uint32_t fileCount = 0;
	norFAT_fileHeader f;
	struct tm ts;
	time_t now;
	uint8_t buf[32];
	NORFAT_INFO(("\r\nVolume listing ......\r\n"));
	for (i = NORFAT_TABLE_COUNT; i < NORFAT_SECTORS; i++) {
		if ((fs->fat->sector[i].base & NORFAT_SOF_MSK) == NORFAT_SOF_MATCH) {
			if (fs->read_block_device(fs->addressStart + (i * NORFAT_SECTOR_SIZE),
				fs->buff, sizeof(norFAT_fileHeader))) {
				return NORFAT_ERR_IO;
			}
			
			memcpy(&f, fs->buff, sizeof(norFAT_fileHeader));
			now = (time_t)f.timeStamp;
			ts = *localtime(&now);
			strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ts);
			if (f.fileLen > 999) {
				NORFAT_INFO(("%s %i,%03i %s\r\n", buf, (int)f.fileLen / 1000, (int)f.fileLen % 1000, f.fileName));
			}
			else {
				NORFAT_INFO(("%s   %03i %s\r\n", buf, (int)f.fileLen % 1000, f.fileName));
			}			
			bytesUsed += f.fileLen;
			fileCount++;
		}
		else if (fs->fat->sector[i].available) {
			bytesAvailable += NORFAT_SECTOR_SIZE;
			bytesFree += NORFAT_SECTOR_SIZE;
		}
		else if (!fs->fat->sector[i].active) {
			bytesUncollected += NORFAT_SECTOR_SIZE;
			bytesFree += NORFAT_SECTOR_SIZE;
		}
		else {

		}
	}
	NORFAT_INFO(("     %i files %i bytes\r\n", fileCount, bytesUsed));
	NORFAT_INFO(("     Bytes free  0x%X\r\n", bytesFree));
	NORFAT_INFO(("     Bytes used  0x%X\r\n", bytesUncollected));
	NORFAT_INFO(("     Bytes ready 0x%X\r\n", bytesAvailable));
	NORFAT_INFO(("     Swaps %i\r\n", fs->fat->swapCount));
	NORFAT_INFO(("     Garbage %i\r\n", fs->fat->garbageCount));
	return 0;
}

norfat_FILE* norfat_fopen(norFAT_FS* fs, uint8_t* filename, uint32_t flags) {
	uint32_t sector;
	NORFAT_ASSERT(fs);
	NORFAT_ASSERT(fs->volumeMounted);
	NORFAT_ASSERT(flags);
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
			file->currentSector = sector;
			file->rwPosInSector = NORFAT_PAGE_SIZE;
			file->openFlags = flags;
			if (flags & NORFAT_FLAG_ZERO_COPY) {
				file->zeroCopy = 1;
			}
			NORFAT_DEBUG(("FILE %s opened for reading\r\n", filename));
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
		file->oldFileSector = NORFAT_FILE_NOT_FOUND;
		file->startSector = NORFAT_INVALID_SECTOR;
		file->openFlags = flags;
		file->currentSector = -1;
		if (f) {
			file->fh = f;
			file->oldFileSector = sector;//Mark for removal
			NORFAT_ASSERT(sector >= NORFAT_TABLE_COUNT);
			NORFAT_DEBUG(("Sector %i marked for removal\r\n", sector));
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
		NORFAT_DEBUG(("FILE %s opened for writing\r\n", filename));
		return file;
	}
	NORFAT_DEBUG(("FILE operation not specified\r\n"));
	return NULL;
}

int32_t norfat_fclose(norFAT_FS* fs, norfat_FILE* file) {
	//Write header to page
	NORFAT_ASSERT(fs);
	NORFAT_ASSERT(fs->volumeMounted);
	NORFAT_ASSERT(file);
	int32_t ret;
	uint32_t limit;
	uint32_t current;
	uint32_t next;

	memset(fs->buff, 0xFF, NORFAT_PAGE_SIZE);
	file->fh->fileLen = file->position;
	file->fh->timeStamp = time(NULL);
	memcpy(fs->buff, file->fh, sizeof(norFAT_fileHeader));
	if (file->error && file->openFlags & NORFAT_FLAG_WRITE) {
		//invalidate the last
		if (file->startSector != NORFAT_INVALID_SECTOR) {
			limit = NORFAT_SECTOR_SIZE;
			current = file->startSector;
			next = fs->fat->sector[current].next;
			while (1) {
				fs->fat->sector[current].base = NORFAT_IS_GARBAGE;
				if (next == NORFAT_EOF) {
					break;
				}
				current = next;
				next = fs->fat->sector[next].next;
				if (--limit < 1) {
					ret = NORFAT_ERR_CORRUPT;
					goto finalize;
				}
			}
		}
		ret = NORFAT_ERR_FULL;
		goto finalize;
	}
	if (file->openFlags & NORFAT_FLAG_WRITE && file->startSector != NORFAT_INVALID_SECTOR) {
		//Write the header
		if (fs->program_block_page(fs->addressStart +
			(file->startSector * NORFAT_SECTOR_SIZE), fs->buff, NORFAT_PAGE_SIZE)) {
			ret = NORFAT_ERR_IO;
			goto finalize;
		}
		//Commit to _FAT table
		fs->fat->sector[file->startSector].write = 0;//Set write inactive
	}

	//Delete old file
	if (file->openFlags & NORFAT_FLAG_WRITE
		&& file->oldFileSector != NORFAT_FILE_NOT_FOUND) {

		limit = NORFAT_SECTOR_SIZE;

		current = file->oldFileSector;
		next = fs->fat->sector[current].next;
		while (1) {
			NORFAT_DEBUG(("Deleting old %i %i\r\n", current, next));
			fs->fat->sector[current].base = NORFAT_IS_GARBAGE;
			if (next == NORFAT_EOF) {
				break;
			}
			current = next;
			next = fs->fat->sector[next].next;

			if (--limit < 1) {
				ret = NORFAT_ERR_CORRUPT;
				goto finalize;
			}
			if (next < NORFAT_TABLE_COUNT || (next >= NORFAT_SECTORS && next != NORFAT_EOF)) {
				NORFAT_ERROR(("Corrupt file system\r\n"));
				ret = NORFAT_ERR_CORRUPT;
				goto finalize;
			}
		}
	}
	ret = commitChanges(fs, 0);
	NORFAT_DEBUG(("FILE %s committed\r\n", file->fh->fileName));
finalize:
	NORFAT_DEBUG(("FILE %s closed\r\n", file->fh->fileName));
	NORFAT_FREE(file->fh);
	NORFAT_FREE(file);
	return ret;
}

int32_t norfat_fwrite(norFAT_FS* fs, norfat_FILE* file, uint8_t* out, uint32_t len) {
	int32_t nextSector;
	uint32_t writeable;
	uint32_t blockWriteLength;
	uint32_t blockAddress;
	uint32_t DataLengthToWrite;
	uint32_t offset;
	NORFAT_ASSERT(fs);
	NORFAT_ASSERT(fs->volumeMounted);
	NORFAT_ASSERT(file);
	NORFAT_ASSERT(file->openFlags & NORFAT_FLAG_WRITE);
	//NORFAT_DEBUG(("norfat_fwrite %s 0x%X\r\n", file->fh->fileName, len));
	if (file->currentSector == -1) {
		file->currentSector = findEmptySector(fs);
		if (file->currentSector == NORFAT_ERR_FULL) {
			file->error = 1;
			return NORFAT_ERR_FULL;
		}
		if (fs->erase_block_sector(fs->addressStart + (NORFAT_SECTOR_SIZE * file->currentSector))) {
			return NORFAT_ERR_IO;
		}
		NORFAT_DEBUG(("New file sector %i\r\n", file->currentSector));
		//New file
		file->startSector = file->currentSector;
		file->rwPosInSector = NORFAT_PAGE_SIZE;
		file->fh->crc = 0xFFFFFFFF;
		NORFAT_ASSERT(file->startSector >= NORFAT_TABLE_COUNT && file->startSector != NORFAT_INVALID_SECTOR);
	}
	//At this point we should have a writeable area
	while (len) {
		//Calculate available space to write in this sector
		writeable = NORFAT_SECTOR_SIZE - file->rwPosInSector;
		if (writeable == 0) {
			nextSector = findEmptySector(fs);
			if (nextSector == NORFAT_ERR_FULL) {
				file->error = 1;
				return NORFAT_ERR_FULL;
			}
			if (fs->erase_block_sector(fs->addressStart + (NORFAT_SECTOR_SIZE * nextSector))) {
				return NORFAT_ERR_IO;
			}
			fs->fat->sector[file->currentSector].next = nextSector;
			fs->fat->sector[nextSector].sof = 0;
			file->currentSector = nextSector;
			writeable = NORFAT_SECTOR_SIZE;
			file->rwPosInSector = 0;
		}
		//Calculate starting offset
		blockWriteLength = 0;
		offset = file->rwPosInSector % NORFAT_PAGE_SIZE;
		blockAddress = (file->currentSector * NORFAT_SECTOR_SIZE) + (file->rwPosInSector - offset);
		//buf = fs->buff;
		if (offset) {
			memset(fs->buff, 0xFF, offset);
			blockWriteLength += offset;
		}
		DataLengthToWrite = len > writeable ? writeable : len;
		memcpy(&fs->buff[blockWriteLength], out, DataLengthToWrite);
		blockWriteLength += DataLengthToWrite;
		if (blockWriteLength % NORFAT_PAGE_SIZE) {
			//Fill remaining page with 0xFF
			uint32_t fill = NORFAT_PAGE_SIZE - (blockWriteLength % NORFAT_PAGE_SIZE);
			memset(&fs->buff[blockWriteLength], 0xFF, fill);
			blockWriteLength += fill;
		}

#if 0

		uint32_t wlen = len > writeable ? writeable : len;
		//uint32_t rawlen = wlen;
		uint32_t rawAdr = (file->currentSector * NORFAT_SECTOR_SIZE) + (file->rwPosInSector - offset);
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
#endif
		NORFAT_ASSERT((blockAddress % NORFAT_SECTOR_SIZE) + blockWriteLength <= NORFAT_SECTOR_SIZE);
		//NORFAT_DEBUG(("Program to 0x%X[0x%X] 0x%X\r\n", fs->addressStart + blockAddress, offset, blockWriteLength));
		if (fs->program_block_page(fs->addressStart + blockAddress, fs->buff, blockWriteLength)) {
			return NORFAT_ERR_IO;
		}
		file->fh->crc = NORFAT_CRC(out, DataLengthToWrite, file->fh->crc);
		file->position += DataLengthToWrite;
		file->rwPosInSector += DataLengthToWrite;
		out += DataLengthToWrite;
		len -= DataLengthToWrite;
	}
	//NORFAT_DEBUG(("FILE %s written\r\n", file->fh->fileName));
	return len;
}

int32_t norfat_fread(norFAT_FS* fs, norfat_FILE* file, uint8_t* in, uint32_t len) {
	NORFAT_ASSERT(fs);
	NORFAT_ASSERT(fs->volumeMounted);
	NORFAT_ASSERT(file);
	NORFAT_ASSERT(file->openFlags & NORFAT_FLAG_READ);
	uint32_t next;
	uint32_t readable;
	uint32_t remaining;
	uint32_t rlen;
	uint32_t rawAdr;
	int32_t readCount = 0;
	while (len) {
		readable = NORFAT_SECTOR_SIZE - file->rwPosInSector;
		remaining = file->fh->fileLen - file->position;
		if (remaining == 0) {
			return readCount;
		}
		if (readable == 0) {
			next = fs->fat->sector[file->currentSector].next;
			if (next == NORFAT_EOF) {
				return readCount;
			}
			file->rwPosInSector = 0;
			file->currentSector = next;
			readable = NORFAT_SECTOR_SIZE;
		}

		rlen = len > readable ? readable : len;
		rlen = rlen > remaining ? remaining : rlen;
		//TODO: only allow reads up to real len
		//uint32_t rawlen = wlen;
		rawAdr = (file->currentSector * NORFAT_SECTOR_SIZE) + file->rwPosInSector;

		NORFAT_DEBUG(("Read from 0x%X 0x%X\r\n", fs->addressStart + rawAdr, rlen));
		if (file->zeroCopy) {
			// Requires user implemented cache free operation
			if (fs->read_block_device(fs->addressStart + rawAdr, in, rlen)) {
				return NORFAT_ERR_IO;
			}
		}
		else {
			if (fs->read_block_device(fs->addressStart + rawAdr, fs->buff, rlen)) {
				return NORFAT_ERR_IO;
			}
			memcpy(in, fs->buff, rlen);
		}

		//crc32(out, wlen, &file->fh->crc);
		file->position += rlen;
		file->rwPosInSector += rlen;
		in += rlen;
		len -= rlen;
		readCount += rlen;

	}
	NORFAT_DEBUG(("FILE %s read\r\n", file->fh->fileName));
	return readCount;
}

int32_t norfat_flength(norfat_FILE* f) {
	NORFAT_ASSERT(f);
	if (f == NULL) {
		return -1;
	}
	return f->fh->fileLen;
}