#define _GNU_SOURCE
#include "shims.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- libSystem passthroughs ----------------------------------------------
 * Only functions whose Apple and Linux ABIs agree belong here: same argument
 * sizes, no variadics (Apple passes variadic arguments on the stack, AAPCS64
 * in registers, so printf-style functions need real wrappers, not passthroughs).
 * Constants and structs that differ (mmap flags, stat, errno values) need
 * translating shims; those come later and are logged in TRACE.md first.
 */

static void stub_binder(void)
{
    /* All binds are applied eagerly at load time, so lazy-binding stubs never
     * reach dyld_stub_binder. If this runs, the loader has a bug. */
    shim_unresolved_hit("dyld_stub_binder (lazy binding is not supported; all binds should be eager)");
}

/* Fortified (_FORTIFY_SOURCE) variants: Apple's compilers emit these for plain memcpy etc.
 * They take the destination's size as an extra argument and abort on overflow. The
 * signatures are non-variadic and match AAPCS64, so they are ordinary wrappers.
 * A dstlen of (size_t)-1 means "unknown size", which never trips the check. */
static void chk_fail(const char *fn)
{
    fflush(stdout);
    fprintf(stderr, "[idroid] CRASH: %s: buffer overflow detected (fortified libSystem check)\n", fn);
    abort();
}

static void *shim_memcpy_chk(void *d, const void *s, size_t n, size_t dl)   { if (n > dl) chk_fail("__memcpy_chk");  return memcpy(d, s, n); }
static void *shim_memmove_chk(void *d, const void *s, size_t n, size_t dl)  { if (n > dl) chk_fail("__memmove_chk"); return memmove(d, s, n); }
static void *shim_memset_chk(void *d, int c, size_t n, size_t dl)           { if (n > dl) chk_fail("__memset_chk");  return memset(d, c, n); }
static char *shim_strcpy_chk(char *d, const char *s, size_t dl)             { if (strlen(s) + 1 > dl) chk_fail("__strcpy_chk");  return strcpy(d, s); }
static char *shim_strncpy_chk(char *d, const char *s, size_t n, size_t dl)  { if (n > dl) chk_fail("__strncpy_chk"); return strncpy(d, s, n); }

typedef struct { const char *name; void *fn; } shim_t;

static const shim_t table[] = {
    { "_puts",    (void *)puts },
    { "_putchar", (void *)putchar },
    { "_write",   (void *)write },
    { "_read",    (void *)read },
    { "_exit",    (void *)exit },     /* C exit(); C _exit() is "__exit" on Apple */
    { "__exit",   (void *)_exit },
    { "_abort",   (void *)abort },
    { "_malloc",  (void *)malloc },
    { "_calloc",  (void *)calloc },
    { "_realloc", (void *)realloc },
    { "_free",    (void *)free },
    { "_strlen",  (void *)strlen },
    { "_strcmp",  (void *)strcmp },
    { "_strncmp", (void *)strncmp },
    { "_strcpy",  (void *)strcpy },
    { "_strncpy", (void *)strncpy },
    { "_memcpy",  (void *)memcpy },
    { "_memmove", (void *)memmove },
    { "_memset",  (void *)memset },
    { "_memcmp",  (void *)memcmp },
    { "___memcpy_chk",  (void *)shim_memcpy_chk },
    { "___memmove_chk", (void *)shim_memmove_chk },
    { "___memset_chk",  (void *)shim_memset_chk },
    { "___strcpy_chk",  (void *)shim_strcpy_chk },
    { "___strncpy_chk", (void *)shim_strncpy_chk },
    { "_getpid",  (void *)getpid },
    { "dyld_stub_binder", (void *)stub_binder },
};

void *shim_lookup(const char *name)
{
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(table[i].name, name) == 0) return table[i].fn;
    return NULL;
}

/* ---- loud thunks ---------------------------------------------------------
 * Each thunk is 32 bytes of aarch64 code that loads a name pointer into x0 and
 * tail-jumps to shim_unresolved_hit:
 *
 *    +0   ldr x0,  [pc, #16]     ; name
 *    +4   ldr x16, [pc, #20]     ; &shim_unresolved_hit
 *    +8   br  x16
 *    +12  nop
 *    +16  .quad name
 *    +24  .quad shim_unresolved_hit
 *
 * The pool is written while read/write and flipped to read/exec by shim_seal(),
 * so it never needs a writable+executable mapping (Android forbids those).
 */
#define THUNK_SIZE 32u
#define POOL_BYTES 16384u
#define MAX_THUNKS (POOL_BYTES / THUNK_SIZE)

static uint8_t *pool;
static uint32_t nthunks;
static const char *thunk_names[MAX_THUNKS];
static int sealed;

void shim_unresolved_hit(const char *name)
{
    fflush(stdout);
    fprintf(stderr, "[idroid] CRASH: guest called a symbol we do not provide\n- MISSING symbol: %s\n", name);
    abort();
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

void *shim_make_unresolved(const char *name)
{
    for (uint32_t i = 0; i < nthunks; i++)
        if (strcmp(thunk_names[i], name) == 0) return pool + (size_t)i * THUNK_SIZE;

    if (sealed || nthunks >= MAX_THUNKS) return NULL;
    if (!pool) {
        void *p = mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return NULL;
        pool = p;
    }
    uint8_t *t = pool + (size_t)nthunks * THUNK_SIZE;
    put32(t + 0, 0x58000080u);   /* ldr x0,  #16 */
    put32(t + 4, 0x580000B0u);   /* ldr x16, #20 */
    put32(t + 8, 0xD61F0200u);   /* br  x16 */
    put32(t + 12, 0xD503201Fu);  /* nop */
    put64(t + 16, (uint64_t)(uintptr_t)name);
    put64(t + 24, (uint64_t)(uintptr_t)shim_unresolved_hit);
    thunk_names[nthunks] = name;
    nthunks++;
    return t;
}

int shim_seal(void)
{
    if (!pool || sealed) { sealed = 1; return 0; }
    if (mprotect(pool, POOL_BYTES, PROT_READ | PROT_EXEC) != 0) return -1;
    __builtin___clear_cache((char *)pool, (char *)pool + POOL_BYTES);
    sealed = 1;
    return 0;
}
