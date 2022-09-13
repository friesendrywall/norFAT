# norFAT
FAT style wear leveling NOR flash flat file system with posix like calls.

## Goals

The goal of this project is a fail-safe, wear leveling file system built on NOR flash. 
However, the file system is not full featured nor thread safe.  It is primarily intended to ease configuration management in 
higher level 32 bit embedded MCU's like the STM32 or PIC32 series. On file overwrites, the
implementation attempts to preserve the old file until the newer file is closed, then
atomically swap and delete.

## Wear leveling
Because the primary goal is reliablity rather than perfect wear leveling, files are never moved.
New files or rewritten files will always choose a randomly selected free sector. 
The tables are cycled through, so table size and crc count should roughly match the 
application.

## Integration
Add your block IO routines etc.
```
/* Setup for 8MB NOR flash */
#define NORFAT_SECTORS 2048
#define NORFAT_SECTOR_SIZE 4096
#define NORFAT_TABLE_SECTORS 3 // Table size
#define NORFAT_TABLE_COUNT 6   // 3 fat table pairs carved from flash space

  /* Statically allocated buffers */
  uint8_t workingBuffer[NORFAT_SECTOR_SIZE * NORFAT_TABLE_SECTORS];
  uint8_t fatTableBuffer[NORFAT_SECTOR_SIZE * NORFAT_TABLE_SECTORS];
  norFAT_FS fs = {
    .addressStart = 0,
    .tableCount = NORFAT_TABLE_COUNT,
    .flashSectors = 1024,
    .sectorSize = NORFAT_SECTOR_SIZE, // Erase block size
    .programSize = 256,               // Program page size
    .erase_block_sector = erase_block_sector,
    .program_block_page = program_block_page,
    .read_block_device = read_block_device,
    .buff = workingBuffer,
    .fat = fatTableBuffer
  };
  int res = norfat_mount(&fs);
```

## Details

Dynamic allocation is not used after from V1.04 on.

Each FAT table is ordered as follows:
```
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
  /* Undefined length, fills remaining table length */
  _sector sector[]; 
} _FAT; /* must equal a multiple of sector size */
```
There are two copies of this in NOR at any time, with each one 
saved atomically on commit.  _sector is used in such a way that 
only 1's are turned to 0's until garbage collection happens.  
After each commit, the old crc is 0'd, and a new entry calculated 
based on its new offset, then entered using ascii to denote the 
new commit.  Once either we are out of commit entries, or garbage 
needs to be collected, then we refresh and copy new entries to the 
next two locations while deleting old.

NORFAT_CRC_COUNT should be chosen to match typical use.  For example, 
a choice of 256 will cost 2048 sector bytes, but give up to 256 file
commits that happen on fclose.  So this could be matched with
tableSectors to roughly match rotation on the file sectors.  swapCount
tracks the number of times the fat sector is swapped to the next in line.
garbageCount tracks the number of collections that have happened.  Essentially
swapCount is the number of erases /  tableCount that have happened to the 
tables.

Tables are managed in groups of two, grouped as 2x2 when mounting, thus
cycling through the possible sequences and managing each one as needed.
All possible options should be listed here, although the inverse options
may not be, for example 0x3022 is essentially the same as 0x2230 for a
4 table system.

```
Commit sequences
[00] 0x0022 |GOOD |GOOD |EMPTY|EMPTY|
[01] 0x3022 | BAD |GOOD |EMPTY|EMPTY|  Partial erase #1
[02] 0x2022 |EMPTY|GOOD |EMPTY|EMPTY|  Full erase #1
[03] 0x3022 | BAD |GOOD |EMPTY|EMPTY|  Partial write #1
[04] 0x0122 |GOOD | OLD |EMPTY|EMPTY|  Halfway through
[05] 0x0322 |GOOD | BAD |EMPTY|EMPTY|  Partial erase #2
[06] 0x0222 |GOOD |EMPTY|EMPTY|EMPTY|  Full erase #2
[07] 0x0322 |GOOD | BAD |EMPTY|EMPTY|  Partial write #2
[00] 0x0022 |GOOD |GOOD |EMPTY|EMPTY|  Completed  

Table Swapping sequences
[00] 0x0022 |GOOD |GOOD |EMPTY|EMPTY|
[01] 0x3022 | BAD |GOOD |EMPTY|EMPTY|  Partial Erase #1 old block
[02] 0x2022 |EMPTY|GOOD |EMPTY|EMPTY|  Full Erase #1 old block
[03] 0x2032 |EMPTY|GOOD | BAD |EMPTY|  Partial program #1 new block
[04] 0x2002 |EMPTY|GOOD |GOOD |EMPTY|  Full program #1 new block
[05] 0x2302 |EMPTY| BAD |GOOD |EMPTY|  Partial Erase #2 old block
[06] 0x2202 |EMPTY|EMPTY|GOOD |EMPTY|  Full Erase #2 old block
[07] 0x2203 |EMPTY|EMPTY|GOOD | BAD |  Partial program #1 new block
[00] 0x2200 |EMPTY|EMPTY|GOOD |GOOD |  Full program #1 new block
```
## Options
```
/* CRC table count, this is the number of commits before garbage
 * is collected and tables are swapped to next pair
 */
NORFAT_CRC_COUNT 255

/* max file name length.  Changing this length will break the file system */
NORFAT_MAX_FILENAME 64

/* define for file CRC checking on fread eof */
NORFAT_FILE_CHECK

/* define for sequence coverage test */
NORFAT_COVERAGE_TEST

/* Print details, sprintf whatever */
NORFAT_DEBUG(x) // printf x
NORFAT_ERROR(x) // printf x
NORFAT_INFO_PRINT(x) printf x 

/* Random number generator, used for wear leveling */
NORFAT_RAND

/* uint32_t crc32(void *buf, int len, uint32_t Seed) */
NORFAT_CRC crc32 /* Or user supplied crc routine */

```

