#ifndef NORFAT_CONFIG_H
#define NORFAT_CONFIG_H

#include <assert.h>

#define NORFAT_CRC_COUNT 255

#define NORFAT_MAX_FILENAME 64
#define NORFAT_FILE_CHECK
#define NORFAT_COVERAGE_TEST

#define NORFAT_DEBUG(x) // printf x
#define NORFAT_ERROR(x) // printf x
#define NORFAT_INFO_PRINT(x) printf x

int traceHandler(const char *format, ...);
#define NORFAT_TRACE(x) traceHandler x

#define NORFAT_RAND rand

void assertHandler(char *file, int line);
#define NORFAT_ASSERT(expr)                                                    \
  if (!(expr))                                                                 \
  assertHandler(__FILE__, __LINE__)

#endif
