#include "norFAT.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NORFAT_FILE_NOT_FOUND (-1)
#define NORFAT_INVALID_SECTOR (0xFFFFFFFF)

#define NORFAT_FLAG_READ 1
#define NORFAT_FLAG_WRITE 2
#define NORFAT_FLAG_ZERO_COPY 4 // Not implemented for writes
#define NORFAT_FILE_CRC_CHECK 8

#define NORFAT_SOF_MSK (0x70000000)
#define NORFAT_SOF_MATCH (0x30000000)
#define NORFAT_EOF (0x0FFFFFFF)

#define NORFAT_EMPTY_MASK (0xFFFFFFFF)
#define NORFAT_GARBAGE_MASK (0x00000000)

#define NORFAT_TABLE_GOOD 0
#define NORFAT_TABLE_OLD 1
#define NORFAT_TABLE_EMPTY 2
#define NORFAT_TABLE_CRC 3

#define NORFAT_TABLE_BYTES(sectors) (sizeof(_FAT) + (sizeof(_sector) * sectors))

#ifdef NORFAT_COVERAGE_TEST
uint32_t scenarioList[64];
uint32_t scenarioCnt[64];
#endif

static int32_t commitChanges(norFAT_FS *fs, uint32_t forceSwap);

#ifndef NORFAT_CRC
/* crc routines written by unknown public source */
static uint32_t crc32_table[256];
void init_crc32(void);
static uint32_t crc32(void *buf, int len, uint32_t Seed);

#define CRC32_POLY 0x04c11db7 /* AUTODIN II, Ethernet, & FDDI 0x04C11DB7 */
#define NORFAT_CRC crc32

uint32_t crc32(void *buf, int len, uint32_t Seed) {
  NORFAT_TRACE(("CRC:0x%p|%i|0x%X\r\n", buf, len, Seed));
  unsigned char *p;
  uint32_t crc = Seed;
  if (!crc32_table[1]) { /* if not already done, */
    init_crc32();        /* build table */
  }
  for (p = buf; len > 0; ++p, --len) {
    crc = (crc << 8) ^ crc32_table[(crc >> 24) ^ *p];
  }
  return crc; /* transmit complement, per CRC-32 spec */
}

void init_crc32(void) {
  int i, j;
  uint32_t c;
  for (i = 0; i < 256; ++i) {
    for (c = i << 24, j = 8; j > 0; --j) {
      c = c & 0x80000000 ? (c << 1) ^ CRC32_POLY : (c << 1);
    }
    crc32_table[i] = c;
  }
}

#endif

static uint32_t calcTableCrc(norFAT_FS *fs, uint32_t index) {
  uint32_t crcRes;
  uint32_t crclen = (sizeof(_FAT) + (sizeof(_sector) * fs->flashSectors)) -
                    (sizeof(_commit) * (index + 1));
  crcRes = NORFAT_CRC(&fs->fat->commit[index + 1], crclen, 0xFFFFFFFF);
  NORFAT_TRACE(("calcTableCrc[%i](%i) 0x%X\r\n", index, crclen, crcRes));
  return crcRes;
}

static void updateTableCrc(norFAT_FS *fs, uint32_t index) {
  char cr[9];
  NORFAT_TRACE(("updateTableCrc[%i]\r\n", index));
  uint32_t crcRes = calcTableCrc(fs, index);
  snprintf(cr, 9, "%08X", crcRes);
  memcpy(&fs->fat->commit[index], cr, 8);
}

static uint32_t findCrcIndex(_FAT *fat) {
  uint32_t i;
  for (i = NORFAT_CRC_COUNT - 1; i > 0; i--) {
    if (fat->commit[i].crc[0] != 0xFF) {
      break;
    }
  }
  return i;
}

static int32_t scanTable(norFAT_FS *fs, _FAT *fat) {
  uint32_t i;
  uint32_t wasRepaired = 0;
  NORFAT_TRACE(("scanTable()\r\n"));
  for (i = (fs->tableCount * fs->tableSectors); i < fs->flashSectors; i++) {
    if (fat->sector[i].write && !fat->sector[i].available) {
      NORFAT_DEBUG(("Sector %i recovered\r\n", i));
      NORFAT_TRACE(("SECTOR:recover %i\r\n", i));
      fat->sector[i].base |= NORFAT_EMPTY_MASK;
      wasRepaired = 1;
    }
  }
  return wasRepaired;
}

static int32_t copyTable(norFAT_FS *fs, uint32_t toIndex, uint32_t fromIndex) {
  toIndex %= fs->tableCount;
  fromIndex %= fs->tableCount;
  NORFAT_TRACE(("copyTable(%i -> %i)\r\n", fromIndex, toIndex));
  if (fs->read_block_device(
          fs->addressStart + ((fs->sectorSize * fs->tableSectors) * fromIndex),
          (uint8_t *)fs->buff, (fs->sectorSize * fs->tableSectors))) {
    fs->lastError = NORFAT_ERR_IO;
    NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
    return NORFAT_ERR_IO;
  }
  if (fs->program_block_page(
          fs->addressStart + ((fs->sectorSize * fs->tableSectors) * toIndex),
          (uint8_t *)fs->buff, (fs->sectorSize * fs->tableSectors))) {
    fs->lastError = NORFAT_ERR_IO;
    NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
    return NORFAT_ERR_IO;
  }
  return NORFAT_OK;
}

static int32_t eraseTable(norFAT_FS *fs, uint32_t tableIndex) {
  uint32_t i;
  tableIndex %= fs->tableCount;
  NORFAT_TRACE(("eraseTable(%i)\r\n", tableIndex));
  for (i = 0; i < fs->tableSectors; i++) {
    if (fs->erase_block_sector(
            fs->addressStart +
            (tableIndex * (fs->sectorSize * fs->tableSectors)) +
            (i * fs->sectorSize))) {
      fs->lastError = NORFAT_ERR_IO;
      NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
      return NORFAT_ERR_IO;
    }
  }
  return NORFAT_OK;
}

static uint32_t loadTable(norFAT_FS *fs, uint32_t tableIndex) {
  uint32_t crcRes, j;
  char cr[9];
  tableIndex %= fs->tableCount;
  NORFAT_TRACE(("loadTable(%i)\r\n", tableIndex));
  if (fs->read_block_device(
          fs->addressStart + ((fs->sectorSize * fs->tableSectors) * tableIndex),
          (uint8_t *)fs->fat, (fs->sectorSize * fs->tableSectors))) {
    fs->lastError = NORFAT_ERR_IO;
    NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
    return NORFAT_ERR_IO;
  }
  j = findCrcIndex(fs->fat);
  NORFAT_TRACE(("loadTable:crc[%i]\r\n", j));
  memcpy(cr, &fs->fat->commit[j], 8);
  cr[8] = 0;
  uint32_t crclen =
      NORFAT_TABLE_BYTES(fs->flashSectors) - (sizeof(_commit) * (j + 1ULL));
  crcRes = NORFAT_CRC(&fs->fat->commit[j + 1], crclen, 0xFFFFFFFF);
  if (crcRes != strtoul(cr, NULL, 0x10)) {
    NORFAT_TRACE(("loadTable:failure 0x%X != 0x%s\r\n", crcRes, cr));
    NORFAT_ERROR(("Table %i crc failure\r\n", tableIndex));
    return NORFAT_ERR_CRC;
  }
  NORFAT_TRACE(("loadTable:CRC 0x%X\r\n", crcRes));
  NORFAT_DEBUG(("Table %i crc match 0x%X\r\n", tableIndex, crcRes));
  return NORFAT_OK;
}

static int32_t validateTable(norFAT_FS *fs, uint32_t tableIndex,
                             uint32_t *crc) {
  uint32_t crcRes, j;
  int32_t res = NORFAT_TABLE_GOOD;
  char cr[9];
  tableIndex %= fs->tableCount;
  NORFAT_TRACE(("validateTable(%i)\r\n", tableIndex));
  _FAT *fat = (_FAT *)fs->buff;
  if (fs->read_block_device(
          fs->addressStart + ((fs->sectorSize * fs->tableSectors) * tableIndex),
          (uint8_t *)fat, (fs->sectorSize * fs->tableSectors))) {
    fs->lastError = NORFAT_ERR_IO;
    NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
    return NORFAT_ERR_IO;
  }
  // See if table is completely empty
  for (j = 0; j < (fs->sectorSize * fs->tableSectors); j++) {
    if (fs->buff[j] != 0xFF) {
      break;
    }
  }
  if (j == (fs->sectorSize * fs->tableSectors)) {
    NORFAT_TRACE(("validateTable(%i):empty\r\n", tableIndex));
    return NORFAT_TABLE_EMPTY;
  }
  j = findCrcIndex(fat);
  NORFAT_TRACE(("validateTable:crc[%i]\r\n", j));
  memcpy(cr, &fat->commit[j], 8);
  cr[8] = 0;
  uint32_t crclen =
      NORFAT_TABLE_BYTES(fs->flashSectors) - (sizeof(_commit) * (j + 1ULL));
  crcRes = NORFAT_CRC(&fat->commit[j + 1], crclen, 0xFFFFFFFF);
  if (crcRes != strtoul(cr, NULL, 0x10)) {
    NORFAT_TRACE(
        ("validateTable:failure 0x%X != 0x%s (%i)\r\n", crcRes, cr, crclen));
    return NORFAT_TABLE_CRC;
  }
  if (crc) {
    *crc = crcRes;
  }
  NORFAT_TRACE(("validateTable:CRC 0x%X\r\n", crcRes));
  NORFAT_DEBUG(("Table %i crc[%i] match 0x%X\r\n", tableIndex, j, crcRes));
  return res;
}

static int32_t garbageCollect(norFAT_FS *fs) {
  uint32_t i;
  uint32_t collected = 0;
  NORFAT_TRACE(("garbageCollect():"));
  for (i = (fs->tableCount * fs->tableSectors); i < fs->flashSectors; i++) {
    if (!fs->fat->sector[i].active) {
      fs->fat->sector[i].base |= NORFAT_EMPTY_MASK;
      NORFAT_TRACE(("[%i]", i));
      collected = 1;
    }
  }
  NORFAT_TRACE(("\r\n"));
  if (collected) {
    fs->fat->garbageCount++;
    return commitChanges(fs, 1);
  } else {
    NORFAT_TRACE(("garbageCollect: FULL\r\n"));
    fs->lastError = NORFAT_ERR_FULL;
    return NORFAT_ERR_FULL;
  }
}

static int32_t findEmptySector(norFAT_FS *fs) {

  uint32_t i;
  int32_t res;
  uint32_t sp = NORFAT_RAND() % fs->flashSectors;
  NORFAT_TRACE(("findEmptySector().."));
  if (sp < (fs->tableCount * fs->tableSectors)) {
    sp = fs->flashSectors / 2;
  }
  for (i = sp; i < fs->flashSectors; i++) {
    if (fs->fat->sector[i].available) {
      fs->fat->sector[i].available = 0;
      NORFAT_TRACE(("[%i]\r\n", i));
      return i;
    }
  }
  for (i = (fs->tableCount * fs->tableSectors); i < sp; i++) {
    if (fs->fat->sector[i].available) {
      fs->fat->sector[i].available = 0;
      NORFAT_TRACE(("[%i]\r\n", i));
      return i;
    }
  }

  res = garbageCollect(fs);
  if (res) {
    return res;
  }
  for (i = (fs->tableCount * fs->tableSectors); i < fs->flashSectors; i++) {
    if (fs->fat->sector[i].available) {
      fs->fat->sector[i].available = 0;
      NORFAT_TRACE(("[%i]\r\n", i));
      return i;
    }
  }
  NORFAT_TRACE(("FULL\r\n"));
  return NORFAT_ERR_FULL;
}

static int fileSearch(norFAT_FS *fs, const char *filename, uint32_t *sector,
                      norFAT_fileHeader *fh, uint32_t *len) {
  uint32_t i;
  int foundFile = NORFAT_ERR_FILE_NOT_FOUND;
  *sector = NORFAT_INVALID_SECTOR;
  NORFAT_TRACE(("fileSearch(%s)..", filename));
  for (i = (fs->tableCount * fs->tableSectors); i < fs->flashSectors; i++) {
    if ((fs->fat->sector[i].base & NORFAT_SOF_MSK) == NORFAT_SOF_MATCH) {
      if (fs->read_block_device(fs->addressStart + (i * fs->sectorSize),
                                fs->buff, sizeof(norFAT_fileHeader))) {
        fs->lastError = NORFAT_ERR_IO;
        NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
        return NORFAT_ERR_IO;
      }
      NORFAT_TRACE(("[%s]", fs->buff));
      // Somewhat wasteful, but we need to allow read function to call
      // cache routines on a safe buffer
      if (strcmp((char *)fs->buff, filename) == 0) {
        *sector = i;
        NORFAT_TRACE(("sector[%i]\r\n", i));
        NORFAT_DEBUG(("File %s found at sector %i\r\n", filename, i));
        if (fh) {
          memcpy(fh, fs->buff, sizeof(norFAT_fileHeader));
        }
        if (len) {
          norFAT_fileHeader *f = (norFAT_fileHeader *)fs->buff;
          *len = f->fileLen;
        }
        foundFile = NORFAT_OK;
        break;
      }
    }
  }
  if (!foundFile) {
    NORFAT_TRACE(("\r\n", i));
  }
  return foundFile;
}

static int commitChanges(norFAT_FS *fs, uint32_t forceSwap) {
  uint32_t i;
  uint32_t index = findCrcIndex(fs->fat);
  NORFAT_TRACE(("commitChanges(%s)..\r\n", forceSwap ? "force" : ".."));
  /* Protect fs state */
  if (fs->lastError == NORFAT_ERR_IO) {
    return NORFAT_ERR_IO;
  }
  // Is current table set full?
  if (index == NORFAT_CRC_COUNT - 1 || forceSwap) {
    NORFAT_TRACE(("commitChanges:Increment _FAT tables %i\r\n", fs->firstFAT));
    uint32_t swap1old = fs->firstFAT;
    uint32_t swap2old = (fs->firstFAT + 1) % fs->tableCount;
    uint32_t swap1new = (fs->firstFAT + 2) % fs->tableCount;
    uint32_t swap2new = (fs->firstFAT + 3) % fs->tableCount;
    fs->fat->swapCount++;
    // Refresh the FAT table and calculate crc
    memset(fs->fat->commit, 0xFF, sizeof(_commit) * NORFAT_CRC_COUNT);
    updateTableCrc(fs, 0);

    // Erase #1 old block
    NORFAT_TRACE(("commitChanges:Erase[%i]\r\n", swap1old));
    for (i = 0; i < fs->tableSectors; i++) {
      if (fs->erase_block_sector(
              fs->addressStart +
              (swap1old * (fs->sectorSize * fs->tableSectors)) +
              (i * fs->sectorSize))) {
        fs->lastError = NORFAT_ERR_IO;
        NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
        return NORFAT_ERR_IO;
      }
    }

    // Program #1 new block
    NORFAT_TRACE(("commitChanges:Program[%i]\r\n", swap1new));
    if (fs->program_block_page(
            fs->addressStart + (swap1new * (fs->sectorSize * fs->tableSectors)),
            (uint8_t *)fs->fat, (fs->sectorSize * fs->tableSectors))) {
      fs->lastError = NORFAT_ERR_IO;
      NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
      return NORFAT_ERR_IO;
    }
    // Erase #2 old block
    NORFAT_TRACE(("commitChanges:Erase[%i]\r\n", swap2old));
    for (i = 0; i < fs->tableSectors; i++) {
      if (fs->erase_block_sector(
              fs->addressStart +
              (swap2old * (fs->sectorSize * fs->tableSectors)) +
              (i * fs->sectorSize))) {
        fs->lastError = NORFAT_ERR_IO;
        NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
        return NORFAT_ERR_IO;
      }
    }
    // Program #2 new block
    NORFAT_TRACE(("commitChanges:Program[%i]\r\n", swap2new));
    if (fs->program_block_page(
            fs->addressStart + (swap2new * (fs->sectorSize * fs->tableSectors)),
            (uint8_t *)fs->fat, (fs->sectorSize * fs->tableSectors))) {
      fs->lastError = NORFAT_ERR_IO;
      NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
      return NORFAT_ERR_IO;
    }
    validateTable(fs, swap1new, &i);
    NORFAT_TRACE(("TESTCRC: 0x%X\r\n", calcTableCrc(fs, 0)));

    fs->firstFAT += 2;
    fs->firstFAT %= fs->tableCount;
    NORFAT_TRACE(("commitChanges:firstFat = %i\r\n", fs->firstFAT));
    NORFAT_DEBUG(("_FAT tables now at %i %i\n", fs->firstFAT,
                  ((fs->firstFAT + 1) % fs->tableCount)));
    return NORFAT_OK;
  }
  NORFAT_DEBUG(("Committing _FAT tables %i %i\r\n", fs->firstFAT,
                ((fs->firstFAT + 1) % fs->tableCount)));
  // Prep for write
  // CRC
  NORFAT_ASSERT(index < NORFAT_CRC_COUNT - 1);
  memset(&fs->fat->commit[index], 0, sizeof(_commit));
  updateTableCrc(fs, index + 1);

  memset(fs->buff, 0xFF, (fs->sectorSize * fs->tableSectors));
  uint8_t *fat = (uint8_t *)fs->fat;
  for (i = 0; i < (fs->sectorSize * fs->tableSectors); i++) {
    fs->buff[i] &= fat[i];
  }
  NORFAT_TRACE(("commitChanges:Program[%i]\r\n", fs->firstFAT));
  if (fs->program_block_page(
          fs->addressStart +
              (fs->firstFAT * (fs->sectorSize * fs->tableSectors)),
          fs->buff, (fs->sectorSize * fs->tableSectors))) {
    fs->lastError = NORFAT_ERR_IO;
    NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
    return NORFAT_ERR_IO;
  }
  NORFAT_TRACE(("commitChanges:Program[%i]\r\n", fs->firstFAT + 1));
  if (fs->program_block_page(fs->addressStart +
                                 (((fs->firstFAT + 1) % fs->tableCount) *
                                  (fs->sectorSize * fs->tableSectors)),
                             fs->buff, (fs->sectorSize * fs->tableSectors))) {
    fs->lastError = NORFAT_ERR_IO;
    NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
    return NORFAT_ERR_IO;
  }
  NORFAT_TRACE(("commitChanges:firstFat = %i\r\n", fs->firstFAT, index + 1));
  return NORFAT_OK;
}

int norfat_mount(norFAT_FS *fs) {
  int32_t i;
  uint32_t ui;
  int32_t empty = 1;
  uint32_t scenario;
  NORFAT_TRACE(("norfat_mount()..\r\n"));
  NORFAT_ASSERT(fs->erase_block_sector);
  NORFAT_ASSERT(fs->program_block_page);
  NORFAT_ASSERT(fs->read_block_device);
  NORFAT_ASSERT(fs->fat);
  NORFAT_ASSERT(fs->buff);
  // Configuration tests
  NORFAT_ASSERT(fs->programSize);
  NORFAT_ASSERT(sizeof(norFAT_fileHeader) < fs->programSize);
  NORFAT_ASSERT(fs->tableCount % 2 == 0); // Must be multiple of 2
  NORFAT_ASSERT(fs->tableCount <=
                NORFAT_MAX_TABLES); // We don't want to dynamically allocate
  NORFAT_TRACE(
      ("Table Bytes = 0x%X\r\n", NORFAT_TABLE_BYTES(fs->flashSectors)));
  NORFAT_ASSERT( // Assure that the total sectors fits in the configured sectors
      NORFAT_TABLE_BYTES(fs->flashSectors) < fs->tableSectors * fs->sectorSize);

  fs->lastError = NORFAT_OK;
  int32_t sectorState[NORFAT_MAX_TABLES];
  uint32_t sectorCRC[NORFAT_MAX_TABLES];
  /* Scan tables for valid records */
  for (i = 0; i < (int32_t)fs->tableCount; i++) {
    sectorState[i] = validateTable(fs, i, &sectorCRC[i]);
    if (sectorState[i] == NORFAT_TABLE_GOOD) {
      empty = 0;
    } else if (sectorState[i] == NORFAT_ERR_IO) {
      return NORFAT_ERR_IO;
    }
    // Check old state
    // If Both sectors are good but crc's don't match, the odd number
    // was programmed prior to the current operation, and is the older.
    if (i % 2 == 1) {
      if (sectorState[i] == NORFAT_TABLE_GOOD &&
          sectorState[i - 1] == NORFAT_TABLE_GOOD &&
          sectorCRC[i] != sectorCRC[i - 1]) {
        sectorState[i] = NORFAT_TABLE_OLD;
      }
    }
  }
  if (empty) {
    NORFAT_TRACE(("norfat_mount:Volume empty\r\n"));
    NORFAT_DEBUG(("Mounted volume is empty\r\n"));
    fs->lastError = NORFAT_ERR_EMPTY;
    return NORFAT_ERR_EMPTY;
  }

  /* There are 4 states, Good, old, empty, and bad
   * Tables are kept in mirrored copies in even pairs
   * Like 0-1, 2-3, etc. We never attempt to do any
   * repairs to a presently valid table.
   */
  uint32_t res;
  uint32_t tablesValid = 0;
  memset(fs->fat, 0, fs->tableSectors * fs->sectorSize);

  for (ui = 0; ui < fs->tableCount; ui += 2) {
    // Build a scenario
    // |N|N|N|N|
    scenario = sectorState[ui] << 12;
    scenario += sectorState[(ui + 1) % fs->tableCount] << 8;
    scenario += sectorState[(ui + 2) % fs->tableCount] << 4;
    scenario += sectorState[(ui + 3) % fs->tableCount] << 0;
#ifdef NORFAT_COVERAGE_TEST
    for (i = 0; i < 64; i++) {
      if (scenarioList[i] == scenario || scenarioList[i] == 0) {
        scenarioCnt[i]++;
        break;
      }
    }
    if (i != 64) {
      scenarioList[i] = scenario;
    }
#endif
    res = 0;
    switch (scenario) {
    case 0x0032: /* |GOOD |GOOD | BAD |EMPTY| */
      res = eraseTable(fs, (ui + 2) % fs->tableCount);
      if (res) {
        return res;
      }
      res = loadTable(fs, ui);
      if (res) {
        return res;
      }
      fs->firstFAT = ui;
      NORFAT_DEBUG(
          ("FAT tables %i %i loaded\r\n", ui, (ui + 1) % fs->tableCount));
      NORFAT_TRACE(("norfat_mount:0x%04X|ui %i\r\n", scenario, ui));
      tablesValid = 1;
      break;
    case 0x0033: /* |GOOD |GOOD | BAD | BAD | */
      res = eraseTable(fs, (ui + 2) % fs->tableCount);
      if (res) {
        return res;
      }
      res = eraseTable(fs, (ui + 3) % fs->tableCount);
      if (res) {
        return res;
      }
      res = loadTable(fs, ui);
      if (res) {
        return res;
      }
      fs->firstFAT = ui;
      NORFAT_DEBUG(
          ("FAT tables %i %i loaded\r\n", ui, (ui + 1) % fs->tableCount));
      NORFAT_TRACE(("norfat_mount:0x%04X|ui %i\r\n", scenario, ui));
      tablesValid = 1;
      break;
    case 0x0023: /* |GOOD |GOOD |EMPTY| BAD | */
      res = eraseTable(fs, (ui + 3) % fs->tableCount);
      if (res) {
        return res;
      }
      res = loadTable(fs, ui);
      if (res) {
        return res;
      }
      fs->firstFAT = ui;
      NORFAT_DEBUG(
          ("FAT tables %i %i loaded\r\n", ui, (ui + 1) % fs->tableCount));
      NORFAT_TRACE(("norfat_mount:0x%04X|ui %i\r\n", scenario, ui));
      tablesValid = 1;
      break;
    case 0x0022: /* |GOOD |GOOD |EMPTY|EMPTY| (ideal conditions) */
      res = loadTable(fs, ui);
      if (res) {
        return res;
      }
      fs->firstFAT = ui;
      NORFAT_DEBUG(
          ("FAT tables %i %i loaded\r\n", ui, (ui + 1) % fs->tableCount));
      NORFAT_TRACE(("norfat_mount:0x%04X|ui %i\r\n", scenario, ui));
      tablesValid = 1;
      break;
    case 0x3022: /* | BAD |GOOD |EMPTY|EMPTY| (Re write table) */
      res = loadTable(fs, ui + 1);
      if (res) {
        return res;
      }
      res = eraseTable(fs, ui);
      if (res) {
        return res;
      }
      fs->firstFAT = ui;
      res = copyTable(fs, ui, ui + 1);
      if (res) {
        return res;
      }
      // NORFAT_ASSERT(validateTable(fs, ui, NULL) == NORFAT_OK);//TODO: TEST
      // and remove
      NORFAT_DEBUG(
          ("FAT table %i rebuilt from %i\r\n", ui, (ui + 1) % fs->tableCount));
      NORFAT_TRACE(("norfat_mount:0x%04X|ui %i\r\n", scenario, ui));
      tablesValid = 1;
      break;
    case 0x2032: /* |EMPTY|GOOD | BAD | EMPTY|*/
      res = eraseTable(fs, ui + 2);
      if (res) {
        return res;
      }
      /* fallthrough */
    case 0x2022: /* |EMPTY|GOOD |EMPTY|EMPTY| */
      res = loadTable(fs, ui + 1);
      if (res) {
        return res;
      }
      fs->firstFAT = ui;
      res = copyTable(fs, ui, ui + 1);
      if (res) {
        return res;
      }
      // NORFAT_ASSERT(validateTable(fs, ui, NULL) == NORFAT_OK);//TODO: TEST
      // and remove
      NORFAT_DEBUG(
          ("FAT table %i rebuilt from %i\r\n", ui, (ui + 1) % fs->tableCount));
      NORFAT_TRACE(("norfat_mount:0x%04X|ui %i\r\n", scenario, ui));
      tablesValid = 1;
      break;
    case 0x2002: /* |EMPTY|GOOD |GOOD |EMPTY| */
                 // Delete the first good because its probably older
      /* fallthrough */
    case 0x2102: /* |EMPTY| OLD |GOOD |EMPTY| */
                 /* fallthrough */
    case 0x2302: /* |EMPTY| BAD |GOOD |EMPTY| */
      res = eraseTable(fs, ui + 1);
      if (res) {
        return res;
      }
      /* fallthrough */
    case 0x2202: /* |EMPTY|EMPTY|GOOD |EMPTY|*/
      res = loadTable(fs, ui + 2);
      if (res) {
        return res;
      }
      fs->firstFAT = (ui + 2) % fs->tableCount;
      res = copyTable(fs, ui + 3, ui + 2);
      if (res) {
        return res;
      }
      NORFAT_DEBUG(("FAT table %i updated from %i\r\n",
                    (ui + 3) % fs->tableCount, (ui + 2) % fs->tableCount));
      NORFAT_TRACE(("norfat_mount:0x%04X|ui %i\r\n", scenario, ui));
      tablesValid = 1;
      break;
    case 0x2201: /* |EMPTY|EMPTY|GOOD | OLD |*/
                 /* fallthrough */
    case 0x2203: /* |EMPTY|EMPTY|GOOD | BAD |*/
      res = eraseTable(fs, ui + 3);
      if (res) {
        return res;
      }
      res = loadTable(fs, ui + 2);
      if (res) {
        return res;
      }
      fs->firstFAT = (ui + 2) % fs->tableCount;
      res = copyTable(fs, ui + 3, ui + 2);
      if (res) {
        return res;
      }
      NORFAT_DEBUG(("FAT table %i updated from %i\r\n",
                    (ui + 3) % fs->tableCount, (ui + 2) % fs->tableCount));
      NORFAT_TRACE(("norfat_mount:0x%04X|ui %i\r\n", scenario, ui));
      tablesValid = 1;
      break;
      /* Inverse actions that mean we are on the wrong cog */
    case 0x2200:/* |EMPTY|EMPTY|GOOD |GOOD |*/
    case 0x2220:/* |EMPTY|EMPTY|EMPTY|GOOD |*/
    case 0x2230:/* |EMPTY|EMPTY| BAD |GOOD |*/
    case 0x3220:/* | BAD |EMPTY|EMPTY|GOOD |*/
    case 0x0220:/* |GOOD |EMPTY|EMPTY|GOOD |*/
    case 0x0221:/* |GOOD |EMPTY|EMPTY| OLD |*/
    case 0x0223:/* |GOOD |EMPTY|EMPTY| BAD |*/
    case 0x0222:/* |GOOD |EMPTY|EMPTY|EMPTY|*/
    case 0x0322:/* |GOOD | BAD |EMPTY|EMPTY|*/
    case 0x0122:/* |GOOD | OLD |EMPTY|EMPTY|*/
    case 0x2300:/* |EMPTY| BAD |GOOD |GOOD |*/
    case 0x3200:/* | BAD |EMPTY|GOOD |GOOD |*/
    case 0x3300:/* | BAD | BAD |GOOD |GOOD |*/
      NORFAT_DEBUG(("Inverse, no action on %04x\r\n", scenario));
      NORFAT_TRACE(("norfat_mount:_x%04X|ui %i\r\n", scenario, ui));
      break;
      /* wrong cog/ignored actions for > 4 tables*/
    case 0x2222:/* |EMPTY|EMPTY|EMPTY|EMPTY|*/
    case 0x2223:/* |EMPTY|EMPTY|EMPTY| BAD |*/
    case 0x2233:/* |EMPTY|EMPTY| BAD | BAD |*/
    case 0x3222:/* | BAD |EMPTY|EMPTY|EMPTY|*/
      NORFAT_TRACE(("norfat_mount:nx%04X|ui %i\r\n", scenario, ui));
      break;
    default:
      NORFAT_DEBUG(("No suitable action for %04x\r\n", scenario));
      NORFAT_TRACE(("norfat_mount:!x%04X|ui %i\r\n", scenario, ui));
      NORFAT_ASSERT(0);
      break;
    }
    if (tablesValid) {
      break;
    }
  }

  /* If there are any valid tables left, use them */
  /* Fallback, last ditch recovery effort */
  if (!tablesValid) {
    for (ui = 0; ui < fs->tableCount; ui += 2) {
      if (sectorState[ui] == NORFAT_TABLE_GOOD) {
        res = loadTable(fs, ui);
        if (res) {
          return res;
        }
        res = eraseTable(fs, ui + 1);
        if (res) {
          return res;
        }
        fs->firstFAT = ui;
        res = copyTable(fs, ui + 1, ui);
        if (res) {
          return res;
        }
        NORFAT_DEBUG(("FAT table %i updated from %i\r\n",
                      (ui + 1) % fs->tableCount, ui));
        NORFAT_TRACE(("norfat_mount:reco ui %i\r\n", ui));
        tablesValid = 1;
      } else if (sectorState[ui + 1] == NORFAT_TABLE_GOOD) {
        res = loadTable(fs, ui + 1);
        if (res) {
          return res;
        }
        res = eraseTable(fs, ui);
        if (res) {
          return res;
        }
        fs->firstFAT = ui + 1;
        res = copyTable(fs, ui, ui + 1);
        if (res) {
          return res;
        }
        NORFAT_DEBUG(("FAT table %i updated from %i\r\n", ui, ui + 1));
        NORFAT_TRACE(("norfat_mount:reco ui %i\r\n", ui + 1));
        tablesValid = 1;
      }
      if (tablesValid) {
        break;
      }
    }
  }

  if (!tablesValid) {
    for (ui = 0; ui < fs->tableCount; ui += 2) {
      if (sectorState[ui] == NORFAT_TABLE_OLD) {
        res = loadTable(fs, ui);
        if (res) {
          return res;
        }
        res = eraseTable(fs, ui + 1);
        if (res) {
          return res;
        }
        fs->firstFAT = ui;
        res = copyTable(fs, ui + 1, ui);
        if (res) {
          return res;
        }
        NORFAT_DEBUG(("FAT table %i updated from %i\r\n",
                      (ui + 1) % fs->tableCount, ui));
        NORFAT_TRACE(("norfat_mount:recold ui %i\r\n", ui));
        tablesValid = 1;
      } else if (sectorState[ui + 1] == NORFAT_TABLE_OLD) {
        res = loadTable(fs, ui + 1);
        if (res) {
          return res;
        }
        res = eraseTable(fs, ui);
        if (res) {
          return res;
        }
        fs->firstFAT = ui + 1;
        res = copyTable(fs, ui, ui + 1);
        if (res) {
          return res;
        }
        NORFAT_DEBUG(("FAT table %i updated from %i\r\n", ui, ui + 1));
        NORFAT_TRACE(("norfat_mount:recold ui %i\r\n", ui + 1));
        tablesValid = 1;
      }
      if (tablesValid) {
        break;
      }
    }
  }

  if (!tablesValid) {
    NORFAT_TRACE(("norfat_mount:No valid tables %i\r\n"));
    fs->lastError = NORFAT_ERR_CORRUPT;
    return NORFAT_ERR_CORRUPT;
  }
  /* scan for unclosed files */
  if (scanTable(fs, fs->fat)) {
    commitChanges(fs, 1);
    NORFAT_DEBUG(("Tables repaired\r\n"));
    NORFAT_TRACE(("norfat_mount:tables repaired\r\n"));
  }
  fs->volumeMounted = 1;
  NORFAT_TRACE(("norfat_mount:mounted\r\n"));
  NORFAT_INFO(("Volume is mounted\r\n"));
  return 0;
}

int norfat_format(norFAT_FS *fs) {
  uint32_t i, j;
  int32_t res;
  NORFAT_ASSERT(fs);
  NORFAT_TRACE(("norfat_format()\r\n"));
  for (i = 0; i < fs->tableCount; i++) {
    if (fs->read_block_device(fs->addressStart +
                                  (i * (fs->sectorSize * fs->tableSectors)),
                              fs->buff, (fs->sectorSize * fs->tableSectors))) {
      return NORFAT_ERR_IO;
    }
    for (j = 0; j < (fs->sectorSize * fs->tableSectors); j++) {
      if (fs->buff[j] != 0xFF) {
        break;
      }
    }
    if (j != (fs->sectorSize * fs->tableSectors)) {
      res = eraseTable(fs, i);
      if (res) {
        return res;
      }
    }
  }
  /* Build up an initial _FAT on the first two sectors */
  memset(fs->fat, 0xFF, (fs->sectorSize * fs->tableSectors));
  fs->fat->garbageCount = 0;
  fs->fat->swapCount = 0;
  updateTableCrc(fs, 0);
  if (fs->program_block_page(fs->addressStart, (uint8_t *)fs->fat,
                             (fs->sectorSize * fs->tableSectors))) {
    NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
    return NORFAT_ERR_IO;
  }
  if (fs->program_block_page(
          (fs->sectorSize * fs->tableSectors) + fs->addressStart,
          (uint8_t *)fs->fat, (fs->sectorSize * fs->tableSectors))) {
    NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
    return NORFAT_ERR_IO;
  }
  fs->firstFAT = 0;
  NORFAT_INFO(("Volume is formatted\r\n"));
  NORFAT_TRACE(("FORMAT:done\r\n"));
  return 0;
}

#ifdef NORFAT_COVERAGE_TEST

int norfat_fsMetaData(char * buff, int32_t maxLen) {
  uint32_t i = 0;
  int32_t j = 0;
  uint32_t len = 0;
  uint32_t spLen;
  char *val;
  spLen = NORFAT_INFO_SNPRINT((buff, maxLen,
                               "\r\nnorFAT Version %s\r\n"
                               "\r\nScenario list\r\n",
                               NORFAT_VERSION));
  maxLen -= spLen;
  buff += spLen;
  
  while (scenarioList[len]) {
    len++;
  }
  /* |GOOD |EMPTY|EMPTY| OLD |*/
  for (i = 0; i < len; i++) {
    spLen = NORFAT_INFO_SNPRINT(
        (buff, maxLen > 0 ? maxLen : 0, "[%02i] 0x%04X |", i, scenarioList[i]));
    maxLen -= spLen;
    buff += spLen;
    for (j = 3; j >= 0; j--) {
      uint8_t block = (scenarioList[i] >> (j * 4)) & 0xF;
      switch (block) {
      case 0:
        val = "GOOD |";
        break;
      case 1:
        val = " OLD |";
        break;
      case 2:
        val = "EMPTY|";
        break;
      case 3:
        val = " BAD |";
        break;
      }
      spLen = NORFAT_INFO_SNPRINT((buff, maxLen > 0 ? maxLen : 0, "%s", val));
      maxLen -= spLen;
      buff += spLen;
    }
    spLen = NORFAT_INFO_SNPRINT(
        (buff, maxLen > 0 ? maxLen : 0, "%i\r\n", scenarioCnt[i]));
    maxLen -= spLen;
    buff += spLen;
  }
}

#endif

int norfat_fsinfo(norFAT_FS *fs, char * buff, int32_t maxLen) {
  NORFAT_ASSERT(fs);
  NORFAT_ASSERT(fs->volumeMounted);
  char * pin = buff;
  uint32_t i, len;
  uint32_t bytesFree = 0;
  uint32_t bytesUsed = 0;
  uint32_t bytesUncollected = 0;
  uint32_t bytesAvailable = 0;
  uint32_t fileCount = 0;
  uint32_t tableOverhead = (fs->tableSectors * fs->tableCount * fs->sectorSize);
  norFAT_fileHeader f;
  struct tm ts;
  time_t now;
  char buf[32];
  len = NORFAT_INFO_SNPRINT((buff, maxLen,
      "\r\nnorFAT Version %s"
       "\r\nVolume info:Capacity %9i KiB\r\n",
       NORFAT_VERSION, 
       ((fs->flashSectors * fs->sectorSize) - tableOverhead) / 1024));
  maxLen -= len;
  buff += len;
  for (i = (fs->tableCount * fs->tableSectors); i < fs->flashSectors; i++) {
    if ((fs->fat->sector[i].base & NORFAT_SOF_MSK) == NORFAT_SOF_MATCH) {
      if (fs->read_block_device(fs->addressStart + (i * fs->sectorSize),
                                fs->buff, sizeof(norFAT_fileHeader))) {
        return NORFAT_ERR_IO;
      }

      memcpy(&f, fs->buff, sizeof(norFAT_fileHeader));
      now = (time_t)f.timeStamp;
      ts = *localtime(&now);
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ts);
      len = NORFAT_INFO_SNPRINT((buff, maxLen > 0 ? maxLen : 0, "%s  %9i %s\r\n",
                               buf, (int)f.fileLen % 1000, f.fileName));
      maxLen -= len;
      buff += len;
      bytesUsed += f.fileLen;
      fileCount++;
    } else if (fs->fat->sector[i].available) {
      bytesAvailable += fs->sectorSize;
      bytesFree += fs->sectorSize;
    } else if (!fs->fat->sector[i].active) {
      bytesUncollected += fs->sectorSize;
      bytesFree += fs->sectorSize;
    } else {
    }
  }

  buff += NORFAT_INFO_SNPRINT((buff, maxLen > 0 ? maxLen : 0,
      "     Files    %9i\r\n"
      "     Used     %9i\r\n"
      "     Free     %9i\r\n"
      "     Swaps    %9i\r\n"
      "     Garbage  %9i\r\n",
      fileCount,
      bytesUsed,
      bytesFree,
      fs->fat->swapCount,
      fs->fat->garbageCount));
  return (int)(buff - pin);
}

int norfat_fopen(norFAT_FS *fs, const char *filename, const char *mode,
                          norfat_FILE *file) {

  uint32_t sector;
  uint32_t flags;
  int retVal;
  NORFAT_ASSERT(fs);
  NORFAT_ASSERT(fs->volumeMounted);
  NORFAT_TRACE(("norfat_fopen(%s,%s)\r\n", filename, mode));
  /* Protect fs state */
  if (fs->lastError == NORFAT_ERR_IO) {
    return NORFAT_ERR_IO;
  }
  if (strcmp("r", mode) == 0) {
    flags = NORFAT_FLAG_READ;
#ifdef NORFAT_FILE_CHECK
    flags |= NORFAT_FILE_CRC_CHECK;
#endif
  } else if (strcmp("rb", mode) == 0) {
    flags = NORFAT_FLAG_READ;
#ifdef NORFAT_FILE_CHECK
    flags |= NORFAT_FILE_CRC_CHECK;
#endif
  } else if (strcmp("w", mode) == 0) {
    flags = NORFAT_FLAG_WRITE;
  } else if (strcmp("wb", mode) == 0) {
    flags = NORFAT_FLAG_WRITE;
  } else {
    fs->lastError = NORFAT_ERR_UNSUPPORTED;
    NORFAT_TRACE(("norfat_fopen:unsupported\r\n"));
    return NORFAT_ERR_UNSUPPORTED;
  }
  memset(file, 0, sizeof(norfat_FILE));
  retVal = fileSearch(fs, filename, &sector, &file->fh, NULL);
  if (flags & NORFAT_FLAG_READ) {
    if (retVal == NORFAT_OK) {
      // memset(file, 0, sizeof(norfat_FILE));
      // memcpy(&file->fh, &fs->fh, sizeof(norFAT_fileHeader));
      file->startSector = sector;
      file->currentSector = sector;
      file->rwPosInSector = fs->programSize;
      file->openFlags = flags;
      if (flags & NORFAT_FLAG_ZERO_COPY) {
        file->zeroCopy = 1;
      }
      file->crcValidate = 0xFFFFFFFF;
      NORFAT_TRACE(("norfat_fopen:file opened for reading\r\n"));
      NORFAT_DEBUG(("FILE %s opened for reading\r\n", filename));
      return NORFAT_OK;
    } else {
      if (fs->lastError == NORFAT_ERR_IO) {
        return NORFAT_ERR_IO;
      }
      NORFAT_TRACE(("NORFAT_ERR_FILE_NOT_FOUND\r\n"));
      fs->lastError = NORFAT_ERR_FILE_NOT_FOUND;
      return NORFAT_ERR_FILE_NOT_FOUND; // File not found
    }
  } else if (flags & NORFAT_FLAG_WRITE) {
    if (retVal == NORFAT_ERR_FILE_NOT_FOUND &&
        sector == NORFAT_INVALID_SECTOR &&
        fs->lastError == NORFAT_ERR_IO) {
      NORFAT_TRACE(("norfat_fopen:failed\r\n"));
      return NORFAT_ERR_IO;
    }
    // memset(file, 0, sizeof(norfat_FILE));
    file->oldFileSector = NORFAT_FILE_NOT_FOUND;
    file->startSector = NORFAT_INVALID_SECTOR;
    file->openFlags = flags;
    file->currentSector = -1;
    if (retVal == NORFAT_OK) { // File found
      // memcpy(&file->fh, &fs->fh, sizeof(norFAT_fileHeader));
      file->oldFileSector = sector; // Mark for removal
      NORFAT_ASSERT(sector >= fs->tableCount);
      NORFAT_DEBUG(("Sector %i marked for removal\r\n", sector));
      NORFAT_TRACE(("norfat_fopen:sector[%i] marked to remove\r\n", sector));
    } else {
      memset(&file->fh, 0, sizeof(norFAT_fileHeader));
      uint32_t nameLen = strlen(filename);
      nameLen =
          nameLen + 1 < NORFAT_MAX_FILENAME ? nameLen : NORFAT_MAX_FILENAME;
      strncpy(file->fh.fileName, filename, nameLen);
    }
    NORFAT_TRACE(("norfat_fopen:file opened for writing\r\n"));
    NORFAT_DEBUG(("FILE %s opened for writing\r\n", filename));
    return NORFAT_OK;
  }
  fs->lastError = NORFAT_ERR_UNSUPPORTED;
  NORFAT_TRACE(("norfat_fopen:unsupported fallthrough\r\n"));
  return NORFAT_ERR_UNSUPPORTED;
}

int norfat_fclose(norFAT_FS *fs, norfat_FILE *stream) {
  // Write header to page
  NORFAT_TRACE(("norfat_fclose()\r\n"));
  NORFAT_ASSERT(fs);
  NORFAT_ASSERT(fs->volumeMounted);
  NORFAT_ASSERT(stream);
  int32_t ret;
  uint32_t limit;
  uint32_t current;
  uint32_t next;
  /* Protect fs state */
  if (fs->lastError == NORFAT_ERR_IO) {
    ret = NORFAT_ERR_IO;
    goto finalize;
  }
  if (stream->error && stream->openFlags & NORFAT_FLAG_WRITE) {
    // invalidate the last
    if (stream->startSector != NORFAT_INVALID_SECTOR) {
      limit = fs->flashSectors;
      current = stream->startSector;
      next = fs->fat->sector[current].next;
      NORFAT_DEBUG(("..INVALID[%i]..%i.%i", stream->position, current, next));
      NORFAT_TRACE(("norfat_fclose:INVALID[%i]:%i.%i\r\n", stream->position,
                    current, next));
      while (1) {
        fs->fat->sector[current].base &= NORFAT_GARBAGE_MASK;
        if (next == NORFAT_EOF) {
          break;
        }
        if (next < fs->tableCount ||
            (next >= fs->flashSectors && next != NORFAT_EOF)) {
          NORFAT_ERROR(("Corrupt file system next = %i\r\n", next));
          NORFAT_TRACE(("NORFAT_ERR_CORRUPT next \r\n", next));
          ret = NORFAT_ERR_CORRUPT;
          goto finalize;
        }
        current = next;
        next = fs->fat->sector[next].next;
        NORFAT_DEBUG((".%i", next));
        NORFAT_TRACE((".%i", next));
        if (--limit < 1) {
          NORFAT_TRACE(("NORFAT_ERR_CORRUPT limit\r\n"));
          ret = NORFAT_ERR_CORRUPT;
          goto finalize;
        }
      }
      NORFAT_DEBUG((".\r\n"));
      NORFAT_TRACE((".\r\n", next));
    }
    ret = fs->lastError;
    goto finalize;
  }
  if (stream->openFlags & NORFAT_FLAG_WRITE &&
      stream->startSector != NORFAT_INVALID_SECTOR) {
    // Write the header
    memset(fs->buff, 0xFF, fs->programSize);
    stream->fh.fileLen = stream->position;
    stream->fh.timeStamp = time(NULL);
    memcpy(fs->buff, &stream->fh, sizeof(norFAT_fileHeader));

    if (fs->program_block_page(fs->addressStart +
                                   (stream->startSector * fs->sectorSize),
                               fs->buff, fs->programSize)) {
      ret = NORFAT_ERR_IO;
      NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
      goto finalize;
    }
    // Commit to _FAT table
    fs->fat->sector[stream->startSector].write = 0; // Set write inactive
    limit = fs->flashSectors;
    current = stream->startSector;
    next = fs->fat->sector[current].next;
    NORFAT_DEBUG(("..WRITE[%i]..%i.%i.", stream->position, current, next));
    NORFAT_TRACE(
        ("norfat_fclose:WRITE[%i]:%i.%i.", stream->position, current, next));
    while (1) {
      if (next == NORFAT_EOF) {
        break;
      }
      if (next < fs->tableCount ||
          (next >= fs->flashSectors && next != NORFAT_EOF)) {
        NORFAT_ERROR(("Corrupt file system next = %i\r\n", next));
        NORFAT_TRACE(("NORFAT_ERR_CORRUPT next \r\n", next));
        ret = NORFAT_ERR_CORRUPT;
        goto finalize;
      }
      fs->fat->sector[next].write = 0; // Set write inactive
      current = next;
      next = fs->fat->sector[next].next;
      NORFAT_DEBUG(("%i.", next));
      NORFAT_TRACE(("%i.", next));
      if (--limit < 1) {
        NORFAT_TRACE(("NORFAT_ERR_CORRUPT limit\r\n"));
        ret = NORFAT_ERR_CORRUPT;
        goto finalize;
      }
    }
    NORFAT_DEBUG(("\r\n"));
    NORFAT_TRACE(("\r\n"));
  }

  // Delete old file
  if (stream->openFlags & NORFAT_FLAG_WRITE &&
      stream->oldFileSector != NORFAT_FILE_NOT_FOUND) {

    limit = fs->flashSectors;
    current = stream->oldFileSector;
    next = fs->fat->sector[current].next;
    NORFAT_TRACE(("norfat_fclose:DELETE:%i.%i.", current, next));
    while (1) {
      fs->fat->sector[current].base &= NORFAT_GARBAGE_MASK; // Delete action
      if (next == NORFAT_EOF) {
        break;
      }
      if (next < fs->tableCount ||
          (next >= fs->flashSectors && next != NORFAT_EOF)) {
        NORFAT_ERROR(("Corrupt file system next = %i\r\n", next));
        NORFAT_TRACE(("NORFAT_ERR_CORRUPT next \r\n", next));
        ret = NORFAT_ERR_CORRUPT;
        goto finalize;
      }
      current = next;
      next = fs->fat->sector[next].next;
      NORFAT_TRACE(("%i.", next));
      if (--limit < 1) {
        NORFAT_TRACE(("NORFAT_ERR_CORRUPT limit\r\n"));
        ret = NORFAT_ERR_CORRUPT;
        goto finalize;
      }
    }
    NORFAT_TRACE(("\r\n"));
  }
  if (stream->openFlags & NORFAT_FLAG_WRITE) {
    ret = commitChanges(fs, 0);

    if (ret) {
      NORFAT_DEBUG(("FILE %s commit failed\r\n", stream->fh.fileName));
      NORFAT_TRACE(
          ("norfat_fclose(%s):commit failed\r\n", stream->fh.fileName));
    } else {
      NORFAT_DEBUG(("FILE %s committed\r\n", stream->fh.fileName));
      NORFAT_TRACE(("norfat_fclose(%s):committed\r\n", stream->fh.fileName));
    }
  } else {
    ret = NORFAT_OK;
  }
finalize:
  NORFAT_DEBUG(("FILE %s closed\r\n", stream->fh.fileName));
  NORFAT_TRACE(("norfat_fclose(%s):finalize\r\n", stream->fh.fileName));
  return ret;
}

size_t norfat_fwrite(norFAT_FS *fs, const void *ptr, size_t size, size_t count,
                     norfat_FILE *stream) {
  int32_t nextSector;
  uint32_t writeable;
  uint32_t blockWriteLength;
  uint32_t blockAddress;
  uint32_t DataLengthToWrite;
  uint32_t offset;
  uint8_t *out = (uint8_t *)ptr;
  uint32_t len = size * count;
  NORFAT_TRACE(("norfat_fwrite(%i)\r\n", len));
  NORFAT_ASSERT(fs);
  NORFAT_ASSERT(fs->volumeMounted);
  NORFAT_ASSERT(stream);
  NORFAT_ASSERT(stream->openFlags & NORFAT_FLAG_WRITE);
  NORFAT_ASSERT(size * count > 0);
  /* Protect fs state */
  if (fs->lastError == NORFAT_ERR_IO) {
    return 0;
  }
  if (stream->currentSector == -1) {
    stream->currentSector = findEmptySector(fs);
    if (stream->currentSector == NORFAT_ERR_FULL) {
      stream->error = 1;
      stream->lastError = NORFAT_ERR_FULL;
      NORFAT_TRACE(("norfat_fwrite:NORFAT_ERR_FULL\r\n"));
      return NORFAT_ERR_FULL;
    } else if (stream->currentSector == NORFAT_ERR_IO) {
      stream->error = 1;
      fs->lastError = stream->lastError = NORFAT_ERR_IO;
      NORFAT_TRACE(("norfat_fwrite:NORFAT_ERR_IO\r\n"));
      return NORFAT_ERR_IO;
    }
    if (fs->erase_block_sector(fs->addressStart +
                               (fs->sectorSize * stream->currentSector))) {
      NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
      fs->lastError = stream->lastError = NORFAT_ERR_IO;
      return NORFAT_ERR_IO;
    }
    NORFAT_DEBUG(("New file sector %i\r\n", stream->currentSector));
    NORFAT_TRACE(("norfat_fwrite:add sector[%i]\r\n", stream->currentSector));
    // New file
    stream->startSector = stream->currentSector;
    stream->rwPosInSector = fs->programSize;
    stream->fh.crc = 0xFFFFFFFF;
    NORFAT_ASSERT(stream->startSector >= fs->tableCount &&
                  stream->startSector != NORFAT_INVALID_SECTOR);
  }
  // At this point we should have a writeable area
  while (len) {
    // Calculate available space to write in this sector
    writeable = fs->sectorSize - stream->rwPosInSector;
    if (writeable == 0) {
      nextSector = findEmptySector(fs);
      if (nextSector == NORFAT_ERR_FULL) {
        stream->error = 1; // Flag for fclose delete
        stream->lastError = NORFAT_ERR_FULL;
        NORFAT_TRACE(("norfat_fwrite:NORFAT_ERR_FULL\r\n"));
        return NORFAT_ERR_FULL;
      } else if (nextSector == NORFAT_ERR_IO) {
        stream->error = 1; // Flag for fclose delete
        fs->lastError = stream->lastError = NORFAT_ERR_IO;
        NORFAT_TRACE(("norfat_fwrite:NORFAT_ERR_IO\r\n"));
        return NORFAT_ERR_IO;
      }
      if (fs->erase_block_sector(fs->addressStart +
                                 (fs->sectorSize * nextSector))) {
        NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
        fs->lastError = stream->lastError = NORFAT_ERR_IO;
        return NORFAT_ERR_IO;
      }
      NORFAT_TRACE(("norfat_fwrite:add sector[%i]->[%i]\r\n",
                    stream->currentSector, nextSector));
      NORFAT_DEBUG(("File sector added %i -> %i\r\n", stream->currentSector,
                    nextSector));
      fs->fat->sector[stream->currentSector].next = nextSector;
      fs->fat->sector[nextSector].sof = 0;
      stream->currentSector = nextSector;
      writeable = fs->sectorSize;
      stream->rwPosInSector = 0;
    }
    // Calculate starting offset
    blockWriteLength = 0;
    offset = stream->rwPosInSector % fs->programSize;
    blockAddress = (stream->currentSector * fs->sectorSize) +
                   (stream->rwPosInSector - offset);
    // buf = fs->buff;
    if (offset) {
      memset(fs->buff, 0xFF, offset);
      blockWriteLength += offset;
    }
    DataLengthToWrite = len > writeable ? writeable : len;
    memcpy(&fs->buff[blockWriteLength], out, DataLengthToWrite);
    blockWriteLength += DataLengthToWrite;
    if (blockWriteLength % fs->programSize) {
      // Fill remaining page with 0xFF
      uint32_t fill = fs->programSize - (blockWriteLength % fs->programSize);
      memset(&fs->buff[blockWriteLength], 0xFF, fill);
      blockWriteLength += fill;
    }

    NORFAT_ASSERT((blockAddress % fs->sectorSize) + blockWriteLength <=
                  fs->sectorSize);
    if (fs->program_block_page(fs->addressStart + blockAddress, fs->buff,
                               blockWriteLength)) {
      NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
      fs->lastError = stream->lastError = NORFAT_ERR_IO;
      return NORFAT_ERR_IO;
    }
    stream->fh.crc = NORFAT_CRC(out, DataLengthToWrite, stream->fh.crc);
    stream->position += DataLengthToWrite;
    stream->rwPosInSector += DataLengthToWrite;
    out += DataLengthToWrite;
    len -= DataLengthToWrite;
  }
  NORFAT_TRACE(("norfat_fwrite:wrote %i\r\n", (size * count)));
  return (size * count);
}

size_t norfat_fread(norFAT_FS *fs, void *ptr, size_t size, size_t count,
                    norfat_FILE *stream) {
  NORFAT_ASSERT(fs);
  NORFAT_ASSERT(fs->volumeMounted);
  NORFAT_ASSERT(stream);
  NORFAT_ASSERT(stream->openFlags & NORFAT_FLAG_READ);
  uint32_t next;
  uint32_t readable;
  uint32_t remaining;
  uint32_t rlen;
  uint32_t rawAdr;
  int32_t readCount = 0;
  uint8_t *in = (uint8_t *)ptr;
  uint32_t len = size * count;
  NORFAT_TRACE(("norfat_fread(%i)\r\n", len));
  /* Protect fs state */
  if (fs->lastError == NORFAT_ERR_IO) {
    return 0;
  }
  while (len) {
    readable = fs->sectorSize - stream->rwPosInSector;
    remaining = stream->fh.fileLen - stream->position;
    if (remaining == 0) {
      break;
      // return readCount;
    }
    if (readable == 0) {
      next = fs->fat->sector[stream->currentSector].next;
      NORFAT_TRACE(("norfat_fread:next sector[%i]\r\n", next));
      if (next == NORFAT_EOF) {
        break;
        /* if (stream->openFlags & NORFAT_FILE_CRC_CHECK) {
          if (stream->crcValidate != stream->fh.crc) {
            return NORFAT_ERR_FILECRC;
          }
        }
        return readCount; */
      }
      stream->rwPosInSector = 0;
      stream->currentSector = next;
      readable = fs->sectorSize;
    }

    rlen = len > readable ? readable : len;
    rlen = rlen > remaining ? remaining : rlen;
    // TODO: only allow reads up to real len
    // uint32_t rawlen = wlen;
    rawAdr = (stream->currentSector * fs->sectorSize) + stream->rwPosInSector;
    if (stream->zeroCopy) {
      // Requires user implemented cache free operation
      if (fs->read_block_device(fs->addressStart + rawAdr, in, rlen)) {
        fs->lastError = stream->lastError = NORFAT_ERR_IO;
        NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
        return 0;
      }
    } else {
      if (fs->read_block_device(fs->addressStart + rawAdr, fs->buff, rlen)) {
        fs->lastError = stream->lastError = NORFAT_ERR_IO;
        NORFAT_TRACE(("NORFAT_ERR_IO\r\n"));
        return 0;
      }
      memcpy(in, fs->buff, rlen);
    }
    if (stream->openFlags & NORFAT_FILE_CRC_CHECK) {
      stream->crcValidate = NORFAT_CRC(in, rlen, stream->crcValidate);
    }
    stream->position += rlen;
    stream->rwPosInSector += rlen;
    in += rlen;
    len -= rlen;
    readCount += rlen;
  }
  if (stream->openFlags & NORFAT_FILE_CRC_CHECK &&
      stream->position == stream->fh.fileLen) {
    if (stream->crcValidate != stream->fh.crc) {
      return NORFAT_ERR_FILECRC;
    }
  }
  NORFAT_TRACE(("norfat_fread:read %i\r\n", readCount));
  return readCount;
}

int norfat_remove(norFAT_FS *fs, const char *filename) {
  uint32_t sector;
  int ret = NORFAT_OK;
  uint32_t limit, current, next;
  NORFAT_ASSERT(fs);
  NORFAT_ASSERT(fs->volumeMounted);
  NORFAT_TRACE(("norfat_remove(%s)\r\n", filename));
  /* Protect fs state */
  if (fs->lastError == NORFAT_ERR_IO) {
    return NORFAT_ERR_IO;
  }
  ret = fileSearch(fs, filename, &sector, NULL, NULL);
  if (ret == NORFAT_ERR_FILE_NOT_FOUND) {
    return NORFAT_OK;
  }

  limit = fs->flashSectors;
  current = sector;
  next = fs->fat->sector[current].next;
  NORFAT_TRACE(("norfat_remove:DELETE:%i.%i.", current, next));
  while (1) {
    fs->fat->sector[current].base &= NORFAT_GARBAGE_MASK; // Delete action
    if (next == NORFAT_EOF) {
      break;
    }
    if (next < fs->tableCount ||
        (next >= fs->flashSectors && next != NORFAT_EOF)) {
      NORFAT_ERROR(("Corrupt file system next = %i\r\n", next));
      NORFAT_TRACE(("NORFAT_ERR_CORRUPT\r\n", next));
      fs->lastError = NORFAT_ERR_CORRUPT;
      ret = NORFAT_ERR_CORRUPT;
      goto finalize;
    }
    current = next;
    next = fs->fat->sector[next].next;
    NORFAT_TRACE(("%i.", next));
    if (--limit < 1) {
      NORFAT_TRACE(("NORFAT_ERR_CORRUPT limit\r\n"));
      fs->lastError = NORFAT_ERR_CORRUPT;
      ret = NORFAT_ERR_CORRUPT;
      goto finalize;
    }
  }
  NORFAT_TRACE(("\r\n", next));
  ret = commitChanges(fs, 0);
  NORFAT_TRACE(("norfat_remove:committed\r\n"));
  NORFAT_DEBUG(("FILE %s delete\r\n", filename));
finalize:
  NORFAT_TRACE(("norfat_remove:finalize\r\n"));
  return ret;
}

int norfat_exists(norFAT_FS *fs, const char *filename) {
  uint32_t sector;
  uint32_t fLen;
  int ret = 0;
  NORFAT_ASSERT(fs);
  NORFAT_ASSERT(fs->volumeMounted);
  NORFAT_TRACE(("norfat_exists(%s)\r\n", filename));
  /* Protect fs state */
  if (fs->lastError == NORFAT_ERR_IO) {
    return NORFAT_ERR_IO;
  }
  ret = fileSearch(fs, filename, &sector, NULL, &fLen);

  if (ret == NORFAT_OK) {
    ret = fLen;
  } else {
    if (fs->lastError == NORFAT_ERR_IO) {
      return NORFAT_ERR_IO;
    }
  }
  return ret == NORFAT_ERR_FILE_NOT_FOUND ? 0 : ret;
}

int norfat_ferror(norfat_FILE *file) {
  return file->error;
}

int norfat_errno(norFAT_FS *fs) { return fs->lastError; }

size_t norfat_flength(norfat_FILE *f) {
  NORFAT_ASSERT(f);
  if (f == NULL) {
    return 0;
  }
  return f->fh.fileLen;
}