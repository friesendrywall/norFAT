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

typedef union {
	struct {
		uint16_t next : 12;
		uint16_t active : 1;
		uint16_t sof : 1;
		uint16_t available : 1;
		uint16_t write : 1;
	};
	uint16_t base;
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
	_sector sector[NORFAT_SECTORS];
} _FAT;/* must equal minimum sector size */

typedef struct {
	const uint32_t addressStart;
	const uint32_t flashSize;
	uint8_t* buff;//User allocated to NORFAT_SECTOR_SIZE
	_FAT* fat;//User allocated cache area
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
int norfat_finfo(norFAT_FS* fs);
int norfat_ferror(norFAT_FS* fs, norfat_FILE* file);
int norfat_errno(norFAT_FS* fs);

#endif
