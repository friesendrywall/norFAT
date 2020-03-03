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
	//printf("BLOCK: Erasing 0x%X\r\n", address);
	if (takeDownTest) {

		if (takeDownPeriod != 0) {
			takeDownPeriod--;
		}
		else {
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
	//printf("BLOCK: Programming 0x%X 0x%X\r\n", address, length);
	if (takeDownTest) {

		if (takeDownPeriod != 0) {
			takeDownPeriod--;
		}
		else {
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

int32_t fillUpTest(norFAT_FS* fs) {

}

void tests(norFAT_FS* fs) {
	uint32_t i, j, tl, testLength, powered;
	int32_t res = 0;
	uint8_t * test = malloc(0x10000);
	uint8_t * compare = malloc(0x10000);
	uint8_t buf[128];
	norfat_FILE* f;
	assert(test);
	assert(compare);
	srand((unsigned)time(NULL));
	for (i = 0; i < 0x10000; i++) {
		test[i] = rand();
	}
	res = i = 0;
	res = norfat_format(fs);
	
	while (res == 0) {
		powered = 1;
		takeDownTest = 0;
		takeDownPeriod = 10 + (rand() % 100000);
		res = norfat_mount(fs);
		if (res) {
			printf("Failed\r\n");
		}
		else {
			norfat_finfo(fs);
		}
		takeDownTest = 1;
		while (res == 0) {
			//Save and stuff until it dies
			sprintf(buf, "test%i.txt", i++ % 10);
			f = norfat_fopen(fs, buf, NORFAT_FLAG_WRITE);
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
				res = norfat_fwrite(fs, f, &test[testLength - j], tl);
				if (res) {
					norfat_fclose(fs, f);
					break;
				}
				j -= tl;
			}

			res = norfat_fclose(fs, f);
		}
		if (res != NORFAT_ERR_IO) {
			break;
		}
	}
	//Multiple open files with power cycles
	//Plain power cycle failure resistance
	//Random times
	//Rollover
	res = 0;
	i = 0;
	while (res == 0) {
		sprintf(buf, "test%i.txt", i++ % 10);
		f = norfat_fopen(fs, buf, NORFAT_FLAG_WRITE);
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
			res = norfat_fwrite(fs, f, &test[testLength - j], tl);
			if (res) {
				norfat_fclose(fs, f);
				break;
			}
			j -= tl;
		}

		res = norfat_fclose(fs, f);
		if (fs->fat->swapCount > 10) {
			break;
		}
		f = norfat_fopen(fs, buf, NORFAT_FLAG_READ);
		if (f == NULL) {
			res = NORFAT_ERR_NULL;
		}
		else {
			res = norfat_fread(fs, f, compare, testLength);
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
		f = norfat_fopen(fs, buf, NORFAT_FLAG_WRITE);
		if (f == NULL) {
			res = NORFAT_ERR_NULL;
		}
		res = norfat_fwrite(fs, f, test, (test[i]<<8) + test[i]);
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

	uint32_t i;
	uint8_t test[16384];
	uint8_t compare[16384];
	for (i = 0; i < sizeof(test); i++) {
		test[i] = rand();
	}
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
		res = norfat_mount(&fs);
	}
	tests(&fs);
	/*
	norfat_FILE* f = norfat_fopen(&fs, "Test.json", NORFAT_FLAG_WRITE);
	res = norfat_fwrite(&fs, f, "Hello world!", 12);
	res = norfat_fclose(&fs, f);

	f = norfat_fopen(&fs, "Test.json", NORFAT_FLAG_WRITE);
	res = norfat_fwrite(&fs, f, test, 9000);
	res = norfat_fclose(&fs, f);

	f = norfat_fopen(&fs, "Test.json", NORFAT_FLAG_READ);

	res = norfat_fread(&fs, f, compare, 9000);
	if (memcmp(test, compare, 9000)) {
		printf("Test failure\r\n");
	}
	else {
		printf("Test Success\r\n");
	}	
	norfat_finfo(&fs);
	for (i = 0; i < sizeof(test); i++) {
		test[i] = rand();
	}
	f = norfat_fopen(&fs, "Test.json", NORFAT_FLAG_WRITE);
	res = norfat_fwrite(&fs, f, test, 9500);
	res = norfat_fclose(&fs, f);

	f = norfat_fopen(&fs, "Test.json", NORFAT_FLAG_READ);

	res = norfat_fread(&fs, f, compare, 9500);
	if (memcmp(test, compare, 9500)) {
		printf("Test failure\r\n");
	}
	else {
		printf("Test Success\r\n");
	}
	norfat_finfo(&fs);*/
	return 1;
}