#define _GNU_SOURCE
#define __STDC_FORMAT_MACROS

#include "ql_demo_recorder.h"
#include "hook.h"
#include "pattern.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>

/* Bind dlvsym to its long-standing GLIBC_2.2.5 symbol version so the
 * recorder does not acquire the GLIBC_2.34-only default symbol version. */
extern void *qldr_dlvsym_old(void *, const char *, const char *);
__asm__(".symver qldr_dlvsym_old,dlvsym@GLIBC_2.2.5");
#include <errno.h>
#include <elf.h>
#include <link.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#ifndef MDR_MAX_CLIENTS
#define MDR_MAX_CLIENTS 64
#endif
#ifndef MAX_MSGLEN
#define MAX_MSGLEN 16384
#endif

/* ------------------------------------------------------------------------- */
/* QLDS 1069 ABI facts.                                                     */
/* ------------------------------------------------------------------------- */

typedef unsigned char byte;

typedef struct {
    uint8_t allowoverflow;
    uint8_t overflowed;
    uint8_t oob;
    uint8_t _pad[5];
    byte *data;
    int maxsize;
    int cursize;
    int readcount;
    int bit;
} ql1069_msg_t;

_Static_assert(offsetof(ql1069_msg_t, data) == 0x08, "bad ql1069 msg layout");
_Static_assert(offsetof(ql1069_msg_t, cursize) == 0x14, "bad ql1069 msg layout");
_Static_assert(sizeof(ql1069_msg_t) == 0x20, "bad ql1069 msg size");

/* These are image-relative VAs from the supplied QLDS 1069 build. */
#define QL1069_MSG_WRITE_BYTE_VA          ((uintptr_t)0x430200)
#define QL1069_SV_SENDCLIENTGAMESTATE_VA  ((uintptr_t)0x43a310)
#define QL1069_SYS_SETMODULEOFFSET_VA     ((uintptr_t)0x452950)

/* client_t offsets, QLDS 1069 x64. */
#define QL1069_CLIENT_STATE_OFFSET                 0x00000000u
#define QL1069_CLIENT_RELIABLE_SEQUENCE_OFFSET    0x00010404u
#define QL1069_CLIENT_RELIABLE_ACKNOWLEDGE_OFFSET 0x00010408u
#define QL1069_CLIENT_MESSAGE_ACK_OFFSET          0x00010410u
#define QL1069_CLIENT_GAMESTATE_SEQUENCE_OFFSET   0x00010414u
#define QL1069_CLIENT_LAST_COMMAND_OFFSET         0x0001043cu
#define QL1069_CLIENT_GENTITY_OFFSET              0x00010840u
#define QL1069_CLIENT_NAME_OFFSET                 0x00010848u
#define QL1069_CLIENT_DELTA_MESSAGE_OFFSET        0x000118b4u
#define QL1069_CLIENT_OUTGOING_SEQUENCE_OFFSET    0x00016a0cu
#define QL1069_CLIENT_STEAM_ID_OFFSET             0x00026a6cu
#define QL1069_CLIENT_NETCHAN_REMOTE_TYPE_OFFSET 0x00015a60u
#define QL1069_CLIENT_SIZE                       0x00026a74u

/* level_locals_t offsets from the supplied QLDS/minqlx 1069 layout. */
#define QL1069_LEVEL_WARMUP_TIME_OFFSET            0x18u
#define QL1069_LEVEL_TIME_OFFSET                   0x24u
#define QL1069_LEVEL_START_TIME_OFFSET             0x2cu
#define QL1069_LEVEL_NUM_PLAYING_CLIENTS_OFFSET   0x5cu
#define QL1069_LEVEL_SORTED_CLIENTS_OFFSET        0x6cu
#define QL1069_LEVEL_INTERMISSION_QUEUED_OFFSET   0x1d9cu
#define QL1069_LEVEL_INTERMISSION_TIME_OFFSET     0x1da0u
#define QL1069_LEVEL_EXIT_TIME_OFFSET             0x1dacu
#define QL1069_LEVEL_ROUND_CURRENT_OFFSET         0x2930u
#define QL1069_LEVEL_MATCH_FORFEITED_OFFSET       0x2954u

#define QL_CS_PRIMED  3
#define QL_CS_ACTIVE  4
#define QL_SVC_EOF    8
#define QL_SVF_BOT     0x00000008u
#define QL_GENTITY_R_SVFLAGS_OFFSET 0x000001e0u

#define QL_ROUND_PREGAME 0
#define QL_ROUND_WARMUP  1
#define QL_ROUND_SHUFFLE 2
#define QL_ROUND_BEGUN   3
#define QL_ROUND_OVER    4
#define QL_ROUND_POSTGAME 5

#define QL_MAX_SAFE_NAME 96

/* Exact signatures used by the supplied patch / QLDS build. */
typedef void (*ql_msg_write_byte_fn)(void *msg, int c);
typedef void (*ql_send_gamestate_fn)(void *client);
typedef void (*ql_send_message_fn)(void *msg, void *client);
typedef void (*ql_send_snapshot_fn)(void *client);

/* ------------------------------------------------------------------------- */
/* Recorder state.                                                           */
/* ------------------------------------------------------------------------- */

typedef enum {
    MDR_IDLE = 0,
    MDR_WAIT_FIRST_FULL,
    MDR_WAIT_SECOND_FULL,
    MDR_RECORDING
} mdr_state_t;

typedef struct {
    void *client;
    FILE *file;
    mdr_state_t state;
    int active;
    int client_num;
    uint64_t steam_id;

    int32_t gamestate_sequence;
    int32_t first_full_sequence;
    int gamestate_valid;
    int gamestate_size;
    int gamestate_written;
    int gamestate_bit;
    byte gamestate[MAX_MSGLEN];

    char path[PATH_MAX];
    char final_path[PATH_MAX];
    char temp_path[PATH_MAX];
    char pov_name[QL_MAX_SAFE_NAME];
    char opponent_name[QL_MAX_SAFE_NAME];
    char map_name[QL_MAX_SAFE_NAME];
    int opponent_client_num;
    time_t started_wall_time;
    int32_t saved_delta_message;
    int delta_forced;
    int has_demo_frame;
    int writer_closing;
    int writer_closed;
    int writer_error;
    uint64_t generation;

    int stop_pending;
    int32_t stop_server_time;
    int stop_due_reached;
    int32_t stop_due_frame;
    int snapshot_after_due;
} server_demo_recorder_t;

static server_demo_recorder_t g_rec[MDR_MAX_CLIENTS];
static ql_msg_write_byte_fn g_msg_write_byte;
static ql_send_gamestate_fn g_send_gamestate;
static ql_send_message_fn g_send_message;
static ql_send_snapshot_fn g_send_snapshot;
typedef void (*ql_set_module_offset_fn)(char *moduleName, void *offset);
static ql_set_module_offset_fn g_set_module_offset;
static uintptr_t g_set_module_target;
static uint8_t g_set_module_saved[14];
static int g_set_module_reentry;

static uintptr_t g_main_base;
static int g_initialized;
static int g_enabled;
static int g_debug;
static int g_stop_delay_ms = 4050;
static uint64_t g_file_serial;
static char g_output_path[PATH_MAX] = "demos";

static uintptr_t g_level;
static uintptr_t g_ginitgame;
static uintptr_t g_vm_call_table;
static int32_t g_match_start_time = INT32_MIN;
static int g_countdown_seen;
static int g_match_started;
static int g_postgame_scheduled;
static int g_last_status = -1;
static int g_warned_no_level = 0;
static int g_snapshot_hook_seen;
static int g_message_hook_seen;
static int32_t g_last_lifecycle_time = INT32_MIN;
static void *g_seen_clients[MDR_MAX_CLIENTS];

/* Dedicated writer thread: disk I/O never runs in the QLDS snapshot thread. */
typedef enum {
    WRITER_JOB_RECORD = 1,
    WRITER_JOB_CLOSE = 2
} writer_job_type_t;

typedef struct writer_job_s {
    writer_job_type_t type;
    server_demo_recorder_t *r;
    uint64_t generation;
    int32_t sequence;
    int len;
    byte *data;
    int delete_file;
    struct writer_job_s *next;
} writer_job_t;

static pthread_t g_writer_thread;
static pthread_mutex_t g_writer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_writer_nonempty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_writer_space = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_writer_done = PTHREAD_COND_INITIALIZER;
static writer_job_t *g_writer_head;
static writer_job_t *g_writer_tail;
static unsigned g_writer_depth;
static int g_writer_stop;
static int g_writer_started;
#define WRITER_MAX_DEPTH 512u

typedef int (*ql_pthread_create_fn)(pthread_t *, const pthread_attr_t *,
                                    void *(*)(void *), void *);
typedef int (*ql_pthread_join_fn)(pthread_t, void **);
static ql_pthread_create_fn g_pthread_create;
static ql_pthread_join_fn g_pthread_join;

static int resolve_pthread_runtime(void) {
    if (!g_pthread_create) {
        *(void **)(&g_pthread_create) = qldr_dlvsym_old(RTLD_DEFAULT, "pthread_create", "GLIBC_2.2.5");
        *(void **)(&g_pthread_join) = qldr_dlvsym_old(RTLD_DEFAULT, "pthread_join", "GLIBC_2.2.5");
    }
    return g_pthread_create && g_pthread_join;
}

/* Forward declarations for helpers used by the writer / module lifecycle. */
static void put_i32_le(FILE *f, int32_t v);
static int write_record(FILE *f, int32_t sequence, const byte *data, size_t len);

/* TLS guards are important because fresh gamestate construction re-enters
 * SV_SendMessageToClient and that message must be captured, not transmitted. */
static __thread void *g_snapshot_client;
static __thread void *g_engine_gamestate_client;
static __thread server_demo_recorder_t *g_engine_gamestate_recorder;
static __thread int g_engine_gamestate_captured;
static __thread int g_internal_action;
static __thread int g_snapshot_last_valid;
static __thread ql1069_msg_t g_snapshot_last_msg;
static __thread byte g_snapshot_last_data[MAX_MSGLEN];
static __thread int32_t g_snapshot_last_sequence;

/* ------------------------------------------------------------------------- */
/* Logging/config.                                                           */
/* ------------------------------------------------------------------------- */

static int parse_bool_env(const char *name, int defval) {
    const char *v = getenv(name);
    if (!v || !*v) return defval;
    if (!strcasecmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "off"))
        return 0;
    return 1;
}

static int parse_int_env(const char *name, int defval, int minval, int maxval) {
    const char *v = getenv(name);
    unsigned long value = 0;
    int negative = 0;
    if (!v || !*v) return defval;
    if (*v == '+' || *v == '-') {
        negative = (*v == '-');
        ++v;
    }
    if (!*v) return defval;
    while (*v >= '0' && *v <= '9') {
        unsigned digit = (unsigned)(*v - '0');
        if (value > (unsigned long)maxval * 10ul + 10ul) return defval;
        value = value * 10ul + digit;
        ++v;
    }
    if (*v != '\0') return defval;
    if (negative) {
        long signed_value = -(long)value;
        if (signed_value < minval) signed_value = minval;
        if (signed_value > maxval) signed_value = maxval;
        return (int)signed_value;
    }
    if (value < (unsigned long)minval) value = (unsigned long)minval;
    if (value > (unsigned long)maxval) value = (unsigned long)maxval;
    return (int)value;
}

static void qldr_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[ql-server-demo-recorder] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void qldr_debug(const char *fmt, ...) {
    va_list ap;
    if (!g_debug) return;
    va_start(ap, fmt);
    fputs("[ql-server-demo-recorder] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------------- */
/* QLDS field access / safety.                                               */
/* ------------------------------------------------------------------------- */

static inline int32_t read_i32(const void *base, size_t off) {
    int32_t v;
    memcpy(&v, (const uint8_t *)base + off, sizeof(v));
    return v;
}

static inline void write_i32(void *base, size_t off, int32_t v) {
    memcpy((uint8_t *)base + off, &v, sizeof(v));
}

static inline uintptr_t main_va(uintptr_t va) {
    return g_main_base + va;
}

static inline int32_t client_state(void *client) {
    return read_i32(client, QL1069_CLIENT_STATE_OFFSET);
}

static inline int32_t client_delta_message(void *client) {
    return read_i32(client, QL1069_CLIENT_DELTA_MESSAGE_OFFSET);
}

static inline int32_t client_outgoing_sequence(void *client) {
    return read_i32(client, QL1069_CLIENT_OUTGOING_SEQUENCE_OFFSET);
}

static inline int32_t client_gamestate_sequence(void *client) {
    return read_i32(client, QL1069_CLIENT_GAMESTATE_SEQUENCE_OFFSET);
}

static inline int32_t level_time(void) {
    return g_level ? read_i32((const void *)g_level, QL1069_LEVEL_TIME_OFFSET) : 0;
}

/* client_t->gentity is a direct pointer to the corresponding gentity.
 * gentity->s.number is the authoritative QLDS client number, so no svs
 * global is needed at all. This also avoids depending on the fragile
 * SV_Shutdown+0xAC OFFSET_PP_SVS relocation used by minqlx internally. */
static int current_client_id(void *client) {
    void *gentity = NULL;
    int32_t number;
    if (!client) return -1;

    /* client_t is an engine heap object, not an ELF PT_LOAD segment.  The old
     * qldr_range_readable() check therefore rejected valid client pointers and
     * turned every snapshot into client=-1.  This function is only called from
     * engine callbacks whose first argument is already a live client_t*. */
    memcpy(&gentity, (const uint8_t *)client + QL1069_CLIENT_GENTITY_OFFSET,
           sizeof(gentity));
    if (!gentity) return -1;
    memcpy(&number, gentity, sizeof(number));
    if (number < 0 || number >= MDR_MAX_CLIENTS) return -1;
    return (int)number;
}

static uint64_t client_steam_id(void *client) {
    uint64_t v = 0;
    if (!client) return 0;
    memcpy(&v, (uint8_t *)client + QL1069_CLIENT_STEAM_ID_OFFSET, sizeof(v));
    return v;
}

static int client_remote_address_type(void *client) {
    if (!client) return -1;
    return read_i32(client, QL1069_CLIENT_NETCHAN_REMOTE_TYPE_OFFSET);
}

static uint32_t client_gentity_svflags(void *client) {
    void *gentity = NULL;
    uint32_t flags = 0;
    if (!client) return 0;

    /* client_t->gentity is the engine sharedEntity_t pointer used by QLDS.
     * The server/game shared layout puts entityShared_t::svFlags at +0x1e0
     * from the start of sharedEntity_t for this x64 build. */
    memcpy(&gentity, (const uint8_t *)client + QL1069_CLIENT_GENTITY_OFFSET,
           sizeof(gentity));
    if (!gentity) return 0;
    memcpy(&flags, (const uint8_t *)gentity + QL_GENTITY_R_SVFLAGS_OFFSET,
           sizeof(flags));
    return flags;
}

/* Prefer the authoritative game-side SVF_BOT marker.  SteamID checks remain
 * as compatibility fallbacks for older/minqlx-style bot representations.
 * Do not use netchan.remoteAddress.type here: human clients can transiently
 * have the same zero value while their network address is not fully settled,
 * which can suppress legitimate player recordings during the countdown. */
static int client_is_bot(void *client) {
    uint64_t steam;
    uint32_t svflags;
    if (!client) return 0;

    svflags = client_gentity_svflags(client);
    if (svflags & QL_SVF_BOT) return 1;

    steam = client_steam_id(client);
    if (steam == 0) return 1;
    while (steam >= 10u) steam /= 10u;
    return steam == 9u;
}

static int client_is_live(void *client) {
    int st;
    if (!client) return 0;
    st = client_state(client);
    return st == QL_CS_PRIMED || st == QL_CS_ACTIVE;
}

static int is_playing_client(void *client) {
    int id = current_client_id(client);
    int count;
    int i;
    if (id < 0 || !g_level) return 0;
    count = read_i32((const void *)g_level, QL1069_LEVEL_NUM_PLAYING_CLIENTS_OFFSET);
    if (count < 0 || count > MDR_MAX_CLIENTS) return 0;
    for (i = 0; i < count; ++i) {
        int listed = read_i32((const void *)g_level,
                              QL1069_LEVEL_SORTED_CLIENTS_OFFSET + (size_t)i * 4u);
        if (listed == id) return 1;
    }
    return 0;
}

static int level_pointer_plausible(void) {
    int32_t warmup, now, start, playing, intermission_q, intermission_t, round;
    if (!g_level || (g_level & 3u) != 0u) return 0;

    /* g_level is derived from the exact QLDS 1069 G_InitGame instruction.
     * Do not use dl_iterate_phdr()/PT_LOAD checks here: QLDS' VM module is
     * loaded by its custom loader, and the dynamic loader may not expose its
     * data mappings through dl_iterate_phdr(). */
    warmup = read_i32((const void *)g_level, QL1069_LEVEL_WARMUP_TIME_OFFSET);
    now = read_i32((const void *)g_level, QL1069_LEVEL_TIME_OFFSET);
    start = read_i32((const void *)g_level, QL1069_LEVEL_START_TIME_OFFSET);
    playing = read_i32((const void *)g_level, QL1069_LEVEL_NUM_PLAYING_CLIENTS_OFFSET);
    intermission_q = read_i32((const void *)g_level, QL1069_LEVEL_INTERMISSION_QUEUED_OFFSET);
    intermission_t = read_i32((const void *)g_level, QL1069_LEVEL_INTERMISSION_TIME_OFFSET);
    round = read_i32((const void *)g_level, QL1069_LEVEL_ROUND_CURRENT_OFFSET);
    if (playing < 0 || playing > MDR_MAX_CLIENTS) return 0;
    if (round < QL_ROUND_PREGAME || round > QL_ROUND_POSTGAME) return 0;
    if (now < -1000000 || warmup < -1000000 || start < -1000000 ||
        intermission_q < -1000000 || intermission_t < -1000000)
        return 0;

    return 1;
}

static void lifecycle_tick(void);
static void start_countdown_candidates_from_seen_clients(void);
static void reset_record(server_demo_recorder_t *r, int delete_file);
static void qldr_sys_setmoduleoffset(char *moduleName, void *offset);

/* ------------------------------------------------------------------------- */
/* qagame lifecycle capture.                                                 */
/* ------------------------------------------------------------------------- */

static int resolve_level_from_vmMain(void *vmMain) {
    uintptr_t vm = (uintptr_t)vmMain;
    uintptr_t rel_addr;
    uintptr_t vm_call_table;
    uintptr_t ginit;
    uintptr_t level_rel_addr;
    int32_t rel_vm_table;
    int32_t rel_level;

    if (!vm) return 0;

    rel_addr = vm + 0x3u;
    memcpy(&rel_vm_table, (const void *)rel_addr, sizeof(rel_vm_table));
    vm_call_table = rel_addr + 4u + (intptr_t)rel_vm_table;

    memcpy(&ginit, (const void *)(vm_call_table + 0x18u), sizeof(ginit));
    if (!ginit) return 0;

    level_rel_addr = ginit + 0x4A1u;
    memcpy(&rel_level, (const void *)level_rel_addr, sizeof(rel_level));
    g_level = level_rel_addr + 4u + (intptr_t)rel_level;
    g_ginitgame = ginit;
    g_vm_call_table = vm_call_table;

    if (!level_pointer_plausible()) {
        g_level = 0;
        g_ginitgame = 0;
        g_vm_call_table = 0;
        return 0;
    }
    return 1;
}

static void qldr_emit_abs_jump14(void *dst, const void *target) {
    uint8_t *p = (uint8_t *)dst;
    uintptr_t address = (uintptr_t)target;
    p[0] = 0xff;
    p[1] = 0x25;
    p[2] = 0x00;
    p[3] = 0x00;
    p[4] = 0x00;
    p[5] = 0x00;
    memcpy(p + 6, &address, sizeof(address));
}

static int qldr_call_original_setmoduleoffset(char *moduleName, void *offset) {
    ql_set_module_offset_fn original;
    int ok = 0;
    if (!g_set_module_target || g_set_module_reentry) return 0;

    /* The first 14 bytes of Sys_SetModuleOffset are not safe to execute from
     * our generic x64 trampoline when no minqlx trampoline already exists:
     * QLDS uses position-dependent instructions in this prologue. For this
     * one function, temporarily restore the exact original bytes, call QLDS
     * in place, and then reinstall our jump. QLDS is effectively single-threaded
     * here and Sys_SetModuleOffset is not recursively entered by the function. */
    if (!qldr_unhook_abs14((void *)g_set_module_target,
                           g_set_module_saved, sizeof(g_set_module_saved)))
        return 0;

    original = (ql_set_module_offset_fn)g_set_module_target;
    g_set_module_reentry = 1;
    original(moduleName, offset);
    g_set_module_reentry = 0;

    /* Reinstall the exact same detour without creating/using a trampoline. */
    {
        uintptr_t t = g_set_module_target;
        long ps = sysconf(_SC_PAGESIZE);
        uintptr_t page = t & ~((uintptr_t)ps - 1u);
        int prot = qldr_query_protection((void *)t);
        if (ps <= 0 || prot < 0 ||
            mprotect((void *)page, (size_t)ps, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            return 0;
        }
        qldr_emit_abs_jump14((uint8_t *)t, (const void *)qldr_sys_setmoduleoffset);
        __builtin___clear_cache((char *)t, (char *)t + sizeof(g_set_module_saved));
        (void)mprotect((void *)page, (size_t)ps, prot);
    }
    ok = 1;
    return ok;
}

static void qldr_sys_setmoduleoffset(char *moduleName, void *offset) {
    if (moduleName && !strcmp(moduleName, "qagame") && offset) {
        g_level = 0;
        g_ginitgame = 0;
        g_vm_call_table = 0;
        memset(g_seen_clients, 0, sizeof(g_seen_clients));
        if (resolve_level_from_vmMain(offset)) {
            qldr_log("qagame ready vmMain=%p vmcall=%p G_InitGame=%p level=%p",
                     offset, (void *)g_vm_call_table, (void *)g_ginitgame, (void *)g_level);
        } else {
            qldr_log("qagame ready hook saw vmMain=%p but level resolve failed", offset);
        }
        /* Do not modify the qagame VM call table. minqlx may legitimately
         * replace G_RunFrame there after this callback; competing with that
         * hook was the cause of the v15 client-connect crash.
         *
         * Also do NOT close active recordings here. QLDS can reload qagame
         * without ending the current server/map segment (for example during
         * minqlx/qagame reinitialization). Match-end scheduling is handled by
         * the level lifecycle instead. */
        g_postgame_scheduled = 0;
        if (!g_match_started)
            g_countdown_seen = 0;
        g_last_status = -1;
        g_last_lifecycle_time = INT32_MIN;
    }
    if (!qldr_call_original_setmoduleoffset(moduleName, offset)) {
        /* Best-effort fallback for an unexpected nested call: keep the hook
         * installed and do not recurse through an unsafe trampoline. */
        qldr_debug("Sys_SetModuleOffset original call was not chained");
    }
}

/* Dedicated writer.                                                         */
/* ------------------------------------------------------------------------- */

static void writer_signal_done(server_demo_recorder_t *r, uint64_t generation) {
    if (!r || r->generation != generation) return;
    pthread_mutex_lock(&g_writer_mutex);
    r->writer_closed = 1;
    pthread_cond_broadcast(&g_writer_done);
    pthread_mutex_unlock(&g_writer_mutex);
}

static void writer_close_record(writer_job_t *j) {
    server_demo_recorder_t *r = j->r;
    FILE *f = NULL;
    char path[PATH_MAX];
    char final_path[PATH_MAX];

    if (!r || r->generation != j->generation) return;

    path[0] = '\0';
    final_path[0] = '\0';
    strncpy(path, r->path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    strncpy(final_path, r->final_path, sizeof(final_path) - 1);
    final_path[sizeof(final_path) - 1] = '\0';

    f = r->file;
    if (f) {
        put_i32_le(f, -1);
        put_i32_le(f, -1);
        if (fflush(f) != 0) r->writer_error = 1;
        if (fclose(f) != 0) r->writer_error = 1;
        r->file = NULL;
    }

    if (j->delete_file || !r->has_demo_frame || r->writer_error) {
        if (path[0]) unlink(path);
    } else if (final_path[0] && strcmp(final_path, path) != 0) {
        char candidate[PATH_MAX];
        int suffix = 2;
        strncpy(candidate, final_path, sizeof(candidate) - 1);
        candidate[sizeof(candidate) - 1] = '\0';
        while (access(candidate, F_OK) == 0 && suffix < 1000) {
            char stem[PATH_MAX];
            size_t len = strlen(final_path);
            if (len >= 6 && !strcmp(final_path + len - 6, ".dm_91")) {
                size_t base = len - 6;
                if (base >= sizeof(stem) - 16) break;
                memcpy(stem, final_path, base);
                stem[base] = '\0';
                if (snprintf(candidate, sizeof(candidate), "%.*s-%d.dm_91",
                             (int)(sizeof(candidate) - 16), stem, suffix++) >= (int)sizeof(candidate))
                    break;
            } else break;
        }
        if (rename(path, candidate) == 0) {
            strncpy(r->path, candidate, sizeof(r->path) - 1);
            r->path[sizeof(r->path) - 1] = '\0';
        } else {
            r->writer_error = 1;
            qldr_log("final filename rename failed: %s -> %s: %s", path, candidate, strerror(errno));
        }
    }

    writer_signal_done(r, j->generation);
}

static void *writer_main(void *unused) {
    (void)unused;
    for (;;) {
        writer_job_t *j;
        pthread_mutex_lock(&g_writer_mutex);
        while (!g_writer_head && !g_writer_stop)
            pthread_cond_wait(&g_writer_nonempty, &g_writer_mutex);
        if (!g_writer_head && g_writer_stop) {
            pthread_mutex_unlock(&g_writer_mutex);
            break;
        }
        j = g_writer_head;
        g_writer_head = j->next;
        if (!g_writer_head) g_writer_tail = NULL;
        if (g_writer_depth) --g_writer_depth;
        pthread_cond_signal(&g_writer_space);
        pthread_mutex_unlock(&g_writer_mutex);

        if (j->r && j->r->generation == j->generation) {
            server_demo_recorder_t *r = j->r;
            if (j->type == WRITER_JOB_RECORD) {
                if (r->file && !r->writer_error &&
                    !write_record(r->file, j->sequence, j->data, (size_t)j->len)) {
                    r->writer_error = 1;
                    qldr_log("writer failed client=%d seq=%d", r->client_num, j->sequence);
                }
            } else if (j->type == WRITER_JOB_CLOSE) {
                writer_close_record(j);
            }
        }

        free(j->data);
        free(j);
    }
    return NULL;
}

static int writer_start(void) {
    if (g_writer_started) return 1;
    g_writer_stop = 0;
    g_writer_head = g_writer_tail = NULL;
    g_writer_depth = 0;
    if (!resolve_pthread_runtime() ||
        g_pthread_create(&g_writer_thread, NULL, writer_main, NULL) != 0)
        return 0;
    g_writer_started = 1;
    return 1;
}

static int writer_enqueue(writer_job_t *j) {
    int allow_one_overflow = (j && j->type == WRITER_JOB_CLOSE);
    pthread_mutex_lock(&g_writer_mutex);
    if (g_writer_stop || (!allow_one_overflow && g_writer_depth >= WRITER_MAX_DEPTH)) {
        pthread_mutex_unlock(&g_writer_mutex);
        return 0;
    }
    j->next = NULL;
    if (g_writer_tail) g_writer_tail->next = j;
    else g_writer_head = j;
    g_writer_tail = j;
    ++g_writer_depth;
    pthread_cond_signal(&g_writer_nonempty);
    pthread_mutex_unlock(&g_writer_mutex);
    return 1;
}

static int writer_queue_record(server_demo_recorder_t *r, int32_t seq,
                               const byte *data, int len) {
    writer_job_t *j;
    if (!r || !r->file || !data || len <= 0) return 0;
    j = (writer_job_t *)calloc(1, sizeof(*j));
    if (!j) return 0;
    j->data = (byte *)malloc((size_t)len);
    if (!j->data) { free(j); return 0; }
    memcpy(j->data, data, (size_t)len);
    j->type = WRITER_JOB_RECORD;
    j->r = r;
    j->generation = r->generation;
    j->sequence = seq;
    j->len = len;
    if (!writer_enqueue(j)) {
        free(j->data);
        free(j);
        qldr_log("writer queue full/stopped client=%d seq=%d; aborting demo without blocking QLDS",
                 r->client_num, seq);
        return 0;
    }
    return 1;
}

static void writer_close_wait(server_demo_recorder_t *r, int delete_file) {
    writer_job_t *j;
    uint64_t gen;
    if (!r || !r->file || !g_writer_started) return;
    gen = r->generation;
    r->writer_closing = 1;
    r->writer_closed = 0;
    j = (writer_job_t *)calloc(1, sizeof(*j));
    if (!j) {
        r->writer_error = 1;
        return;
    }
    j->type = WRITER_JOB_CLOSE;
    j->r = r;
    j->generation = gen;
    j->delete_file = delete_file;
    if (!writer_enqueue(j)) {
        free(j->data);
        free(j);
        if (r->file) {
            put_i32_le(r->file, -1);
            put_i32_le(r->file, -1);
            (void)fflush(r->file);
            (void)fclose(r->file);
            r->file = NULL;
        }
        return;
    }

    pthread_mutex_lock(&g_writer_mutex);
    while (!r->writer_closed && r->generation == gen)
        pthread_cond_wait(&g_writer_done, &g_writer_mutex);
    pthread_mutex_unlock(&g_writer_mutex);
    r->writer_closing = 0;
}

static void writer_stop(void) {
    if (!g_writer_started) return;
    pthread_mutex_lock(&g_writer_mutex);
    g_writer_stop = 1;
    pthread_cond_broadcast(&g_writer_nonempty);
    pthread_mutex_unlock(&g_writer_mutex);
    if (g_pthread_join)
        (void)g_pthread_join(g_writer_thread, NULL);
    g_writer_started = 0;
}

/* ------------------------------------------------------------------------- */
/* File helpers.                                                             */
/* ------------------------------------------------------------------------- */

static void put_i32_le(FILE *f, int32_t v) {
    uint32_t u = (uint32_t)v;
    byte b[4];
    b[0] = (byte)(u & 0xffu);
    b[1] = (byte)((u >> 8) & 0xffu);
    b[2] = (byte)((u >> 16) & 0xffu);
    b[3] = (byte)((u >> 24) & 0xffu);
    (void)fwrite(b, 1, sizeof(b), f);
}

static int write_record(FILE *f, int32_t sequence, const byte *data, size_t len) {
    if (!f || len > INT32_MAX) return 0;
    put_i32_le(f, sequence);
    put_i32_le(f, (int32_t)len);
    return !len || fwrite(data, 1, len, f) == len;
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t i;
    DIR *dir;
    if (!path || !*path) return 0;
    if (strlen(path) >= sizeof(tmp)) return 0;
    strcpy(tmp, path);

    for (i = 1; tmp[i]; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (*tmp && mkdir(tmp, 0755) != 0 && errno != EEXIST) return 0;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return 0;

    dir = opendir(tmp);
    if (!dir) return 0;
    closedir(dir);
    return 1;
}

static void read_client_name(void *client, char *out, size_t out_sz) {
    size_t n;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!client) return;
    n = out_sz - 1;
    if (n > 32) n = 32;
    memcpy(out, (const uint8_t *)client + QL1069_CLIENT_NAME_OFFSET, n);
    out[n] = '\0';
}

static void safe_component(const char *src, char *dst, size_t dst_sz) {
    size_t i, j = 0;
    if (!dst || dst_sz == 0) return;
    for (i = 0; src && src[i] && j + 1 < dst_sz; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '^' && src[i + 1] >= '0' && src[i + 1] <= '9') {
            ++i;
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
            c == '(' || c == ')') {
            dst[j++] = (char)c;
        } else if (j && dst[j - 1] != '-') {
            dst[j++] = '-';
        }
    }
    while (j && dst[j - 1] == '-') --j;
    if (!j) {
        strncpy(dst, "player", dst_sz - 1);
        dst[dst_sz - 1] = '\0';
    } else {
        dst[j] = '\0';
    }
}

typedef struct ql_cvar_probe_s {
    const char *name;
    const char *string;
} ql_cvar_probe_t;
typedef ql_cvar_probe_t *(*ql_cvar_find_var_fn)(const char *name);

static const char *ql_cvar_string(const char *name) {
    static char cached[QL_MAX_SAFE_NAME];
    ql_cvar_find_var_fn find_var;
    ql_cvar_probe_t *cv;
    cached[0] = '\0';
    if (!name || !*name) return cached;
    find_var = (ql_cvar_find_var_fn)main_va((uintptr_t)0x426160);
    if (find_var && qldr_address_executable((const void *)find_var)) {
        cv = find_var(name);
        if (cv && cv->string && *cv->string) {
            strncpy(cached, cv->string, sizeof(cached) - 1);
            cached[sizeof(cached) - 1] = '\0';
        }
    }
    return cached;
}

static int ql_gametype_value(void) {
    const char *s = ql_cvar_string("g_gametype");
    long v = 0;
    int sign = 1;
    int any = 0;
    if (!s || !*s) return -1;
    if (*s == '-' || *s == '+') {
        if (*s == '-') sign = -1;
        ++s;
    }
    while (*s >= '0' && *s <= '9') {
        int digit = *s - '0';
        if (v > (LONG_MAX - digit) / 10L) return -1;
        v = v * 10L + digit;
        any = 1;
        ++s;
    }
    if (!any || *s != '\0') return -1;
    v *= sign;
    if (v < INT_MIN || v > INT_MAX) return -1;
    return (int)v;
}

static const char *ql_gametype_name(void) {
    switch (ql_gametype_value()) {
        case 0:  return "FFA";
        case 1:  return "DUEL";
        case 2:  return "RACE";
        case 3:  return "TDM";
        case 4:  return "CA";
        case 5:  return "CTF";
        case 6:  return "1FCTF";
        case 8:  return "HARVESTER";
        case 9:  return "FT";
        case 10: return "DOM";
        case 11: return "AD";
        case 12: return "RR";
        default: {
            static char unknown[16];
            int gt = ql_gametype_value();
            if (gt < 0) return "GT-UNKNOWN";
            snprintf(unknown, sizeof(unknown), "GT%d", gt);
            return unknown;
        }
    }
}

static int ql_is_duel(void) {
    return ql_gametype_value() == 1;
}

static const char *ql_map_name(void) {
    static char cached[QL_MAX_SAFE_NAME];
    const char *map = ql_cvar_string("mapname");
    cached[0] = '\0';
    if (map && *map)
        safe_component(map, cached, sizeof(cached));
    if (!cached[0]) strncpy(cached, "map-unknown", sizeof(cached) - 1);
    cached[sizeof(cached) - 1] = '\0';
    return cached;
}

static void remember_client(void *client) {
    int id = current_client_id(client);
    if (id >= 0 && id < MDR_MAX_CLIENTS) g_seen_clients[id] = client;
}

static void refresh_opponent_name(server_demo_recorder_t *r) {
    int count, self, i, other = -1;
    void *opp = NULL;
    char raw[QL_MAX_SAFE_NAME];
    char safe[QL_MAX_SAFE_NAME];
    if (!r || !r->client || !g_level) return;
    count = read_i32((const void *)g_level, QL1069_LEVEL_NUM_PLAYING_CLIENTS_OFFSET);
    self = current_client_id(r->client);
    if (count != 2 || self < 0) return;
    for (i = 0; i < count; ++i) {
        int id = read_i32((const void *)g_level,
                          QL1069_LEVEL_SORTED_CLIENTS_OFFSET + (size_t)i * 4u);
        if (id >= 0 && id < MDR_MAX_CLIENTS && id != self) {
            other = id;
            break;
        }
    }
    if (other < 0) return;
    r->opponent_client_num = other;
    opp = g_seen_clients[other];
    if (!opp || !client_is_live(opp)) return;
    read_client_name(opp, raw, sizeof(raw));
    safe_component(raw, safe, sizeof(safe));
    if (safe[0]) strncpy(r->opponent_name, safe, sizeof(r->opponent_name) - 1);
    r->opponent_name[sizeof(r->opponent_name) - 1] = '\0';
}

static void build_final_demo_path(server_demo_recorder_t *r) {
    struct tm tmv;
    char stamp[64];
    int n;
    if (!r || !r->client) return;
    localtime_r(&r->started_wall_time, &tmv);
    snprintf(stamp, sizeof(stamp), "%04d_%02d_%02d-%02d_%02d_%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    if (ql_is_duel() && r->opponent_name[0]) {
        n = snprintf(r->final_path, sizeof(r->final_path),
                     "%s/%s(POV)-vs-%s-%s-%s.dm_91",
                     g_output_path, r->pov_name, r->opponent_name,
                     r->map_name[0] ? r->map_name : "map-unknown", stamp);
    } else {
        n = snprintf(r->final_path, sizeof(r->final_path),
                     "%s/%s-%s-%s-%s.dm_91",
                     g_output_path, ql_gametype_name(), r->pov_name,
                     r->map_name[0] ? r->map_name : "map-unknown", stamp);
    }
    if (n < 0 || (size_t)n >= sizeof(r->final_path)) r->final_path[0] = '\0';
}

static void build_temp_demo_path(server_demo_recorder_t *r) {
    int n;
    if (!r) return;
    n = snprintf(r->temp_path, sizeof(r->temp_path),
                 "%s/.ql-server-demo-recorder-%d-%d-%" PRIu64 ".part",
                 g_output_path, (int)getpid(), r->client_num, ++g_file_serial);
    if (n < 0 || (size_t)n >= sizeof(r->temp_path)) r->temp_path[0] = '\0';
}

static void maybe_finalize_filename(server_demo_recorder_t *r) {
    if (!r || !r->active) return;
    if (ql_is_duel()) {
        refresh_opponent_name(r);
        if (r->opponent_name[0] && !strstr(r->final_path, "-vs-"))
            build_final_demo_path(r);
    } else if (strstr(r->final_path, "-vs-")) {
        build_final_demo_path(r);
    }
}

/* ------------------------------------------------------------------------- */
/* Recorder bookkeeping.                                                     */
/* ------------------------------------------------------------------------- */

static server_demo_recorder_t *recorder_for_client(void *client) {
    int i;
    if (!client) return NULL;
    for (i = 0; i < MDR_MAX_CLIENTS; ++i)
        if (g_rec[i].client == client) return &g_rec[i];
    return NULL;
}

static server_demo_recorder_t *ensure_recorder(void *client) {
    server_demo_recorder_t *r = recorder_for_client(client);
    int i;
    if (r) return r;
    for (i = 0; i < MDR_MAX_CLIENTS; ++i) {
        if (!g_rec[i].client) {
            memset(&g_rec[i], 0, sizeof(g_rec[i]));
            g_rec[i].client = client;
            g_rec[i].client_num = current_client_id(client);
            g_rec[i].gamestate_sequence = -1;
            g_rec[i].generation = 1u;
            return &g_rec[i];
        }
    }
    return NULL;
}

static void restore_client_delta(server_demo_recorder_t *r) {
    if (r && r->client && r->delta_forced) {
        write_i32(r->client, QL1069_CLIENT_DELTA_MESSAGE_OFFSET, r->saved_delta_message);
        r->delta_forced = 0;
    }
}

static void close_writer(server_demo_recorder_t *r, int delete_file) {
    if (!r || !r->file) return;
    if (g_writer_started) {
        writer_close_wait(r, delete_file);
        if (r->writer_error && r->path[0]) unlink(r->path);
    } else {
        if (r->file) {
            put_i32_le(r->file, -1);
            put_i32_le(r->file, -1);
            (void)fflush(r->file);
            (void)fclose(r->file);
            r->file = NULL;
        }
        if (delete_file && r->path[0]) unlink(r->path);
    }
}

static void reset_record(server_demo_recorder_t *r, int delete_file) {
    uint64_t next_generation;
    if (!r) return;
    next_generation = r->generation + 1u;
    close_writer(r, delete_file);
    memset(r, 0, sizeof(*r));
    r->generation = next_generation ? next_generation : 1u;
    r->state = MDR_IDLE;
    r->gamestate_sequence = -1;
    r->first_full_sequence = -1;
}

/* ------------------------------------------------------------------------- */
/* Gamestate bootstrap.                                                       */
/* ------------------------------------------------------------------------- */

static void store_gamestate(server_demo_recorder_t *r, void *client, const void *msg) {
    const ql1069_msg_t *m = (const ql1069_msg_t *)msg;
    if (!r || !client || !m || !m->data) return;
    if (m->cursize <= 0 || m->cursize > MAX_MSGLEN) return;
    memcpy(r->gamestate, m->data, (size_t)m->cursize);
    r->gamestate_size = m->cursize;
    r->gamestate_bit = m->bit;
    r->gamestate_sequence = client_gamestate_sequence(client);
    r->gamestate_valid = 1;
}

static int capture_fresh_engine_gamestate(void *client, server_demo_recorder_t *r) {
    uint8_t *saved = NULL;
    int32_t saved_rel_ack;
    int32_t saved_last_cmd;

    if (!client || !r || !g_send_gamestate) return 0;

    saved = malloc(QL1069_CLIENT_SIZE);
    if (!saved) return 0;
    memcpy(saved, client, QL1069_CLIENT_SIZE);

    saved_rel_ack = read_i32(client, QL1069_CLIENT_RELIABLE_ACKNOWLEDGE_OFFSET);
    saved_last_cmd = read_i32(client, QL1069_CLIENT_LAST_COMMAND_OFFSET);

    /* Suppress pending reliable server commands in the network gamestate. */
    write_i32(client, QL1069_CLIENT_RELIABLE_ACKNOWLEDGE_OFFSET,
              read_i32(client, QL1069_CLIENT_RELIABLE_SEQUENCE_OFFSET));
    write_i32(client, QL1069_CLIENT_LAST_COMMAND_OFFSET, 0);

    g_internal_action = 1;
    g_engine_gamestate_client = client;
    g_engine_gamestate_recorder = r;
    g_engine_gamestate_captured = 0;

    qldr_debug("capturing fresh gamestate client=%d", current_client_id(client));
    g_send_gamestate(client);

    g_engine_gamestate_client = NULL;
    g_engine_gamestate_recorder = NULL;
    g_internal_action = 0;

    memcpy(client, saved, QL1069_CLIENT_SIZE);
    write_i32(client, QL1069_CLIENT_RELIABLE_ACKNOWLEDGE_OFFSET, saved_rel_ack);
    write_i32(client, QL1069_CLIENT_LAST_COMMAND_OFFSET, saved_last_cmd);
    free(saved);

    return g_engine_gamestate_captured;
}

static int finalize_gamestate_for_demo(server_demo_recorder_t *r) {
    ql1069_msg_t tmp;
    if (!r || !r->gamestate_valid || !g_msg_write_byte) return 0;
    if (r->gamestate_size <= 0 || r->gamestate_size > MAX_MSGLEN) return 0;
    if (r->gamestate_bit <= 0 || r->gamestate_bit > MAX_MSGLEN * 8) return 0;

    memset(&tmp, 0, sizeof(tmp));
    tmp.data = r->gamestate;
    tmp.maxsize = MAX_MSGLEN;
    tmp.cursize = r->gamestate_size;
    tmp.bit = r->gamestate_bit;
    g_msg_write_byte(&tmp, QL_SVC_EOF);
    if (tmp.overflowed || tmp.cursize <= 0 || tmp.cursize > MAX_MSGLEN) return 0;
    r->gamestate_size = tmp.cursize;
    r->gamestate_bit = tmp.bit;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Automatic countdown lifecycle.                                            */
/* ------------------------------------------------------------------------- */

static int32_t actual_match_end_server_time(void) {
    int32_t queued;
    int32_t intermission;
    int32_t forfeited;
    if (!g_level) return 0;
    queued = read_i32((const void *)g_level, QL1069_LEVEL_INTERMISSION_QUEUED_OFFSET);
    if (queued > 0 && queued < INT32_MAX / 2) return queued;
    intermission = read_i32((const void *)g_level, QL1069_LEVEL_INTERMISSION_TIME_OFFSET);
    if (intermission > 0 && intermission < INT32_MAX / 2) return intermission;
    forfeited = read_i32((const void *)g_level, QL1069_LEVEL_MATCH_FORFEITED_OFFSET);
    if (forfeited != 0 && g_match_started) return level_time();
    return level_time();
}

static int level_status(void) {
    int32_t intermission_q, intermission_t, round, warmup, now, delta, forfeited;
    if (!g_level) return 0;

    intermission_q = read_i32((const void *)g_level, QL1069_LEVEL_INTERMISSION_QUEUED_OFFSET);
    intermission_t = read_i32((const void *)g_level, QL1069_LEVEL_INTERMISSION_TIME_OFFSET);
    forfeited = read_i32((const void *)g_level, QL1069_LEVEL_MATCH_FORFEITED_OFFSET);
    round = read_i32((const void *)g_level, QL1069_LEVEL_ROUND_CURRENT_OFFSET);
    warmup = read_i32((const void *)g_level, QL1069_LEVEL_WARMUP_TIME_OFFSET);
    now = read_i32((const void *)g_level, QL1069_LEVEL_TIME_OFFSET);

    /* ROUND_OVER / POSTGAME are round-state transitions in QL and are not
     * sufficient by themselves to declare the whole match finished. Use the
     * actual intermission markers (or an explicit forfeiture) as the stop
     * trigger. This prevents multi-round matches from being cut after an
     * intermediate round. */
    if (intermission_q > 0)
        return 4;
    if (intermission_t > 0)
        return 4;
    if (forfeited != 0 && g_match_started)
        return 4;

    delta = warmup - now;
    if (warmup > 0 && delta > 0 && delta <= 120000)
        return 2;
    if (round == QL_ROUND_WARMUP || round == QL_ROUND_SHUFFLE)
        return 2;
    if (round == QL_ROUND_BEGUN)
        return 3;
    if (g_countdown_seen && warmup > 0 && delta <= 0)
        return 3;
    return 1;
}

static void log_level_transition(int status) {
    int32_t warmup, now, round, players;
    if (!g_level || status == g_last_status) return;
    warmup = read_i32((const void *)g_level, QL1069_LEVEL_WARMUP_TIME_OFFSET);
    now = read_i32((const void *)g_level, QL1069_LEVEL_TIME_OFFSET);
    round = read_i32((const void *)g_level, QL1069_LEVEL_ROUND_CURRENT_OFFSET);
    players = read_i32((const void *)g_level, QL1069_LEVEL_NUM_PLAYING_CLIENTS_OFFSET);
    qldr_log("state=%d round=%d warmup=%d level_time=%d warmup_delta=%d playing=%d",
             status, round, warmup, now, warmup - now, players);
    g_last_status = status;
}

static void prune_prestart_records(void) {
    int i;
    for (i = 0; i < MDR_MAX_CLIENTS; ++i) {
        server_demo_recorder_t *r = &g_rec[i];
        if (!r->client || !r->active) continue;
        if (!client_is_live(r->client) ||
            (r->steam_id && client_steam_id(r->client) != r->steam_id) ||
            !is_playing_client(r->client)) {
            qldr_log("countdown recording cancelled client=%d: player left playing set",
                     r->client_num);
            reset_record(r, 1);
        }
    }
}

static void close_reused_or_dead_clients(void) {
    int i;
    for (i = 0; i < MDR_MAX_CLIENTS; ++i) {
        server_demo_recorder_t *r = &g_rec[i];
        if (!r->client || !r->active) continue;
        if ((r->steam_id && client_steam_id(r->client) != r->steam_id) ||
            (!g_match_started && !r->stop_pending && !client_is_live(r->client))) {
            /* Before match start, leaving the player set invalidates the pending
             * countdown recording. After match start, keep the segment alive
             * through spectator/CS_FREE transitions until the post-game deadline. */
            reset_record(r, 0);
        }
    }
}

static void lifecycle_tick(void) {
    int status;
    int prev_status;
    int i;

    if (!g_enabled || g_internal_action || !g_level) return;
    if (!level_pointer_plausible()) {
        qldr_debug("level pointer no longer plausible; waiting for next qagame Sys_SetModuleOffset");
        /* A transient/teardown level pointer must not truncate an active demo.
         * qagame reload is not, by itself, a match-end signal. Keep recorder
         * state intact and let the next qagame load restore g_level. */
        g_ginitgame = 0;
        g_level = 0;
        g_vm_call_table = 0;
        g_countdown_seen = 0;
        g_match_started = 0;
        g_postgame_scheduled = 0;
        g_last_status = -1;
        g_last_lifecycle_time = INT32_MIN;
        memset(g_seen_clients, 0, sizeof(g_seen_clients));
        return;
    }

    prev_status = g_last_status;
    status = level_status();
    if (prev_status == 4 && status != 4) {
        /* A new match/round cycle after postgame. The next countdown can
         * create new recordings without touching an already-finished segment. */
        g_postgame_scheduled = 0;
        g_countdown_seen = 0;
        g_match_started = 0;        
    }

    log_level_transition(status);
    if (status == 1) {
        if (!g_match_started) prune_prestart_records();
    }
    else if (status == 2) {
        g_countdown_seen = 1;
        /* During countdown an active recording is provisional: the player must
         * still belong to the playing set. Going to spectator cancels it and
         * deletes the .part file instead of leaving a partial demo behind. */
        if (!g_match_started) prune_prestart_records();
    } else if (status == 3) {
        if (g_countdown_seen) g_match_started = 1;
    } else if (status == 4) {
        if (!g_postgame_scheduled) {
            for (i = 0; i < MDR_MAX_CLIENTS; ++i) {
                server_demo_recorder_t *r = &g_rec[i];
                if (r->client && r->active) {
                    /* Never stop immediately on spectator transition. Game end
                     * is the event; the recorder remains alive for the exact
                     * QLDS server-time delay requested by the user. */
                    r->stop_pending = 1;
                    r->stop_server_time = actual_match_end_server_time() + (int32_t)g_stop_delay_ms;
                    r->stop_due_reached = 0;
                    r->stop_due_frame = 0;
                    r->snapshot_after_due = 0;
                    qldr_log("scheduled stop client=%d end=%d due=%d delay=%d ms",
                             r->client_num, actual_match_end_server_time(), r->stop_server_time, g_stop_delay_ms);
                }
            }
            g_postgame_scheduled = 1;
        }
    }

    close_reused_or_dead_clients();
}

/* ------------------------------------------------------------------------- */
/* Start/record.                                                             */
/* ------------------------------------------------------------------------- */

static int start_client(void *client) {
    server_demo_recorder_t *r;
    FILE *f;
    char raw[QL_MAX_SAFE_NAME];
    char safe[QL_MAX_SAFE_NAME];
    int32_t demo_gamestate_sequence;

    if (!g_enabled || !client || !g_send_gamestate) return 0;
    if (!client_is_live(client) || !is_playing_client(client) || client_is_bot(client)) {
        qldr_debug("start skipped client=%d live=%d playing=%d bot=%d state=%d addrtype=%d svflags=0x%08x",
                   current_client_id(client), client_is_live(client),
                   is_playing_client(client), client_is_bot(client), client_state(client),
                   client_remote_address_type(client), client_gentity_svflags(client));
        return 0;
    }
    r = recorder_for_client(client);
    if (r && r->active) return 1;

    r = ensure_recorder(client);
    if (!r) return 0;
    if (r->active) reset_record(r, 1);

    if (!capture_fresh_engine_gamestate(client, r)) {
        qldr_log("start failed client=%d: fresh gamestate capture", current_client_id(client));
        reset_record(r, 1);
        return 0;
    }
    if (!r->gamestate_valid || !finalize_gamestate_for_demo(r)) {
        qldr_log("start failed client=%d: gamestate capture/finalization", current_client_id(client));
        reset_record(r, 1);
        return 0;
    }

    read_client_name(client, raw, sizeof(raw));
    safe_component(raw, safe, sizeof(safe));
    strncpy(r->pov_name, safe, sizeof(r->pov_name) - 1);
    r->pov_name[sizeof(r->pov_name) - 1] = '\0';
    r->map_name[0] = '\0';
    strncpy(r->map_name, ql_map_name(), sizeof(r->map_name) - 1);
    r->map_name[sizeof(r->map_name) - 1] = '\0';
    r->opponent_client_num = -1;
    r->started_wall_time = time(NULL);
    refresh_opponent_name(r);
    build_final_demo_path(r);

    r->client_num = current_client_id(client);
    build_temp_demo_path(r);
    if (!r->temp_path[0]) {
        reset_record(r, 1);
        qldr_log("cannot create temporary demo path for client=%d", r->client_num);
        return 0;
    }
    if (!mkdir_p(g_output_path)) {
        reset_record(r, 1);
        qldr_log("cannot create output directory: %s", g_output_path);
        return 0;
    }
    if (!writer_start()) {
        reset_record(r, 1);
        qldr_log("cannot start dedicated demo writer thread");
        return 0;
    }
    f = fopen(r->temp_path, "wb");
    if (!f) {
        reset_record(r, 1);
        qldr_log("cannot open demo file: %s", r->temp_path);
        return 0;
    }
    (void)setvbuf(f, NULL, _IOFBF, 1u << 20);

    /* The native dm_91 bootstrap uses the live gamestate sequence.
     * Do not shift it.  The following two server snapshots are forced full
     * while deltaMessage == -1: the first is synchronization-only and is
     * omitted from the demo; the second is the first stored snapshot.
     * This is the exact bootstrap used by the supplied recorder patch. */
    demo_gamestate_sequence = r->gamestate_sequence;
    put_i32_le(f, demo_gamestate_sequence);
    put_i32_le(f, (int32_t)r->gamestate_size);
    if (r->gamestate_size && fwrite(r->gamestate, 1, (size_t)r->gamestate_size, f) != (size_t)r->gamestate_size) {
        fclose(f);
        unlink(r->temp_path);
        reset_record(r, 1);
        return 0;
    }
    r->gamestate_written = 1;
    r->file = f;
    r->active = 1;
    r->state = MDR_WAIT_FIRST_FULL;
    r->first_full_sequence = -1;
    r->steam_id = client_steam_id(client);
    strncpy(r->path, r->temp_path, sizeof(r->path) - 1);
    r->path[sizeof(r->path) - 1] = '\0';

    r->saved_delta_message = client_delta_message(client);
    write_i32(client, QL1069_CLIENT_DELTA_MESSAGE_OFFSET, -1);
    r->delta_forced = 1;

    qldr_log("started client=%d path=%s gamestate_seq=%d waiting_for_two_full_snapshots",
             r->client_num, r->final_path, demo_gamestate_sequence);
    return 1;
}

static int append_engine_eof(const ql1069_msg_t *src, byte *out, int *out_size) {
    ql1069_msg_t tmp;
    if (!src || !src->data || !out || !out_size || !g_msg_write_byte) return 0;
    if (src->cursize <= 0 || src->cursize > MAX_MSGLEN) return 0;
    memcpy(out, src->data, (size_t)src->cursize);
    tmp = *src;
    tmp.data = out;
    tmp.maxsize = MAX_MSGLEN;
    tmp.cursize = src->cursize;
    tmp.overflowed = 0;
    g_msg_write_byte(&tmp, QL_SVC_EOF);
    if (tmp.overflowed || tmp.cursize <= 0 || tmp.cursize > MAX_MSGLEN) return 0;
    *out_size = tmp.cursize;
    return 1;
}

static void record_snapshot_message(void *client, const void *msg, int32_t seq) {
    server_demo_recorder_t *r;
    const ql1069_msg_t *m = (const ql1069_msg_t *)msg;
    byte demo_message[MAX_MSGLEN];
    int out_size;

    if (!client || !m || !m->data || m->cursize <= 0 || m->cursize > MAX_MSGLEN)
        return;
    r = recorder_for_client(client);
    if (!r || !r->active || !r->file || r->writer_closing) return;

    if (!append_engine_eof(m, demo_message, &out_size)) return;

    if (r->state == MDR_WAIT_FIRST_FULL) {
        /* The first forced-full snapshot belongs to the live-client
         * synchronization after we switch deltaMessage to -1.  It is NOT a
         * demo frame.  Recording it shifts the demo frame indices by one and
         * makes the next delta refer to an invalid frame 1. */
        r->first_full_sequence = seq;
        r->state = MDR_WAIT_SECOND_FULL;
        qldr_log("first full skipped client=%d seq=%d", r->client_num, seq);
        return;
    }

    if (r->state == MDR_WAIT_SECOND_FULL) {
        /* The second consecutive full snapshot is the demo's first frame.
         * Restore the client's original delta base immediately afterwards so
         * subsequent network snapshots use the normal delta chain. */
        if (!writer_queue_record(r, seq, demo_message, out_size)) {
            r->writer_error = 1;
            reset_record(r, 1);
            return;
        }
        r->state = MDR_RECORDING;
        r->has_demo_frame = 1;
        restore_client_delta(r);
        qldr_log("second full recorded client=%d seq=%d after_first=%d; recording normal deltas",
                 r->client_num, seq, r->first_full_sequence);
        return;
    }

    if (r->state == MDR_RECORDING) {
        if (!writer_queue_record(r, seq, demo_message, out_size)) {
            r->writer_error = 1;
            reset_record(r, 1);
            return;
        }
        r->has_demo_frame = 1;
    }
}

static void start_countdown_candidates_from_seen_clients(void) {
    int i;
    if (!g_enabled || !g_level || g_match_started) return;
    if (level_status() != 2) return;
    for (i = 0; i < MDR_MAX_CLIENTS; ++i) {
        void *client = g_seen_clients[i];
        server_demo_recorder_t *r;
        if (!client || current_client_id(client) != i) continue;
        if (client_is_bot(client)) continue;
        if (!client_is_live(client) || !is_playing_client(client)) continue;
        r = recorder_for_client(client);
        if (r && r->active) continue;
        qldr_log("countdown candidate client=%d bot=%d steam=%" PRIu64 " addrtype=%d svflags=0x%08x",
                 i, client_is_bot(client), client_steam_id(client),
                 client_remote_address_type(client), client_gentity_svflags(client));
        (void)start_client(client);
    }
}

/* ------------------------------------------------------------------------- */
/* Inline hooks.                                                             */
/* ------------------------------------------------------------------------- */

static void maybe_finalize_due_non_snapshot(void *client) {
    server_demo_recorder_t *r;
    int32_t now;
    if (!client || g_snapshot_client == client) return;
    r = recorder_for_client(client);
    if (!r || !r->active || !r->stop_pending) return;
    now = level_time();
    if ((int32_t)(now - r->stop_server_time) < 0) return;
    r->stop_due_reached = 1;
    qldr_log("core stop client=%d now=%d due=%d (non-snapshot message)",
             r->client_num, now, r->stop_server_time);
    reset_record(r, 0);
}

static void __attribute__((noinline)) hook_send_message(void *msg, void *client) {
    const ql1069_msg_t *m = (const ql1069_msg_t *)msg;
    server_demo_recorder_t *r = NULL;

    if (!g_message_hook_seen) {
        g_message_hook_seen = 1;
        qldr_log("SV_SendMessageToClient hook reached");
    }

    if (g_engine_gamestate_client == client &&
        g_engine_gamestate_recorder && g_internal_action) {
        store_gamestate(g_engine_gamestate_recorder, client, msg);
        g_engine_gamestate_captured = g_engine_gamestate_recorder->gamestate_valid;
        return;
    }

    /* Never serialize/write from inside SV_SendMessageToClient. A normal
     * client snapshot reaches this hook after the server has finished building
     * the message; just retain one copy and process it after the snapshot
     * returns. This keeps disk I/O and MSG_WriteByte out of the QLDS network
     * hot path and avoids ping spikes. */
    if (!g_internal_action && g_snapshot_client == client && m && m->data &&
        m->cursize > 0 && m->cursize <= MAX_MSGLEN) {
        r = recorder_for_client(client);
        if (r && r->active && r->file) {
            g_snapshot_last_msg = *m;
            memcpy(g_snapshot_last_data, m->data, (size_t)m->cursize);
            g_snapshot_last_msg.data = g_snapshot_last_data;
            g_snapshot_last_valid = 1;
            g_snapshot_last_sequence = client_outgoing_sequence(client);
        }
    }

    g_send_message(msg, client);
    if (!g_internal_action) {
        lifecycle_tick();
    }
    maybe_finalize_due_non_snapshot(client);
}

static void __attribute__((noinline)) hook_send_snapshot(void *client) {
    void *old = g_snapshot_client;
    int32_t seq = -1;
    if (!g_snapshot_hook_seen) {
        g_snapshot_hook_seen = 1;
        qldr_log("SV_SendClientSnapshot hook reached client=%d", current_client_id(client));
    }

    remember_client(client);
    g_snapshot_last_valid = 0;
    g_snapshot_last_sequence = -1;
    g_snapshot_client = client;

    /* Re-force the bootstrap snapshot immediately before QLDS builds it.
     * The countdown/start callback can run well before the next network
     * snapshot and deltaMessage may be changed again by then. If bootstrap
     * is active, SV_WriteSnapshotToClient must see deltaMessage <= 0 so the
     * message we skip/record here is a genuine uncompressed snapshot. */
    {
        server_demo_recorder_t *pre = recorder_for_client(client);
        if (pre && pre->active && pre->state != MDR_RECORDING) {
            write_i32(client, QL1069_CLIENT_DELTA_MESSAGE_OFFSET, -1);
            pre->delta_forced = 1;
            if (g_debug) {
                qldr_log("bootstrap force-full client=%d state=%d seq=%d",
                         pre->client_num, pre->state, client_outgoing_sequence(client));
            }
        }
    }

    g_send_snapshot(client);
    g_snapshot_client = old;

    if (g_snapshot_last_valid && g_snapshot_last_sequence >= 0) {
        seq = g_snapshot_last_sequence;
        record_snapshot_message(client, (const void *)&g_snapshot_last_msg, seq);
    }

    {
        server_demo_recorder_t *rr = recorder_for_client(client);
        if (rr && rr->active && rr->stop_pending) {
            int32_t now = level_time();
            if ((int32_t)(now - rr->stop_server_time) >= 0) {
                rr->stop_due_reached = 1;
                rr->snapshot_after_due = 1;
                qldr_log("core stop client=%d now=%d due=%d (after final snapshot)",
                         rr->client_num, now, rr->stop_server_time);
                reset_record(rr, 0);
            }
        }
    }

    remember_client(client);
    if (!g_internal_action) {
        lifecycle_tick();
        /* Start only after SV_SendClientSnapshot has fully returned.
         * Starting from the nested SV_SendMessageToClient hook changes
         * deltaMessage too late for the snapshot currently being built,
         * which was the source of invalid delta bases in v20. */
        start_countdown_candidates_from_seen_clients();
    }
    {
        server_demo_recorder_t *r = recorder_for_client(client);
        if (r && r->active) maybe_finalize_filename(r);
    }
    /* A due recorder is finalized above, after its final snapshot has been
     * enqueued. The VM frame hook handles the no-snapshot grace case. */
}

/* ------------------------------------------------------------------------- */
/* Engine resolution.                                                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    uintptr_t base;
} main_probe_t;

static int main_probe_callback(struct dl_phdr_info *info, size_t size, void *opaque) {
    main_probe_t *p = (main_probe_t *)opaque;
    (void)size;
    if (!info->dlpi_name || *info->dlpi_name) return 0;
    p->base = (uintptr_t)info->dlpi_addr;
    return 1;
}

static uintptr_t find_main_base(void) {
    main_probe_t p;
    memset(&p, 0, sizeof(p));
    dl_iterate_phdr(main_probe_callback, &p);
    return p.base;
}

typedef struct {
    uintptr_t base;
    const uint8_t *pat;
    const char *mask;
    size_t len;
    void *first;
    int hits;
} main_pattern_probe_t;

static int main_pattern_callback(struct dl_phdr_info *info, size_t size, void *opaque) {
    main_pattern_probe_t *p = (main_pattern_probe_t *)opaque;
    size_t j;
    (void)size;
    if ((uintptr_t)info->dlpi_addr != p->base) return 0;
    for (j = 0; j < info->dlpi_phnum; ++j) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[j];
        uintptr_t start;
        int part_hits = 0;
        void *m;
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X) || ph->p_memsz < p->len)
            continue;
        start = (uintptr_t)info->dlpi_addr + (uintptr_t)ph->p_vaddr;
        m = qldr_pattern_search((const void *)start, (size_t)ph->p_memsz,
                                p->pat, p->mask, p->len, &part_hits);
        p->hits += part_hits;
        if (!p->first && m) p->first = m;
    }
    return 0;
}

static void *find_main_pattern(const uint8_t *pat, const char *mask,
                               size_t len, int *hits_out) {
    main_pattern_probe_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.base = g_main_base;
    ctx.pat = pat;
    ctx.mask = mask;
    ctx.len = len;
    dl_iterate_phdr(main_pattern_callback, &ctx);
    if (hits_out) *hits_out = ctx.hits;
    return ctx.first;
}

static int resolve_engine(void) {
    static const uint8_t message_pattern[] = {
        0x41,0x54,0x55,0x48,0x89,0xfd,0x53,0x48,0x89,0xf3,0x48,0x83,0xec,0x20
    };
    static const uint8_t snapshot_pattern[] = {
        0x41,0x57,0x31,0xf6,0xba,0x20,0x00,0x00,0x00,0x41,0x56,0x49,0x89,0xfe
    };
    int hits_message = 0;
    int hits_snapshot = 0;
    void *message;
    void *snapshot;
    uint8_t message_original[14];
    uint8_t snapshot_original[14];
    uintptr_t helper_message;
    uintptr_t helper_gamestate;
    void *setmodule;
    uint8_t setmodule_original[14];

    g_main_base = find_main_base();
    if (!g_main_base && !qldr_address_executable((const void *)QL1069_MSG_WRITE_BYTE_VA)) {
        qldr_log("refusing to install: QLDS main image was not resolved");
        return 0;
    }

    message = find_main_pattern(message_pattern, "XXXXXXXXXXXXXX",
                                sizeof(message_pattern), &hits_message);
    snapshot = find_main_pattern(snapshot_pattern, "XXXXXXXXXXXXXX",
                                 sizeof(snapshot_pattern), &hits_snapshot);
    if (!message || hits_message != 1 || !snapshot || hits_snapshot != 1) {
        qldr_log("refusing to install: QLDS 1069 target patterns not uniquely found (message=%d snapshot=%d)",
                 hits_message, hits_snapshot);
        return 0;
    }

    helper_message = main_va(QL1069_MSG_WRITE_BYTE_VA);
    helper_gamestate = main_va(QL1069_SV_SENDCLIENTGAMESTATE_VA);
    if (!qldr_address_executable((const void *)helper_message) ||
        !qldr_address_executable((const void *)helper_gamestate)) {
        qldr_log("refusing to install: QLDS 1069 helper addresses are not executable");
        return 0;
    }

    g_msg_write_byte = (ql_msg_write_byte_fn)helper_message;
    g_send_gamestate = (ql_send_gamestate_fn)helper_gamestate;

    /* Stock minqlx does not hook either recorder target. The standalone recorder
     * therefore never enters minqlx's function-pointer or VM hook tables. The
     * old recorder-specific minqlx patch is intentionally not supported as a
     * second recorder layer. Its modified target prologue fails the strict
     * signature check above, so installation aborts before any QLDS patching. */
    memcpy(message_original, message, sizeof(message_original));
    memcpy(snapshot_original, snapshot, sizeof(snapshot_original));

    g_send_message = (ql_send_message_fn)message;
    if (!qldr_hook_abs14((void *)g_send_message, (void *)hook_send_message,
                         sizeof(message_original), (void **)&g_send_message)) {
        qldr_log("failed to hook SV_SendMessageToClient");
        return 0;
    }

    g_send_snapshot = (ql_send_snapshot_fn)snapshot;
    if (!qldr_hook_abs14((void *)g_send_snapshot, (void *)hook_send_snapshot,
                         sizeof(snapshot_original), (void **)&g_send_snapshot)) {
        (void)qldr_unhook_abs14(message, message_original, sizeof(message_original));
        g_send_message = NULL;
        qldr_log("failed to hook SV_SendClientSnapshot; previous hook rolled back");
        return 0;
    }

    /* Sys_SetModuleOffset is intentionally hooked last.  When minqlx is present,
     * it has already installed its own hook by the time our constructor runs;
     * our trampoline therefore chains through minqlx's detour and back into QLDS.
     * Without minqlx, the same hook calls the stock QLDS implementation. */
    setmodule = (void *)main_va(QL1069_SYS_SETMODULEOFFSET_VA);
    if (!qldr_range_readable(setmodule, sizeof(setmodule_original)) ||
        !qldr_address_executable(setmodule)) {
        (void)qldr_unhook_abs14(message, message_original, sizeof(message_original));
        qldr_log("failed to resolve Sys_SetModuleOffset at %#" PRIxPTR, (uintptr_t)setmodule);
        return 0;
    }
    memcpy(setmodule_original, setmodule, sizeof(setmodule_original));
    memcpy(g_set_module_saved, setmodule_original, sizeof(g_set_module_saved));
    g_set_module_target = (uintptr_t)setmodule;
    if (!qldr_hook_abs14(setmodule, (void *)qldr_sys_setmoduleoffset,
                         sizeof(setmodule_original), (void **)&g_set_module_offset)) {
        (void)qldr_unhook_abs14(message, message_original, sizeof(message_original));
        (void)qldr_unhook_abs14(snapshot, snapshot_original, sizeof(snapshot_original));
        g_send_message = NULL;
        g_send_snapshot = NULL;
        g_set_module_offset = NULL;
        g_set_module_target = 0;
        qldr_log("failed to hook Sys_SetModuleOffset");
        return 0;
    }
    /* We deliberately do not use the generic trampoline returned above for
     * Sys_SetModuleOffset. The function is called through its fixed main-image
     * address and is chained by temporary restoration of the exact original
     * prologue; this avoids relocating QLDS RIP-relative instructions. */
    g_set_module_offset = NULL;

    qldr_log("engine ready; message=%p snapshot=%p setmodule=%p gamestate=%p",
             (void *)g_send_message, (void *)g_send_snapshot,
             (void *)g_set_module_offset, (void *)g_send_gamestate);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle.                                                                */
/* ------------------------------------------------------------------------- */

void qldr_init(void) {
    memset(g_rec, 0, sizeof(g_rec));
    g_snapshot_client = NULL;
    g_engine_gamestate_client = NULL;
    g_engine_gamestate_recorder = NULL;
    g_engine_gamestate_captured = 0;
    g_internal_action = 0;
    g_ginitgame = 0;
    g_vm_call_table = 0;
    g_level = 0;
    g_match_start_time = INT32_MIN;
    g_countdown_seen = 0;
    g_match_started = 0;
    g_postgame_scheduled = 0;
    g_last_status = -1;
    g_warned_no_level = 0;
    g_snapshot_hook_seen = 0;
    g_message_hook_seen = 0;
    g_last_lifecycle_time = INT32_MIN;
    memset(g_seen_clients, 0, sizeof(g_seen_clients));
    g_file_serial = 0;

    g_enabled = parse_bool_env("QL_DEMO_RECORDER_ENABLED", 1);
    g_debug = parse_bool_env("QL_DEMO_RECORDER_DEBUG", 0);
    g_stop_delay_ms = parse_int_env("QL_DEMO_RECORDER_STOP_DELAY_MS", 4050, 0, 600000);
    {
        const char *path = getenv("QL_DEMO_RECORDER_PATH");
        if (path && *path) {
            strncpy(g_output_path, path, sizeof(g_output_path) - 1);
            g_output_path[sizeof(g_output_path) - 1] = '\0';
        }
    }

    if (!g_enabled) {
        qldr_log("disabled by QL_DEMO_RECORDER_ENABLED=0");
        return;
    }

    if (!resolve_engine()) {
        g_enabled = 0;
        return;
    }
    if (!writer_start()) {
        qldr_log("failed to start dedicated writer thread");
        g_enabled = 0;
        return;
    }

    g_initialized = 1;
    qldr_log("loaded; native-only countdown recorder, stop_delay=%d ms, path=%s",
             g_stop_delay_ms, g_output_path);
}

void qldr_shutdown(int restore_client_state) {
    int i;
    if (!g_initialized) return;
    for (i = 0; i < MDR_MAX_CLIENTS; ++i) {
        if (!g_rec[i].client) continue;
        if (!restore_client_state) g_rec[i].delta_forced = 0;
        reset_record(&g_rec[i], 0);
    }
    writer_stop();
    g_initialized = 0;
}

__attribute__((constructor)) static void qldr_constructor(void) {
    qldr_init();
}

__attribute__((destructor)) static void qldr_destructor(void) {
    qldr_shutdown(1);
}
