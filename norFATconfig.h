#ifndef NORFAT_CONFIG_H
#define NORFAT_CONFIG_H

#include <assert.h>

#define NORFAT_SECTORS		    1024

#define NORFAT_CRC_COUNT	    255	
#define NORFAT_SECTOR_SIZE	    4096

#define NORFAT_MAX_FILENAME     64

#define NORFAT_DEBUG(x) //printf x
#define NORFAT_INFO(x) //printf x
#define NORFAT_ERROR(x) //printf x
#define NORFAT_INFO_PRINT(x) printf x
//#define NORFAT_VERBOSE(x) //printf x

int traceHandler(const char* format, ...);
#define NORFAT_TRACE(x) traceHandler x

#define NORFAT_MALLOC(x) malloc(x)
#define NORFAT_FREE(x) free(x)
#define NORFAT_RAND rand

void assertHandler(char* file, int line);
#define NORFAT_ASSERT(expr) \
 if (!(expr)) \
        assertHandler(__FILE__, __LINE__)

#endif
