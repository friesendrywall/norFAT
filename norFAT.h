#ifndef NORFAT_H
#define NORFAT_H
#include <stdint.h>
#include "norFATconfig.h"

#define NORFAT_FLAG_READ 1
#define NORFAT_FLAG_WRITE 2

#define NORFAT_ERR_FULL (-3)
#define NORFAT_ERR_CRC	(-2)
#define NORFAT_ERR_IO	(-1)
#define NORFAT_OK		0
#define NORFAT_EMPTY	(1)

#define NORFAT_MAGIC 0xBEEF

#define NORFAT_SOF_MSK		(0b0011000000000000)
#define NORFAT_EMPTY_MSK	(0xFFFF)

typedef union {
	struct {
		uint16_t next : 12;
		uint16_t active : 1;
		uint16_t sof : 1;
		uint16_t available : 1;
		uint16_t : 1;
	};
	uint16_t base;
}_sector;

typedef struct {
	uint8_t crc[8];//Ascii crc-32
} _commit;

typedef struct {
	_commit commit[NORFAT_CRC_COUNT];
	_sector sector[NORFAT_SECTORS];
} _FAT;/* must equal minimum sector size */

#if ((NORFAT_CRC_COUNT*8)+(NORFAT_SECTORS*2)) != NORFAT_SECTOR_SIZE
#error _FAT must be a multiple of NORFAT_SECTOR_SIZE
#endif

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
	uint32_t currentSector;
	uint32_t writePosInSector;
} norfat_FILE;

int32_t norfat_mount(norFAT_FS* fs);
int32_t norfat_format(norFAT_FS* fs);
norfat_FILE* norfat_fopen(norFAT_FS* fs, uint8_t* filename, uint32_t flags);
int32_t norfat_fclose(norFAT_FS* fs, norfat_FILE* file);
int32_t norfat_fwrite(norFAT_FS* fs, norfat_FILE* file, uint8_t* out, uint32_t len);
int32_t norfat_fread(norFAT_FS* fs, norfat_FILE* file, uint8_t* in, uint32_t len);
int32_t norfat_flength(norfat_FILE* file);
int32_t norfat_finfo(norFAT_FS* fs);

#endif
