#define _GNU_SOURCE
#include "hook.h"

#include <elf.h>
#include <link.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
    uintptr_t address;
    size_t length;
    int need_exec;
    int need_read;
    int found;
    int prot;
} range_probe_t;

static int range_probe_callback(struct dl_phdr_info *info, size_t size, void *opaque) {
    range_probe_t *p = (range_probe_t *)opaque;
    size_t i;
    uintptr_t a = p->address;
    uintptr_t end;
    (void)size;

    if (!a || (p->length && a > UINTPTR_MAX - p->length)) return 0;
    end = a + p->length;

    for (i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t start, finish;
        int prot = 0;
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        start = (uintptr_t)info->dlpi_addr + (uintptr_t)ph->p_vaddr;
        if ((uintptr_t)ph->p_memsz > UINTPTR_MAX - start) continue;
        finish = start + (uintptr_t)ph->p_memsz;
        if (a < start || end > finish) continue;
        if (ph->p_flags & PF_R) prot |= PROT_READ;
        if (ph->p_flags & PF_W) prot |= PROT_WRITE;
        if (ph->p_flags & PF_X) prot |= PROT_EXEC;
        if (p->need_exec && !(prot & PROT_EXEC)) continue;
        if (p->need_read && !(prot & PROT_READ)) continue;
        p->prot = prot;
        p->found = 1;
        return 1;
    }
    return 0;
}

int qldr_query_protection(void *address) {
    range_probe_t p;
    memset(&p, 0, sizeof(p));
    p.address = (uintptr_t)address;
    p.length = 1;
    if (!address) return -1;
    return dl_iterate_phdr(range_probe_callback, &p) && p.found ? p.prot : -1;
}

int qldr_range_readable(const void *address, size_t len) {
    range_probe_t p;
    memset(&p, 0, sizeof(p));
    p.address = (uintptr_t)address;
    p.length = len;
    p.need_read = 1;
    if (!address && len) return 0;
    if (len == 0) return 1;
    (void)dl_iterate_phdr(range_probe_callback, &p);
    return p.found;
}

int qldr_address_executable(const void *address) {
    range_probe_t p;
    memset(&p, 0, sizeof(p));
    p.address = (uintptr_t)address;
    p.length = 1;
    p.need_exec = 1;
    if (!address) return 0;
    (void)dl_iterate_phdr(range_probe_callback, &p);
    return p.found;
}

static uintptr_t page_start(uintptr_t p) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) return p;
    return p & ~((uintptr_t)ps - 1u);
}

static void emit_abs_jump14(uint8_t *dst, const void *target) {
    uintptr_t address = (uintptr_t)target;
    dst[0] = 0xff;
    dst[1] = 0x25;
    dst[2] = 0x00;
    dst[3] = 0x00;
    dst[4] = 0x00;
    dst[5] = 0x00;
    memcpy(dst + 6, &address, sizeof(address));
}

int qldr_hook_abs14(void *target, void *detour, size_t patch_len,
                    void **trampoline_out) {
    uintptr_t t = (uintptr_t)target;
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t first_page;
    uintptr_t last_page;
    size_t protected_len;
    size_t trampoline_size;
    uint8_t *trampoline;
    int original_prot;

    if (!target || !detour || !trampoline_out || patch_len < 14 || ps <= 0)
        return 0;
    if (!qldr_range_readable(target, patch_len) ||
        !qldr_address_executable(target))
        return 0;

    original_prot = qldr_query_protection(target);
    if (original_prot < 0) return 0;

    first_page = page_start(t);
    last_page = page_start(t + patch_len - 1u);
    if (last_page < first_page || first_page != last_page) return 0;
    protected_len = (size_t)(last_page - first_page) + (size_t)ps;

    trampoline_size = patch_len + 14;
    trampoline = mmap(NULL, trampoline_size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) return 0;

    memcpy(trampoline, target, patch_len);
    emit_abs_jump14(trampoline + patch_len, (const void *)(t + patch_len));

    if (mprotect((void *)first_page, protected_len,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        munmap(trampoline, trampoline_size);
        return 0;
    }

    emit_abs_jump14((uint8_t *)target, detour);
    if (patch_len > 14)
        memset((uint8_t *)target + 14, 0x90, patch_len - 14);
    __builtin___clear_cache((char *)target, (char *)target + patch_len);

    (void)mprotect((void *)first_page, protected_len, original_prot);
    if (mprotect(trampoline, trampoline_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(trampoline, trampoline_size);
        return 0;
    }

    *trampoline_out = trampoline;
    return 1;
}
int qldr_unhook_abs14(void *target, const void *original, size_t patch_len) {
    uintptr_t t = (uintptr_t)target;
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page;
    int original_prot;

    if (!target || !original || patch_len < 14 || ps <= 0) return 0;
    page = page_start(t);
    if (page != page_start(t + patch_len - 1u)) return 0;
    original_prot = qldr_query_protection(target);
    if (original_prot < 0) return 0;
    if (mprotect((void *)page, (size_t)ps,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return 0;
    memcpy(target, original, patch_len);
    __builtin___clear_cache((char *)target, (char *)target + patch_len);
    (void)mprotect((void *)page, (size_t)ps, original_prot);
    return 1;
}
