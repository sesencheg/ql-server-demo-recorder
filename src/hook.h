#ifndef QLDR_HOOK_H
#define QLDR_HOOK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Installs an x86-64 absolute jump at target. patch_len is the number of
 * complete instructions replaced and must be >= 14. The trampoline contains
 * those original bytes followed by a jump to target + patch_len.
 */
int qldr_hook_abs14(void *target, void *detour, size_t patch_len,
                    void **trampoline_out);
int qldr_unhook_abs14(void *target, const void *original, size_t patch_len);

/* Memory / mapping checks used only during initialisation and map discovery. */
int qldr_query_protection(void *address);
int qldr_range_readable(const void *address, size_t len);
int qldr_address_executable(const void *address);

#ifdef __cplusplus
}
#endif

#endif
