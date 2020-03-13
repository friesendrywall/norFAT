# norFAT
FAT style wear leveling NOR flash flat file system with posix like calls.

## Goals

The goal of this project is a fail safe, wear leveling file system built on NOR flash. 
However, the file system is intended to be neither full featured nor thread safe at 
this point.  It is primarily intended to ease configuration management in 
higher level 32 bit embedded MCU's like the STM32 or PIC32 series.

## Integration
Add your block IO routines etc.
```
	norFAT_FS fs = {
		.addressStart = 0,
		.tableCount = 6,//3 fat table pairs carved from flash space
		.flashSectors = 1024,
		.sectorSize = 4096,//Erase block size
		.programSize = 256,//Program page size
		.erase_block_sector = erase_block_sector,
		.program_block_page = program_block_page,
		.read_block_device = read_block_device
	};
	int res = norfat_mount(&fs);
```

## Details

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
}_sector;

typedef struct {
	_commit commit[NORFAT_CRC_COUNT];
	uint32_t swapCount;
	uint32_t garbageCount;
	uint32_t future;
	_sector sector[];
} _FAT;
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

More to follow later..
