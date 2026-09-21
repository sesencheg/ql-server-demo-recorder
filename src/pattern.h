#ifndef QLDR_PATTERN_H
#define QLDR_PATTERN_H
#include <stddef.h>
#include <stdint.h>
void *qldr_pattern_search(const void *base, size_t size,
                          const uint8_t *pattern, const char *mask,
                          size_t pattern_len, int *match_count);
#endif
