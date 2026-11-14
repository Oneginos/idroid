/*
 * load.h - map a parsed Mach-O into this process, apply rebases and binds.
 *
 * The loader never executes guest code. It only builds the image in memory;
 * running main() is the caller's job (see tools/idroid-run.c). That keeps the
 * whole mapping/relocation path testable on any Linux host.
 */
#ifndef IDROID_LOAD_H
#define IDROID_LOAD_H

#include "macho.h"

typedef struct {
    uint8_t  *base;        /* start of the reserved address range */
    size_t    span;        /* its length in bytes */
    uintptr_t slide;       /* add to a vmaddr from the file to get a real address */
    uintptr_t entry;       /* address of main() from LC_MAIN, 0 if there is none */

    uint32_t rebases;      /* statistics, useful in tests and TRACE */
    uint32_t binds;
    uint32_t lazy_binds;
    int      weak_binds_ignored; /* the image has weak-bind info we do not process yet */
} macho_image_t;

#define MACHO_BIND_WEAK_IMPORT 0x1u

/*
 * Called once per bind site. `lib` is the install name of the library the
 * symbol is expected from, or NULL for special ordinals (self/flat lookup).
 * Return the address to bind, or NULL if unresolved. An unresolved weak
 * import (flags & MACHO_BIND_WEAK_IMPORT) binds to 0; any other unresolved
 * symbol fails the load.
 */
typedef void *(*macho_resolver_fn)(const char *symbol, const char *lib, unsigned flags, void *ctx);

/*
 * Map `m` and apply fixups. On success returns 0 and fills `img`. On failure
 * returns -1 with a message in err and leaves nothing mapped.
 * Only classic dyld rebase/bind opcodes are handled; chained fixups are
 * reported as an error (see TRACE.md).
 */
int macho_load(const macho_t *m, macho_resolver_fn resolve, void *ctx,
               macho_image_t *img, char *err, size_t errlen);

void macho_unload(macho_image_t *img);

#endif
