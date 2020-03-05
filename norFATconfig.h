#ifndef NORFAT_CONFIG_H
#define NORFAT_CONFIG_H

#include <assert.h>

#define NORFAT_TABLE_COUNT	4
#define NORFAT_SECTORS		1024
#define NORFAT_CRC_COUNT	 255	
#define NORFAT_SECTOR_SIZE	4096
#define NORFAT_PAGE_SIZE	256
#define NORFAT_MAX_FILENAME   32

#define NORFAT_ASSERT(x) assert(x)

#define NORFAT_DEBUG(x) //printf x
#define NORFAT_INFO(x) //printf x
#define NORFAT_ERROR(x) //printf x
#define NORFAT_VERBOSE(x) //printf x

#define NORFAT_MALLOC(x) malloc(x)
#define NORFAT_FREE(x) free(x)


#endif
