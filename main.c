#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "norFAT.h"

#define BLOCK_SIZE (NORFAT_SECTORS * NORFAT_SECTOR_SIZE)
uint8_t block[BLOCK_SIZE];

uint32_t read_block_device(uint32_t address, uint8_t* data, uint32_t len) {
	if (address + len > BLOCK_SIZE) {
		assert(0);
	}
	//printf("Read 0x%X len 0x%X\r\n", address, len);
	memcpy(data, &block[address], len);
	return 0;
}
uint32_t erase_block_sector(uint32_t address)
{
	printf("Erase 0x%X\r\n", address);
	if (address + NORFAT_SECTOR_SIZE > BLOCK_SIZE) {
		assert(0);
	}
	memset(&block[address], 0xFF, NORFAT_SECTOR_SIZE);
	return 0;
}
uint32_t program_block_page(uint32_t address, uint8_t* data, uint32_t length)
{
	uint32_t i;
	if (address + length > BLOCK_SIZE) {
		assert(0);
	}
	for (i = 0; i < length; i++) {
		block[address + i] &= data[i];
	}
	return 0;
}


int main(int argv, char** argc) {

	norFAT_FS fs = {
		.addressStart = 0,
		.flashSize = BLOCK_SIZE
	};
	fs.erase_block_sector = erase_block_sector;
	fs.program_block_page = program_block_page;
	fs.read_block_device = read_block_device;
	memset(block, 0xFF, BLOCK_SIZE);
	fs.buff = malloc(NORFAT_SECTOR_SIZE);
	fs.fat = malloc(NORFAT_SECTOR_SIZE);
	int32_t res = norfat_mount(&fs);
	if (res == NORFAT_EMPTY) {
		res = norfat_format(&fs);
		block[100] = 0xFE;
		res = norfat_mount(&fs);
	}
	norfat_FILE* f = norfat_fopen(&fs, "Test.json", NORFAT_FLAG_READ);
	return 1;
}