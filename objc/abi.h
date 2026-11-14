/*
 * abi.h - Apple's 64-bit Objective-C metadata layout, and the portable pieces that read it.
 *
 * The guest's compiler emits class objects, class_ro_t structures and method lists as plain data.
 * We read that data in place. Nothing here depends on the host CPU, so it is unit-tested on any host.
 */
#ifndef IDROID_OBJC_ABI_H
#define IDROID_OBJC_ABI_H

#include <stddef.h>
#include <stdint.h>

typedef struct objc_class_s objc_class_t;

/* A class object, as laid out by Apple's compiler (64-bit): 40 bytes. */
struct objc_class_s {
    objc_class_t *isa;        /* the metaclass (for a metaclass: the root metaclass) */
    objc_class_t *superclass;
    void         *cache;      /* points at _objc_empty_cache; we never fill it */
    uintptr_t     cache_mask; /* unused */
    uintptr_t     bits;       /* class_ro_t pointer in the low 47 bits, flag bits below bit 3 */
};

#define OBJC_FAST_DATA_MASK 0x00007ffffffffff8ull

#define RO_META 0x1u
#define RO_ROOT 0x2u

/* class_ro_t: the compile-time half of a class. */
typedef struct {
    uint32_t    flags, instanceStart, instanceSize, reserved;
    const void *ivarLayout;
    const char *name;
    const void *baseMethods;  /* method_list_t* or NULL */
    const void *baseProtocols;
    const void *ivars;
    const void *weakIvarLayout;
    const void *baseProperties;
} objc_ro_t;

_Static_assert(sizeof(objc_class_t) == 40, "class object layout");
_Static_assert(sizeof(objc_ro_t) == 72, "class_ro_t layout");

static inline const objc_ro_t *objc_class_ro(const objc_class_t *c)
{
    return (const objc_ro_t *)(c->bits & OBJC_FAST_DATA_MASK);
}

/* ---- method lists ----------------------------------------------------------------------------
 * Header: { uint32 entsizeAndFlags; uint32 count; } followed by `count` entries. Three formats exist:
 *   large:          24-byte entries { char *name; char *types; IMP imp; }               (older toolchains)
 *   small, indirect: 12-byte entries of three int32 offsets, each relative to ITS OWN address;
 *                    `name` points at a selector-reference slot that holds the selector (iOS 14+ default)
 *   small, direct:   as above, but `name` points straight at the selector string
 */
#define OBJC_ML_ENTSIZE_MASK 0x0000fffcu
#define OBJC_ML_SMALL        0x80000000u
#define OBJC_ML_DIRECT_SEL   0x40000000u

typedef struct {
    const char *name;   /* selector string (not necessarily canonical) */
    const char *types;
    void       *imp;
} objc_method_view_t;

uint32_t objc_mlist_count(const void *list);
/* Decode entry i. Returns 0 on success, -1 if the list is NULL, i is out of range or the entry size is unknown. */
int objc_mlist_get(const void *list, uint32_t i, objc_method_view_t *out);

/* ---- selector interning -------------------------------------------------------------------------
 * Returns one canonical pointer per distinct string, so two selectors are equal iff their pointers are.
 * The canonical string is a private copy: it stays valid even if the image it came from goes away. */
const char *objc_intern(const char *s);
size_t objc_intern_count(void);

#endif
