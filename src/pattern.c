#define _GNU_SOURCE
#include "pattern.h"
#include "hook.h"

#include <elf.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

static void *pattern_search_range(const uint8_t *base, size_t size,
                                  const uint8_t *pat, const char *mask,
                                  size_t plen, int *count) {
    size_t i, j;
    void *first = NULL;
    int hits = 0;
    if (!base || !pat || !mask || plen == 0 || size < plen) {
        if (count) *count = 0;
        return NULL;
    }
    for (i = 0; i + plen <= size; ++i) {
        int ok = 1;
        for (j = 0; j < plen; ++j) {
            if (mask[j] == 'X' && base[i + j] != pat[j]) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            ++hits;
            if (!first) first = (void *)(base + i);
        }
    }
    if (count) *count = hits;
    return first;
}

void *qldr_pattern_search(const void *base, size_t size,
                          const uint8_t *pattern, const char *mask,
                          size_t pattern_len, int *match_count) {
    return pattern_search_range((const uint8_t *)base, size,
                                 pattern, mask, pattern_len, match_count);
}
