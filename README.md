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
		uint32_t next : 16;//64MB limit
		uint32_t erases : 11;//Track up to 2048 erases
		uint32_t eraseFlag : 1;
		uint32_t active : 1;
		uint32_t sof : 1;
		uint32_t available : 1;
		uint32_t write : 1;
	};
	uint32_t base;
}_sector;

typedef struct {
	_commit commit[NORFAT_CRC_COUNT];
	uint16_t swapCount;
	uint16_t garbageCount;
	uint32_t future;
	_sector sector[NORFAT_SECTORS];
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

More to follow later..
