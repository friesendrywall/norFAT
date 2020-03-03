# norFAT
FAT style wear leveling NOR flash flat file system

## Goals

The goal of this project is a fail safe, wear leveling file system built on NOR flash. However, the file system is intended to be neither full featured nor thread safe at this point.  It is primarily intended to ease configuration management in higher level 32 bit embedded MCU's like the STM32 or PIC32 series.

## Details

Each FAT table is ordered as follows:
```
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
	_commit commit[NORFAT_CRC_COUNT];
	uint16_t swapCount;
	uint16_t garbageCount;
	uint32_t future;
	_sector sector[NORFAT_SECTORS];
} _FAT;/* must equal sector size */
```
There are two copies of this in NOR at any time, with each one saved atomically on commit.  _sector is used in such a way that only 1's are turned to 0's until garbage collection happens.  After each commit, the old crc is 0'd, and a new entry calculated based on its new offset, then entered using ascii to denote the new commit.  Once either we are out of commit entries, or garbage needs to be collected, then we refresh and copy new entries to the next two locations while deleting old.

More to follow later..
