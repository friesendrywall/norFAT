#ifndef NORFAT_H
#define NORFAT_H
#include "norFATconfig.h"
#include <stdint.h>
#include <stdlib.h>

#define NORFAT_VERSION              "1.04"
#define NORFAT_ERR_EMPTY            (-20)
#define NORFAT_ERR_FILECRC          (-12)
#define NORFAT_ERR_CORRUPT          (-10)
#define NORFAT_ERR_MALLOC           (-8)
#define NORFAT_ERR_FILE_NOT_FOUND   (-7)
#define NORFAT_ERR_UNSUPPORTED      (-6)
#define NORFAT_ERR_NULL             (-5)
#define NORFAT_ERR_NOFS             (-4)
#define NORFAT_ERR_FULL             (-3)
#define NORFAT_ERR_CRC              (-2)
#define NORFAT_ERR_IO               (-1)
#define NORFAT_OK                   (0)

#ifndef NORFAT_MAX_TABLES
#define NORFAT_MAX_TABLES 16
#endif

#ifndef NORFAT_CRC_COUNT
#error NORFAT_CRC_COUNT must be defined in norFATconfig.h
#endif

typedef struct {
  char fileName[NORFAT_MAX_FILENAME];
  uint32_t fileLen;
  uint32_t timeStamp;
  uint32_t crc;
} norFAT_fileHeader;

typedef struct {
  uint32_t startSector;
  uint32_t position;
  norFAT_fileHeader fh;
  int32_t oldFileSector;
  int32_t currentSector;
  uint32_t rwPosInSector;
  uint32_t openFlags;
  int lastError;
  uint32_t zeroCopy : 1;
  uint32_t error : 1;
  uint32_t crcValidate;
} norfat_FILE;

typedef union {
  struct {
    uint32_t next : 28;
    uint32_t active : 1;
    uint32_t sof : 1;
    uint32_t available : 1;
    uint32_t write : 1;
  };
  uint32_t base;
} _sector;

typedef struct {
  uint8_t crc[8]; // Ascii crc-32
} _commit;

typedef struct {
  _commit commit[NORFAT_CRC_COUNT];
  /* Number of times _Fat tables have been swapped. */
  uint32_t swapCount;
  /* Number of times garbage collected, never more than swapCount */
  uint32_t garbageCount;
  union {
    struct {
      uint32_t future : 32;
    };
    uint32_t flags;
  };
  _sector sector[];
} _FAT; /* must equal sector size */

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
  uint8_t *buff; // User allocated to sectorSize
  /* fat is used to store the working copy of the table */
  _FAT *fat; // User allocated to sectorSize
  uint32_t (*read_block_device)(uint32_t address, uint8_t *data, uint32_t len);
  uint32_t (*erase_block_sector)(uint32_t address);
  uint32_t (*program_block_page)(uint32_t address, uint8_t *data,
                                 uint32_t length);
  // Non userspace stuff
  uint32_t firstFAT;
  uint32_t volumeMounted;
  // Working buffer for fopen, norfat_exists
  // norFAT_fileHeader fh;
  int lastError;

} norFAT_FS;

int norfat_mount(norFAT_FS *fs);
int norfat_format(norFAT_FS *fs);
int norfat_fopen(norFAT_FS *fs, const char *filename, const char *mode,
                 norfat_FILE *file);
int norfat_fclose(norFAT_FS *fs, norfat_FILE *stream);
size_t norfat_fwrite(norFAT_FS *fs, const void *ptr, size_t size, size_t count,
                     norfat_FILE *stream);
size_t norfat_fread(norFAT_FS *fs, void *ptr, size_t size, size_t count,
                    norfat_FILE *stream);
int norfat_remove(norFAT_FS *fs, const char *filename);
size_t norfat_flength(norfat_FILE *file);
int norfat_fsinfo(norFAT_FS *fs, char *buff, int32_t maxLen);

#ifdef NORFAT_COVERAGE_TEST
int norfat_fsMetaData(char *buff, int32_t maxLen);
#endif

  /* norfat_exists()
 * Returns:
 * < 0 error
 * 0 = File not found
 * > 0 File length
 */
int norfat_exists(norFAT_FS *fs, const char *filename);
int norfat_ferror(norfat_FILE *file);
int norfat_errno(norFAT_FS *fs);

#endif
