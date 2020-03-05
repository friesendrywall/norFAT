#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "norFAT.h"

#define BLOCK_SIZE (NORFAT_SECTORS * NORFAT_SECTOR_SIZE)
uint8_t block[BLOCK_SIZE];

uint32_t EraseCounts[NORFAT_SECTORS];

uint32_t takeDownPeriod = 0;
uint32_t takeDownTest = 0;

uint32_t read_block_device(uint32_t address, uint8_t* data, uint32_t len) {
	//printf("BLOCK: Reading 0x%X 0x%X\r\n", address, len);
	if (takeDownTest) {

		if (takeDownPeriod != 0) {
			takeDownPeriod--;
		}
		else {
			printf("Power failed at read 0x%X .......\r\n", address);
			return 1;
		}
	}
	if (address + len > BLOCK_SIZE) {
		assert(0);
	}
	//printf("Read 0x%X len 0x%X\r\n", address, len);
	memcpy(data, &block[address], len);
	return 0;
}

uint32_t erase_block_sector(uint32_t address)
{
	
	if (takeDownTest) {

		if (takeDownPeriod != 0) {
			takeDownPeriod--;
		}
		else {
			printf("Power failed at erase 0x%X .......\r\n", address);
			return 1;
		}
	}
	EraseCounts[address / NORFAT_SECTOR_SIZE] ++;
	if (address + NORFAT_SECTOR_SIZE > BLOCK_SIZE) {
		assert(0);
	}
	memset(&block[address], 0xFF, NORFAT_SECTOR_SIZE);
	return 0;
}

uint32_t program_block_page(uint32_t address, uint8_t* data, uint32_t length)
{
	if (takeDownTest) {

		if (takeDownPeriod != 0) {
			takeDownPeriod--;
		}
		else {
			printf("Power failed at Program 0x%X .......\r\n", address);
			return 1;
		}
	}
	uint32_t i;
	if (address + length > BLOCK_SIZE) {
		assert(0);
	}
	for (i = 0; i < length; i++) {
		block[address + i] &= data[i];
	}
	return 0;
}

void tests(norFAT_FS* fs) {
	uint32_t i, j, tl, testLength, powered;
	uint32_t cycles;
	int32_t res = 0;
	uint8_t* test = malloc(0x10000);
	uint8_t* validate = malloc(0x10000);
	uint8_t* compare = malloc(0x10000);
	uint8_t buf[128];
	norfat_FILE* f;
	assert(test);
	assert(compare);
	assert(validate);
	srand((unsigned)time(NULL));
	for (i = 0; i < 0x10000; i++) {
		test[i] = rand();
	}
	for (i = 0; i < 0x10000; i++) {
		validate[i] = rand();
	}
	res = i = 0;
	res = norfat_format(fs);
	res = norfat_mount(fs);
	f = norfat_fopen(fs, "validate.bin", "w");
	res = norfat_fwrite(fs, validate, 1, 0x9988, f);
	res = norfat_fclose(fs, f);
	cycles = 10000;
	while (res == 0) {
		powered = 1;
		takeDownTest = 0;
		takeDownPeriod = 10 + (rand() % 2500);
		printf("Mount attempt .......\r\n");
		res = norfat_mount(fs);
		if (res) {
			printf("Failed %i\r\n", res);
			break;
		}
		else {
			printf("Mount success, work...\r\n");
			//norfat_finfo(fs);
		}
		f = norfat_fopen(fs, "validate.bin", "r");
		if (res || !f) {
			printf("File Failed %i\r\n", res);
			break;
		}
		res = norfat_fread(fs, compare, 1, 0x9988, f);
		if (res != 0x9988) {
			printf("File Failed %i\r\n", res);
			break;
		}
		if (memcmp(compare, validate, 0x9988)) {
			printf("File Failed\r\n");
			break;
		}
		res = norfat_fclose(fs, f);
		if (res) {
			printf("File close Failed %i\r\n", res);
			break;
		}
		takeDownTest = 1;
		while (res == 0) {
			//Save and stuff until it dies
			sprintf(buf, "test%i.txt", i++ % 10);
			f = norfat_fopen(fs, buf, "w");
			if (f == NULL) {
				res = NORFAT_ERR_NULL;
			}
			testLength = (test[i%0x8000] << 8) + test[i%0x7999];
			if (testLength == 0) {
				testLength = 1;
			}
			j = testLength;
			while (j) {
				tl = test[j];//Some random test length
				if (!tl) {
					tl = rand();
					tl &= 0xFF;
				}
				if (tl > j) {
					tl = j;
				}
				res = norfat_fwrite(fs, &test[testLength - j], 1, tl, f);
				if (res) {
					//norfat_fclose(fs, f);
					break;
				}
				j -= tl;
			}

			res = norfat_fclose(fs, f);
		}
		if (res != NORFAT_ERR_IO) {
			printf("Failed %i\r\n", res);
			break;
		}
		res = 0;
		if (cycles-- == 0) {
			printf("Cycle test success\r\n");
			break;
		}
	}
	//Multiple open files with power cycles
	//Plain power cycle failure resistance
	//Random times
	//Rollover
	takeDownTest = 0;
	res = norfat_format(fs);
	res = norfat_mount(fs);
	res = 0;
	i = 0;
	while (res == 0) {
		sprintf(buf, "test%i.txt", i++ % 10);
		f = norfat_fopen(fs, buf, "w");
		if (f == NULL) {
			res = NORFAT_ERR_NULL;
		}
		testLength = (test[i] << 8) + test[i];
		if (testLength == 0) {
			testLength = 1;
		}
		j = testLength;
		while (j) {
			tl = test[j];//Some random test length
			if (!tl) {
				tl = rand();
				tl &= 0xFF;
			}
			if (tl > j) {
				tl = j;
			}
			res = norfat_fwrite(fs, &test[testLength - j], 1, tl, f);
			if (res) {
				//norfat_fclose(fs, f);
				break;
			}
			j -= tl;
		}

		res = norfat_fclose(fs, f);
		if (fs->fat->swapCount > 10) {
			break;
		}
		f = norfat_fopen(fs, buf, "r");
		if (f == NULL) {
			res = NORFAT_ERR_NULL;
		}
		else {
			res = norfat_fread(fs, compare, 1, testLength, f);
			if (res != testLength) {
				norfat_fclose(fs, f);
				printf("Test file did not read\r\n");
				break;
			}
			if (memcmp(test, compare, testLength)) {
				norfat_fclose(fs, f);
				for (i = 0; i < testLength; i++) {
					if (test[i] != compare[i]) {
						break;
					}
				}
				printf("Test file did not match at %i 0x%X 0x%X 0x%X 0x%X\r\n", i, test[i], compare[i], (uint32_t)&test[i], (uint32_t)&compare[i]);
				break;
			}
			res = norfat_fclose(fs, f);
		}
	}
	if (res != NORFAT_OK) {
		printf("Test failed, some rollover issue");
		return;
	}
	else {
		norfat_finfo(fs);
		printf("Rollover test succeeded");
	}
	res = norfat_format(fs);
	res = norfat_mount(fs);
	//Wear leveling

	//Does it really fill up?
	res = 0;
	i = 0;
	while (res == 0) {
		sprintf(buf, "test%i.txt", i++);
		f = norfat_fopen(fs, buf, "w");
		if (f == NULL) {
			res = NORFAT_ERR_NULL;
		}
		res = norfat_fwrite(fs, test, 1, (test[i] << 8) + test[i], f);
		if (res) {
			norfat_fclose(fs, f);
			break;
		}
		res = norfat_fclose(fs, f);
	}
	if (res != NORFAT_ERR_FULL) {
		printf("Test failed, did not fill up\r\n");
		return;
	}
	else {
		printf("Fill up test success\r\n");
	}
	res = norfat_format(fs);
	res = norfat_mount(fs);


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
	if (res == NORFAT_ERR_EMPTY) {
		res = norfat_format(&fs);
		res = norfat_mount(&fs);
	}
	tests(&fs);
	return 1;
}