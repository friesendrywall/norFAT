#define _CRT_RAND_S 
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "norFAT.h"

#define NORFAT_SECTORS		    1534
#define NORFAT_SECTOR_SIZE	    4096
#define NORFAT_TABLE_SECTORS	2
#define NORFAT_TABLE_COUNT		6

#define TRACE_BUFFER_SIZE (10 * 1024 * 1024)
uint32_t POWER_CYCLE_COUNT = 2500;
#define BLOCK_SIZE (NORFAT_SECTORS * NORFAT_SECTOR_SIZE)
uint8_t block[BLOCK_SIZE];

uint32_t EraseCounts[NORFAT_SECTORS];

uint32_t takeDownPeriod = 0;
uint32_t takeDownTest = 0;

FILE* traceFile = NULL;
char* traceBuffer = NULL;
uint32_t traceLocation = 0;

uint32_t read_block_device(uint32_t address, uint8_t* data, uint32_t len) {
	if (address + len > BLOCK_SIZE) {
		NORFAT_ASSERT(0);
	}
	if (takeDownTest) {

		if (takeDownPeriod != 0) {
			takeDownPeriod--;
		}
		else {
			//printf("Power failed at read 0x%X .......\r\n", address);
			memcpy(data, &block[address], len / 2);
			return 1;
		}
	}
	//printf("Read 0x%X len 0x%X\r\n", address, len);
	memcpy(data, &block[address], len);
	return 0;
}

uint32_t erase_block_sector(uint32_t address)
{
	if (address < NORFAT_TABLE_SECTORS * NORFAT_TABLE_COUNT * NORFAT_SECTOR_SIZE) {
		traceHandler("erase_block_sector(0x%X)\r\n", address);
	}
	if (address + NORFAT_SECTOR_SIZE > BLOCK_SIZE) {
		NORFAT_ASSERT(0);
	}
	if (takeDownTest) {

		if (takeDownPeriod != 0) {
			takeDownPeriod--;
		}
		else {
			memset(&block[address], 0xFF, NORFAT_SECTOR_SIZE / 2);
			//printf("Power failed at erase 0x%X .......\r\n", address);
			return 1;
		}
	}
	EraseCounts[address / NORFAT_SECTOR_SIZE] ++;

	memset(&block[address], 0xFF, NORFAT_SECTOR_SIZE);
	return 0;
}

uint32_t program_block_page(uint32_t address, uint8_t* data, uint32_t length)
{	
	uint32_t i;
	if (address < NORFAT_TABLE_SECTORS * NORFAT_TABLE_COUNT * NORFAT_SECTOR_SIZE) {
		traceHandler("program_block_page(0x%X)(%i)\r\n", address, length);
	}
	if (address + length > BLOCK_SIZE) {
		NORFAT_ASSERT(0);
	}
	if (takeDownTest) {

		if (takeDownPeriod != 0) {
			takeDownPeriod--;
		}
		else {
			for (i = 0; i < length / 2; i++) {
				block[address + i] &= data[i];
			}
			//printf("Power failed at Program 0x%X .......\r\n", address);
			return 1;
		}
	}
	for (i = 0; i < length; i++) {
		block[address + i] &= data[i];
	}
	return 0;
}

static uint32_t getRand(void) {
	uint32_t ret, a = 0;
	if (rand_s(&ret) == EINVAL) {
#ifdef _DEBUG
		while (1) {
			a++;
		}
#else
		exit(1);
#endif
	}
	return ret;
}

int traceHandler(const char* format, ...) {
	char buf[1024];//Not thread safe
	int n;
	va_list argptr;
	va_start(argptr, format);
	n = vsprintf(buf, format, argptr);
	va_end(argptr);
	if (traceBuffer != NULL) {
		if (traceLocation + n > TRACE_BUFFER_SIZE) {
			uint32_t tail = TRACE_BUFFER_SIZE - traceLocation;
			memcpy(&traceBuffer[traceLocation], buf, tail);
			n -= tail;
			memcpy(traceBuffer, &buf[tail], n);
			traceLocation = n;
		}
		else {
			memcpy(&traceBuffer[traceLocation], buf, n);
			traceLocation += n;
		}

	}
	return 0;
}

void writeTraceToFile(void) {
	traceFile = fopen("norfat_trace.txt", "wb");
	if (traceFile != NULL) {
		uint32_t tail = TRACE_BUFFER_SIZE - traceLocation; 
		fwrite(&traceBuffer[traceLocation], 1, tail, traceFile);
		fwrite(traceBuffer, 1, traceLocation, traceFile);
		fclose(traceFile);
	}
#if 0
	FILE* f = fopen("fs_block0.bin", "wb");
	fwrite(&block[0], 1, 8192, f);
	fclose(f);

	f = fopen("fs_block1.bin", "wb");
	fwrite(&block[0x2000], 1, 8192, f);
	fclose(f);

	f = fopen("fs_block2.bin", "wb");
	fwrite(&block[0x4000], 1, 8192, f);
	fclose(f);

	f = fopen("fs_block3.bin", "wb");
	fwrite(&block[0x6000], 1, 8192, f);
	fclose(f);

	f = fopen("fs_block4.bin", "wb");
	fwrite(&block[0x8000], 1, 8192, f);
	fclose(f);

	f = fopen("fs_block5.bin", "wb");
	fwrite(&block[0xA000], 1, 8192, f);
	fclose(f);
#endif

}

void assertHandler(char* file, int line) {
	printf("NORFAT_ASSERT(%s:%i\r\n", file, line);
	writeTraceToFile();
	int a = 0;
#ifdef _DEBUG
	while (1) {
		a++;
	}
#else
	exit(1);
#endif

}

int PowerFailOnWriteTest(norFAT_FS* fs) {
	uint32_t i, j, tl, testLength, powered;
	uint32_t rnd = 0;
	int32_t res = 0;
	uint8_t* validate = malloc(0x10000);
	uint8_t* newwrite = malloc(0x10000);
	uint8_t* compare = malloc(0x10000);
	uint8_t buf[128];
	norfat_FILE* f;
	assert(compare);
	assert(validate);
	assert(newwrite);
	srand((unsigned)time(NULL));
	j = 0;
	for (i = 0; i < 0x10000; i++) {
		validate[i] = (uint8_t)getRand();
		newwrite[i] = (uint8_t)getRand();
	}
	res = i = 0;
	res = norfat_format(fs);
	res = norfat_mount(fs);
	f = norfat_fopen(fs, "validate.bin", "w");
	res = norfat_fwrite(fs, validate, 1, 0x9988, f);
	res = norfat_fclose(fs, f);

	res = norfat_mount(fs);
	if (res) {
		printf("Mount failed err %i\r\n", res);
		goto finalize;
	}
	//Write halfway
	if (memcmp(compare, newwrite, 0x9988) == 0) {
		printf("What!!?\r\n");
		res = 1;
		goto finalize;
	}
	f = norfat_fopen(fs, "validate.bin", "w");
	res = norfat_fwrite(fs, newwrite, 1, 0x8000, f);
	//Power failure here.
	res = norfat_mount(fs);
	f = norfat_fopen(fs, "validate.bin", "r");
	if (!f) {
		printf("File open failure\r\n");
		res = 1;
		goto finalize;
	}
	res = norfat_fread(fs, compare, 1, 0x9988, f);
	if (res != 0x9988) {
		printf("File Failed %i\r\n", res);
		if (res == 0) {
			res = norfat_ferror(fs, f);
		}
		goto finalize;;
	}
	if (memcmp(compare, validate, 0x9988)) {
		printf("File Failed\r\n");
		res = 1;
		goto finalize;
	}
	res = norfat_fclose(fs, f);
	if (res) {
		printf("File close Failed %i\r\n", res);
		goto finalize;
	}
	printf("Half write test passed\r\n");
finalize:
	free(newwrite);
	free(validate);
	free(compare);
	return res;
}

int PowerStressTest(norFAT_FS* fs) {
	uint32_t i, j, tl, testLength, powered;
	uint32_t powerCycleTest = 0;
	uint32_t powerCycleTestResult = 0;
	uint32_t powerCycleValidate = 0;
	uint32_t bytesWritten = 0;
	uint32_t rnd = 0;
	uint32_t cycles = POWER_CYCLE_COUNT;
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
	j = 0;
	for (i = 0; i < 0x10000; i++) {
		test[i] = (uint8_t)getRand();
	}
	for (i = 0; i < 0x10000; i++) {
		validate[i] = (uint8_t)getRand();
	}
	res = i = 0;
	res = norfat_format(fs);
	res = norfat_mount(fs);
	f = norfat_fopen(fs, "validate.bin", "w");
	res = norfat_fwrite(fs, validate, 1, 0x9988, f);
	res = norfat_fclose(fs, f);
#if 1
	f = norfat_fopen(fs, "powercycles.txt", "w");
	if (f == NULL) {
		return norfat_errno(fs);
	}
	res = norfat_fwrite(fs, &powerCycleTest, 1, 4, f);
	res = norfat_fclose(fs, f);
#endif
	while (res == 0) {
		powered = 1;
		takeDownTest = 1;
		takeDownPeriod = 1 + (getRand() % 2500);
		//printf("Mount attempt .......\r\n");
		res = norfat_mount(fs);
		if (res == NORFAT_ERR_IO) {
			res = 0;
			continue;
		}
		if (res) {
			printf("Mount failed err %i\r\n", res);
			break;
		}
		printf(".");
		if (cycles % 100 == 0) {
			printf("%i", cycles);
		}
		takeDownTest = 0;
		f = norfat_fopen(fs, "validate.bin", "r");
		if (res || !f) {
			printf("File Failed %i\r\n", res);
			break;
		}
		res = norfat_fread(fs, compare, 1, 0x9988, f);
		if (res != 0x9988) {
			printf("File Failed %i\r\n", res);
			if (res == 0) {
				res = norfat_ferror(fs, f);
			}
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

		//powerCycleTest = 0;
		f = norfat_fopen(fs, "powercycles.txt", "rb");
		if (f != NULL) {
			res = norfat_fread(fs, &powerCycleTestResult, 1, 4, f);
			res = norfat_fclose(fs, f);
			if (res == 0) {
				if (powerCycleValidate > powerCycleTestResult) {
					res = 10;
					break;
				}
				powerCycleValidate = powerCycleTestResult;
			}
			powerCycleTest = powerCycleTestResult + 1;
			f = norfat_fopen(fs, "powercycles.txt", "wb");
			if (f != NULL) {
				res = norfat_fwrite(fs, &powerCycleTest, 1, 4, f);
				res = norfat_fclose(fs, f);
			}
	}
		else {
			if (norfat_errno(fs) != NORFAT_ERR_IO) {
				return norfat_errno(fs);
			}
		}


		while (res == 0) {
			//Save and stuff until it dies
			sprintf(buf, "test%i.txt", i++ % 10);
			f = norfat_fopen(fs, buf, "w");
			if (f == NULL) {
				if (norfat_errno(fs) != NORFAT_ERR_IO) {
					res = NORFAT_ERR_NULL;
				}
				else {
					res = NORFAT_ERR_IO;
				}
				break;
			}
			testLength = (test[i % 0x8000] << 8) + test[i % 0x7999];
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
				if (!tl) {
					tl++;
				}
				res = norfat_fwrite(fs, &test[testLength - j], 1, tl, f);
				if (res != tl) {
					if (res == 0) {
						res = norfat_ferror(fs, f);
					}
					break;
				}
				bytesWritten += res;
				j -= tl;
			}

			res = norfat_fclose(fs, f);
			if (res) {
				if (res != NORFAT_ERR_IO) {
					printf("Error on close res %i\r\n", res);
					break;
				}
			}
		}
		if (res != NORFAT_ERR_IO) {
			printf("\r\nPower test stress failed err %i\r\n", res);
			break;
		}
		res = 0;
		if (cycles-- == 0) {
			printf("\r\nPower stress test passed (%i)\r\n%i bytes written\r\n", powerCycleTestResult, bytesWritten);
			break;
		}
	}
	free(test);
	free(validate);
	free(compare);
	if (res) {
		return res;
	}
	takeDownTest = 0;
	res = norfat_mount(fs);
	if (res) {
		printf("Mount failed err %i\r\n", res);
	}
	res = norfat_fsinfo(fs);
	return res;
}

int fillupTest(norFAT_FS* fs) {
	uint32_t i, j;
	uint32_t cycles;
	int32_t res = 0;
	uint8_t* test = malloc(0x10000);
	uint8_t buf[128];
	norfat_FILE* f;
	assert(test);
	srand((unsigned)time(NULL));
	j = 0;
	for (i = 0; i < 0x10000; i++) {
		test[i] = (uint8_t)getRand();
	}
	res = i = 0;
	res = norfat_format(fs);
	res = norfat_mount(fs);
	//Does it really fill up?
	res = 0;
	while (res == 0) {
		sprintf(buf, "test%i.txt", i++);
		f = norfat_fopen(fs, buf, "w");
		if (f == NULL) {
			res = NORFAT_ERR_NULL;
		}
		uint32_t wl = (test[i] << 8) + test[i];
		if (!wl) {
			wl = 1;
		}
		res = norfat_fwrite(fs, test, 1, wl, f);
		if (res <= 0) {
			norfat_fclose(fs, f);
			break;
		}
		res = norfat_fclose(fs, f);
	}
	free(test);
	if (res != NORFAT_ERR_FULL) {
		printf("Test failed, did not fill up err %i\r\n", res);
		return 1;
	}
	else {
		printf("Fill up test passed\r\n");
		return 0;
	}


}

int randomWriteLengths(norFAT_FS* fs) {
	uint32_t i, j, tl, testLength;
	int32_t res = 0;
	uint8_t* test = malloc(0x10000);
	uint8_t* compare = malloc(0x10000);
	uint8_t buf[128];
	norfat_FILE* f;
	assert(test);
	assert(compare);
	srand((unsigned)time(NULL));
	j = 0;
	for (i = 0; i < 0x10000; i++) {
		test[i] = (uint8_t)getRand();;
	}

	//Rollover
	res = norfat_format(fs);
	res = norfat_mount(fs);
	res = 0;
	i = 0;
	while (res == 0) {
		sprintf(buf, "test%i.txt", i++ % 10);
		f = norfat_fopen(fs, buf, "w");
		if (f == NULL) {
			res = NORFAT_ERR_NULL;
			break;
		}
		testLength = (uint8_t)getRand();
		testLength %= 0xFFFF;
		if (testLength == 0) {
			testLength = 1;
		}
		j = testLength;
		while (j) {
			//tl = test[j];//Some random test length

			tl = (uint8_t)getRand();
			tl %= testLength;
			if (!tl) {
				tl = 1;
			}
			if (tl > j) {
				tl = j;
			}
			res = norfat_fwrite(fs, &test[testLength - j], 1, tl, f);
			if (res != tl) {
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
				res = 10;
				printf("Test file did not match at %i 0x%X 0x%X 0x%X 0x%X\r\n", i, test[i], compare[i], (uint32_t)&test[i], (uint32_t)&compare[i]);
				break;
			}
			res = norfat_fclose(fs, f);
		}
	}
	free(test);
	free(compare);
	if (res != NORFAT_OK) {
		printf("Test failed, some rollover issue\r\n");
		return 1;
	}
	else {
		printf("\r\n");
		norfat_fsinfo(fs);
		printf("\r\nRollover test succeeded\r\n");
		return 0;
	}

}

int wearLeveling(norFAT_FS* fs) {
	int i, j;
	uint64_t val;
	uint32_t min = 1000000;
	uint32_t max = 0;
	int minIndex = 0;
	int maxIndex = 0;
#if 1
	printf("Wear results\r\n");
	printf("     |00|01|02|03|04|05|06|07|08|09|0A|0B|0C|0D|0E|0F");
	for (i = 0; i < NORFAT_SECTORS; i++) {
		if (i % 16 == 0) {
			printf("|\r\n[%3X]", i / 16);
		}
		if (i <= (fs->tableCount * fs->tableSectors)) {
			printf("!%02i", EraseCounts[i]);
		}
		else {
			printf("|%02i", EraseCounts[i]);
		}

	}
	printf("\r\n");
#else
	for (i = 0; i < NORFAT_SECTORS; i++) {
		if (EraseCounts[i] > max) {
			max = EraseCounts[i];
			maxIndex = i / 16;
		}
		if (min > EraseCounts[i]) {
			min = EraseCounts[i];
			minIndex = i / 16;
		}
	}
	printf("Wear results min[%i] %i  max[%i] %i\r\n\r\n", minIndex, min, maxIndex, max);
	uint32_t factor = max / 20;
	if (!factor) {
		factor = 1;
	}
	uint32_t results[NORFAT_SECTORS / 16];
	for (i = 0; i < NORFAT_SECTORS / 16; i++) {
		val = 0;
		for (j = 0; j < 16; j++) {
			val += EraseCounts[i * 16 + j];
		}
		val /= 16;
		val /= factor;
		/*printf("\r\n%02i ", i);
		for (j = 0; j < val; j++) {
			printf("*");
		}*/
		if (minIndex == i) {
			results[i] = min / factor;
		}
		else if (maxIndex == i) {
			results[i] = max / factor;
		}
		else {
			results[i] = val;
		}

	}

	for (i = 19; i >= 0; i--) {
		printf("    ");
		for (j = 0; j < NORFAT_SECTORS / 16; j++) {
			if (results[j] >= i) {
				if (j == minIndex) {
					printf("m");
				}
				else if (j == maxIndex) {
					printf("M");
				}
				else {
					printf("*");
				}
			}
			else {
				printf(" ");
			}
		}
		printf("\r\n");
	}
	printf("----");
	for (i = 0; i < NORFAT_SECTORS / 16; i++) {
		printf("%i", i % 10);
	}
	printf("\r\n");
#endif
	return 0;
}

int deleteTest(norFAT_FS* fs) {
	int res;
	norfat_FILE* f;
	res = norfat_format(fs);
	res = norfat_mount(fs);
	f = norfat_fopen(fs, "testfile.bin", "wb");
	if (f == NULL) {
		return 1;
	}
	res = norfat_fwrite(fs, "Hello world!", 1, 12, f);
	if (res != 12) {
		norfat_fclose(fs, f);
		return 1;
	}
	res = norfat_fclose(fs, f);
	if (res) {
		return 1;
	}
	if (norfat_exists(fs, "testfile.bin") != 12) {
		printf("File doesn't exist where it should\r\n");
		return 1;
	}
	res = norfat_remove(fs, "testfile.bin");
	if (res) {
		printf("File remove error\r\n");
		return 1;
	}
	if (norfat_exists(fs, "testfile.bin") != 0) {
		printf("File exists where it shouldn't\r\n");
		return 1;
	}
	printf("File remove test passed\r\n");
	return 0;
}

int runTestSuite(norFAT_FS* fs) {
	int res = 0;
	res = PowerStressTest(fs);
	if (res) {
		printf("Power cycle stress test failed err %i\r\n", res);
		writeTraceToFile();
		return res;
	}
	res = deleteTest(fs);
	if (res) {
		printf("Delete test err %i\r\n", res);
		writeTraceToFile();
		return res;
	}

	res = fillupTest(fs);
	if (res) {
		printf("Fill up test err %i\r\n", res);
		writeTraceToFile();
		return res;
	}
	res = randomWriteLengths(fs);
	if (res) {
		printf("Random write length test err %i\r\n", res);
		writeTraceToFile();
		return res;
	}
	res = wearLeveling(fs);
	if (res) {
		printf("Wear leveling test failed %i\r\n", res);
		writeTraceToFile();
		return res;
	}

	res = PowerFailOnWriteTest(fs);
	if (res) {
		printf("Half write test failed %i\r\n", res);
		writeTraceToFile();
		return res;
	}
	printf("Passed all tests\r\n");
	return res;
}

int main(int argv, char** argc) {

	norFAT_FS fs1 = {
		.addressStart = 0,
		.tableCount = NORFAT_TABLE_COUNT - 2,//FAT tables carved out of flash sectors
		.tableSectors = NORFAT_TABLE_SECTORS,
		.flashSectors = NORFAT_SECTORS,
		.sectorSize = NORFAT_SECTOR_SIZE,
		.programSize = 256,
		.erase_block_sector = erase_block_sector,
		.program_block_page = program_block_page,
		.read_block_device = read_block_device
	};

	norFAT_FS fs2 = {
	.addressStart = 0,
	.tableCount = NORFAT_TABLE_COUNT,//FAT tables carved out of flash sectors
	.tableSectors = NORFAT_TABLE_SECTORS,
	.flashSectors = NORFAT_SECTORS,
	.sectorSize = NORFAT_SECTOR_SIZE,
	.programSize = 256,
	.erase_block_sector = erase_block_sector,
	.program_block_page = program_block_page,
	.read_block_device = read_block_device
	};

	printf("norFAT test jig 1.00, norFAT Version %s\r\n", NORFAT_VERSION);
	if (argv > 1) {
		POWER_CYCLE_COUNT = strtol(argc[1], NULL, 10);
		printf("Testing cycles set to %i\r\n", POWER_CYCLE_COUNT);
	}
	memset(block, 0xFF, BLOCK_SIZE);
	fs1.buff = malloc(NORFAT_SECTOR_SIZE * 2);
	fs1.fat = malloc(NORFAT_SECTOR_SIZE * 2);
	fs2.buff = malloc(NORFAT_SECTOR_SIZE * 2);
	fs2.fat = malloc(NORFAT_SECTOR_SIZE * 2);
	traceBuffer = malloc(TRACE_BUFFER_SIZE);
	//traceFile = fopen("norfat_trace.txt", "wb");
	int32_t res = norfat_mount(&fs1);
	if (res == NORFAT_ERR_EMPTY) {
		res = norfat_format(&fs1);
		res = norfat_mount(&fs1);
	}
	res = runTestSuite(&fs1);
	if (res) {
		return res;
	}
	memset(block, 0xFF, BLOCK_SIZE);
	memset(EraseCounts, 0, sizeof(EraseCounts));
	res = norfat_mount(&fs2);
	if (res == NORFAT_ERR_EMPTY) {
		res = norfat_format(&fs2);
		res = norfat_mount(&fs2);
	}
	res = runTestSuite(&fs2);
	free(traceBuffer);
	return res;
}