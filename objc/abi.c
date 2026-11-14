#include "abi.h"

#include <stdlib.h>
#include <string.h>

uint32_t objc_mlist_count(const void *list)
{
    if (!list) return 0;
    uint32_t h[2];
    memcpy(h, list, sizeof h);
    return h[1];
}

int objc_mlist_get(const void *list, uint32_t i, objc_method_view_t *out)
{
    if (!list) return -1;
    uint32_t h[2];
    memcpy(h, list, sizeof h);
    uint32_t flags = h[0], count = h[1];
    if (i >= count) return -1;

    const uint8_t *base = (const uint8_t *)list + 8;

    if (flags & OBJC_ML_SMALL) {
        const uint8_t *e = base + (size_t)i * 12;
        int32_t name_off, types_off, imp_off;
        memcpy(&name_off, e, 4);
        memcpy(&types_off, e + 4, 4);
        memcpy(&imp_off, e + 8, 4);
        /* every offset is relative to the address of the field that holds it */
        const uint8_t *name_target = e + name_off;
        if (flags & OBJC_ML_DIRECT_SEL) {
            out->name = (const char *)name_target;
        } else {
            const char *sel;
            memcpy(&sel, name_target, sizeof sel); /* name_target is a selector-reference slot */
            out->name = sel;
        }
        out->types = (const char *)(e + 4 + types_off);
        out->imp = (void *)(uintptr_t)(e + 8 + imp_off);
        return 0;
    }

    size_t entsize = flags & OBJC_ML_ENTSIZE_MASK;
    if (entsize != 24) return -1; /* the only large layout we know */
    const uint8_t *e = base + (size_t)i * entsize;
    memcpy(&out->name, e, 8);
    memcpy(&out->types, e + 8, 8);
    memcpy(&out->imp, e + 16, 8);
    return 0;
}

/* ---- interning: open addressing, FNV-1a, grows at 70% load ------------------------------------- */

static const char **table;
static size_t cap, used;

static uint64_t hash_str(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
    return h;
}

static int grow(void)
{
    size_t ncap = cap ? cap * 2 : 256;
    const char **nt = calloc(ncap, sizeof *nt);
    if (!nt) return -1;
    for (size_t i = 0; i < cap; i++) {
        if (!table[i]) continue;
        size_t j = hash_str(table[i]) & (ncap - 1);
        while (nt[j]) j = (j + 1) & (ncap - 1);
        nt[j] = table[i];
    }
    free(table);
    table = nt;
    cap = ncap;
    return 0;
}

const char *objc_intern(const char *s)
{
    if (!s) return NULL;
    if (used * 10 >= cap * 7 && grow() != 0) return NULL;
    size_t i = hash_str(s) & (cap - 1);
    while (table[i]) {
        if (strcmp(table[i], s) == 0) return table[i];
        i = (i + 1) & (cap - 1);
    }
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (!copy) return NULL;
    memcpy(copy, s, len);
    table[i] = copy;
    used++;
    return copy;
}

size_t objc_intern_count(void) { return used; }
