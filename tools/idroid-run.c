/*
 * idroid-run - load an arm64 Mach-O executable and run its main().
 *
 *   idroid-run FILE [args...]
 *
 * Build for aarch64 Linux/Android (it executes the guest's arm64 code directly;
 * on other hosts run it under qemu-aarch64). Set IDROID_VERBOSE=1 for load stats.
 */
#include "load.h"
#include "macho.h"
#include "runtime.h"
#include "shims.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc(n ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* Binds an import: a shim if we have one, otherwise a loud thunk, logged for TRACE.md. */
static void *resolve(const char *symbol, const char *lib, unsigned flags, void *ctx)
{
    (void)ctx;
    void *p = shim_lookup(symbol);
    if (!p) p = objc_symbol(symbol);   /* objc_msgSend, NSObject, ARC entry points, ... */
    if (p) return p;

    if (flags & MACHO_BIND_WEAK_IMPORT) {
        fprintf(stderr, "- MISSING symbol (weak, bound to NULL): %s (%s)\n", symbol, lib ? lib : "?");
        return NULL;
    }
    p = shim_make_unresolved(symbol);
    if (p) fprintf(stderr, "- MISSING symbol: %s (%s)\n", symbol, lib ? lib : "?");
    return p;
}

int main(int argc, char **argv, char **envp)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE [args...]\n", argv[0]);
        return 2;
    }

    /* A core file is a copy of this process's memory, environment variables (tokens!) included.
     * Guests crash on purpose in tests and by accident in real use, so never write one unless
     * the developer asks: IDROID_CORE=1 keeps the shell's limit. */
    if (!getenv("IDROID_CORE")) {
        struct rlimit nocore = { 0, 0 };
        setrlimit(RLIMIT_CORE, &nocore);
    }

    size_t len;
    uint8_t *buf = slurp(argv[1], &len);
    if (!buf) { fprintf(stderr, "%s: cannot read file\n", argv[1]); return 1; }

    static macho_t m;
    char err[200];
    if (macho_open(&m, buf, len, err, sizeof err) != 0) { fprintf(stderr, "%s: %s\n", argv[1], err); return 1; }

    if (m.cputype != MACHO_CPU_TYPE_ARM64) { fprintf(stderr, "%s: not an arm64 binary (cputype 0x%x)\n", argv[1], m.cputype); return 1; }
    if (m.has_encryption && m.cryptid) {
        fprintf(stderr, "%s: the binary is FairPlay-encrypted; provide a decrypted copy\n", argv[1]);
        return 1;
    }

    for (uint32_t i = 0; i < m.ndylibs; i++) {
        const char *n = m.dylibs[i].name;
        if (strcmp(n, "/usr/lib/libSystem.B.dylib") != 0)
            fprintf(stderr, "- MISSING library: %s\n", n);
    }

    macho_image_t img;
    if (macho_load(&m, resolve, NULL, &img, err, sizeof err) != 0) { fprintf(stderr, "%s: load failed: %s\n", argv[1], err); return 1; }
    if (!img.entry) { fprintf(stderr, "%s: no LC_MAIN entry point\n", argv[1]); return 1; }
    if (shim_seal() != 0) { fprintf(stderr, "cannot seal the thunk pool\n"); return 1; }

    objc_image_stats_t ostats;
    objc_register_image(&m, &img, &ostats);
    if (ostats.categories)   fprintf(stderr, "- MISSING feature: %u Objective-C categories ignored (not implemented)\n", ostats.categories);
    if (ostats.load_methods) fprintf(stderr, "- MISSING feature: %u classes with +load, not called (not implemented)\n", ostats.load_methods);
    if (ostats.selrefs_ro)   fprintf(stderr, "- NOTE: %u selector references are read-only and were not interned\n", ostats.selrefs_ro);

    if (getenv("IDROID_VERBOSE"))
        fprintf(stderr, "[idroid] loaded at %p (slide 0x%lx): %u rebases, %u binds, %u lazy binds, entry %p%s\n",
                (void *)img.base, (unsigned long)img.slide, img.rebases, img.binds, img.lazy_binds,
                (void *)img.entry, img.weak_binds_ignored ? " (weak binds ignored)" : "");
    if (getenv("IDROID_VERBOSE") && ostats.classes)
        fprintf(stderr, "[idroid] objc: %u classes registered, %u selector references interned\n", ostats.classes, ostats.selrefs);

    /* dyld calls LC_MAIN's main(argc, argv, envp, apple) and then exit()s with its result. */
    typedef int (*main_fn)(int, char **, char **, char **);
    char *apple[] = { NULL };
    fflush(NULL);
    int rc = ((main_fn)img.entry)(argc - 1, argv + 1, envp, apple);
    fflush(NULL);
    exit(rc);
}
