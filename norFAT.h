#ifndef NORFAT_H
#define NORFAT_H
#include <stdint.h>
#include "norFATconfig.h"

#define NORFAT_VERSION "1.00"
#define NORFAT_ERR_EMPTY			(-20)
#define NORFAT_ERR_CORRUPT			(-10)
#define NORFAT_ERR_MALLOC			(-8)
#define NORFAT_ERR_FILE_NOT_FOUND	(-7)
#define NORFAT_ERR_UNSUPPORTED		(-6)
#define NORFAT_ERR_NULL (-5)
#define NORFAT_ERR_NOFS (-4)
#define NORFAT_ERR_FULL (-3)
#define NORFAT_ERR_CRC	(-2)
#define NORFAT_ERR_IO	(-1)
#define NORFAT_OK		0

#ifndef NORFAT_MAX_TABLES
#define NORFAT_MAX_TABLES 16
#endif

typedef union {
	struct {
		uint32_t next : 16;//64MB limit
		uint32_t erases : 12;//Track up to 4096 erases
		uint32_t active : 1;
		uint32_t sof : 1;
		uint32_t available : 1;
		uint32_t write : 1;
	};
	uint32_t base;
}_sector;

typedef struct {
	uint8_t crc[8];//Ascii crc-32
} _commit;

typedef struct {
	_commit commit[NORFAT_CRC_COUNT];
	uint16_t swapCount;
	uint16_t garbageCount;
	union {
		struct {
			uint32_t future : 32;
		};
		uint32_t flags;
	};
	_sector sector[];
} _FAT;/* must equal sector size */

typedef struct {
	/* Physical address of media */
	const uint32_t addressStart;
	/* Number of sectors used in media */
	const uint32_t flashSectors;
	/* Minimum erase size */
	const uint32_t sectorSize;
	/* Minimum program size */
	const uint32_t programSize;
	/* FAT tables carved out of flash sectors */
	const uint32_t tableCount;
	/* Number of sectors per table */
	const uint32_t tableSectors;
	/* buff is used for all IO, so if driver uses DMA, allocate accordingly */
	uint8_t* buff;//User allocated to sectorSize
	/* fat is used to store the working copy of the table */
	_FAT* fat;//User allocated to sectorSize
	uint32_t(*read_block_device)(uint32_t address, uint8_t* data, uint32_t len);
	uint32_t(*erase_block_sector)(uint32_t address);
	uint32_t(*program_block_page)(uint32_t address, uint8_t* data, uint32_t length);
	//Non userspace stuff
	uint32_t firstFAT;
	uint32_t volumeMounted;
	int lastError;
} norFAT_FS;

typedef struct {
	uint8_t fileName[NORFAT_MAX_FILENAME];
	uint32_t fileLen;
	uint32_t timeStamp;
	uint32_t crc;
}norFAT_fileHeader;

typedef struct {
	uint32_t startSector;
	uint32_t position;
	norFAT_fileHeader * fh;
	uint32_t oldFileSector;
	int32_t currentSector;
	uint32_t rwPosInSector;
	uint32_t openFlags;
	int lastError;
	uint32_t zeroCopy : 1;
	uint32_t error : 1;
} norfat_FILE;

int norfat_mount(norFAT_FS* fs);
int norfat_format(norFAT_FS* fs);
norfat_FILE* norfat_fopen(norFAT_FS* fs, const char* filename, const char* mode);
int norfat_fclose(norFAT_FS* fs, norfat_FILE* stream);
size_t norfat_fwrite(norFAT_FS* fs, const void* ptr, size_t size, size_t count, norfat_FILE* stream);
size_t norfat_fread(norFAT_FS* fs, void* ptr, size_t size, size_t count, norfat_FILE* stream);
int norfat_remove(norFAT_FS* fs, const char* filename);
size_t norfat_flength(norfat_FILE* file);
int norfat_fsinfo(norFAT_FS* fs);

/* norfat_exists()
 * Returns:
 * < 0 error 
 * 0 = File not found
 * > 0 File length
 */
int norfat_exists(norFAT_FS* fs, const char* filename);
int norfat_ferror(norFAT_FS* fs, norfat_FILE* file);
int norfat_errno(norFAT_FS* fs);

#endif
