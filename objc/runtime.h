/*
 * runtime.h - a small Objective-C runtime that reads Apple's compiler-emitted metadata in place.
 * It is aarch64-only (objc_msgSend is assembly) and is used by idroid-run.
 */
#ifndef IDROID_OBJC_RUNTIME_H
#define IDROID_OBJC_RUNTIME_H

#include "load.h"
#include "macho.h"

/* Address of a runtime-provided symbol (a function or a data symbol such as _OBJC_CLASS_$_NSObject),
 * or NULL. Use it as a second resolver after the libSystem shims. */
void *objc_symbol(const char *name);

typedef struct {
    uint32_t classes;      /* classes registered from __objc_classlist */
    uint32_t selrefs;      /* selector references interned in place */
    uint32_t selrefs_ro;   /* selector references left alone because their segment is read-only */
    uint32_t categories;   /* __objc_catlist entries seen but NOT applied (not implemented) */
    uint32_t load_methods; /* classes with +load seen but NOT called (not implemented) */
} objc_image_stats_t;

/* Register an image's classes and selectors. Call after macho_load and before running the guest. */
int objc_register_image(const macho_t *m, const macho_image_t *img, objc_image_stats_t *stats);

#endif
