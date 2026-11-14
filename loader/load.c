#define _GNU_SOURCE
#include "load.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SG_READ_ONLY 0x10u
#define MAX_SPAN (8ull << 30) /* refuse to reserve more than 8 GiB of address space */

/* classic dyld opcode groups (high nibble) */
#define REBASE_OPCODE_DONE                               0x00
#define REBASE_OPCODE_SET_TYPE_IMM                       0x10
#define REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB        0x20
#define REBASE_OPCODE_ADD_ADDR_ULEB                      0x30
#define REBASE_OPCODE_ADD_ADDR_IMM_SCALED                0x40
#define REBASE_OPCODE_DO_REBASE_IMM_TIMES                0x50
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES               0x60
#define REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB            0x70
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB 0x80

#define BIND_OPCODE_DONE                                 0x00
#define BIND_OPCODE_SET_DYLIB_ORDINAL_IMM                0x10
#define BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB               0x20
#define BIND_OPCODE_SET_DYLIB_SPECIAL_IMM                0x30
#define BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM        0x40
#define BIND_OPCODE_SET_TYPE_IMM                         0x50
#define BIND_OPCODE_SET_ADDEND_SLEB                      0x60
#define BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB          0x70
#define BIND_OPCODE_ADD_ADDR_ULEB                        0x80
#define BIND_OPCODE_DO_BIND                              0x90
#define BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB                0xA0
#define BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED          0xB0
#define BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB     0xC0
#define BIND_OPCODE_THREADED                             0xD0

#define POINTER_TYPE 1 /* REBASE_TYPE_POINTER == BIND_TYPE_POINTER */

typedef struct {
    const macho_t *m;
    macho_image_t *img;
    uint64_t       lo;                          /* lowest mapped vmaddr */
    int            mapped[MACHO_MAX_SEGMENTS];  /* segment is part of the image */
    macho_resolver_fn resolve;
    void          *rctx;
    char          *err;
    size_t         errlen;
} ctx_t;

static int fail(ctx_t *c, const char *fmt, ...)
{
    if (c->err && c->errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->err, c->errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static uint64_t round_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

static int uleb(const uint8_t **pp, const uint8_t *end, uint64_t *out)
{
    uint64_t v = 0;
    unsigned shift = 0;
    const uint8_t *p = *pp;
    for (;;) {
        if (p >= end || shift > 63) return -1;
        uint8_t b = *p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    *pp = p;
    *out = v;
    return 0;
}

static int sleb(const uint8_t **pp, const uint8_t *end, int64_t *out)
{
    uint64_t v = 0;
    unsigned shift = 0;
    const uint8_t *p = *pp;
    uint8_t b;
    do {
        if (p >= end || shift > 63) return -1;
        b = *p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) v |= ~(uint64_t)0 << shift;
    *pp = p;
    *out = (int64_t)v;
    return 0;
}

/* Address of an 8-byte slot at (segment, offset), or NULL with an error set. */
static uint8_t *slot(ctx_t *c, uint64_t seg, uint64_t off)
{
    if (seg >= c->m->nsegments) { fail(c, "segment index %llu is out of range", (unsigned long long)seg); return NULL; }
    const macho_segment_t *s = &c->m->segments[seg];
    if (!c->mapped[seg]) { fail(c, "fixup targets unmapped segment %s", s->name); return NULL; }
    if (off > s->vmsize || s->vmsize - off < 8) { fail(c, "fixup offset 0x%llx is past the end of %s", (unsigned long long)off, s->name); return NULL; }
    return c->img->base + (s->vmaddr - c->lo) + off;
}

static int run_rebase(ctx_t *c)
{
    const macho_t *m = c->m;
    if (m->rebase_size == 0) return 0;
    const uint8_t *p = macho_data(m, m->rebase_off, m->rebase_size);
    if (!p) return fail(c, "rebase info is outside the file");
    const uint8_t *end = p + m->rebase_size;

    uint64_t seg = 0, off = 0;
    unsigned type = 0;
    int have_seg = 0;

#define DO_REBASE() do { \
        if (type != POINTER_TYPE) return fail(c, "unsupported rebase type %u", type); \
        if (!have_seg) return fail(c, "rebase before a segment was set"); \
        uint8_t *a = slot(c, seg, off); \
        if (!a) return -1; \
        uint64_t v; memcpy(&v, a, 8); v += c->img->slide; memcpy(a, &v, 8); \
        c->img->rebases++; \
    } while (0)

    while (p < end) {
        uint8_t op = *p++;
        unsigned imm = op & 0x0fu;
        uint64_t n, skip;
        switch (op & 0xf0u) {
        case REBASE_OPCODE_DONE:
            break;
        case REBASE_OPCODE_SET_TYPE_IMM:
            type = imm;
            break;
        case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            seg = imm; have_seg = 1;
            if (uleb(&p, end, &off) != 0) return fail(c, "truncated rebase opcode");
            break;
        case REBASE_OPCODE_ADD_ADDR_ULEB:
            if (uleb(&p, end, &n) != 0) return fail(c, "truncated rebase opcode");
            off += n;
            break;
        case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
            off += (uint64_t)imm * 8;
            break;
        case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
            for (unsigned i = 0; i < imm; i++) { DO_REBASE(); off += 8; }
            break;
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
            if (uleb(&p, end, &n) != 0) return fail(c, "truncated rebase opcode");
            for (uint64_t i = 0; i < n; i++) { DO_REBASE(); off += 8; } /* slot() ends it at the segment edge */
            break;
        case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
            if (uleb(&p, end, &n) != 0) return fail(c, "truncated rebase opcode");
            DO_REBASE();
            off += n + 8;
            break;
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
            if (uleb(&p, end, &n) != 0 || uleb(&p, end, &skip) != 0) return fail(c, "truncated rebase opcode");
            for (uint64_t i = 0; i < n; i++) { DO_REBASE(); off += skip + 8; }
            break;
        default:
            return fail(c, "unknown rebase opcode 0x%02x", op);
        }
    }
#undef DO_REBASE
    return 0;
}

static int run_bind(ctx_t *c, uint32_t off0, uint32_t size, int lazy)
{
    const macho_t *m = c->m;
    if (size == 0) return 0;
    const uint8_t *p = macho_data(m, off0, size);
    if (!p) return fail(c, "bind info is outside the file");
    const uint8_t *end = p + size;

    uint64_t seg = 0, off = 0;
    int have_seg = 0;
    int ordinal = 0;
    unsigned flags = 0, type = POINTER_TYPE;
    const char *name = NULL;
    int64_t addend = 0;

#define DO_BIND() do { \
        if (!name) return fail(c, "bind without a symbol name"); \
        if (type != POINTER_TYPE) return fail(c, "unsupported bind type %u", type); \
        if (!have_seg) return fail(c, "bind before a segment was set"); \
        const char *lib = NULL; \
        if (ordinal > 0) { \
            if ((uint32_t)ordinal > m->ndylibs) return fail(c, "bind ordinal %d has no matching library", ordinal); \
            lib = m->dylibs[ordinal - 1].name; \
        } \
        uint8_t *a = slot(c, seg, off); \
        if (!a) return -1; \
        void *target = c->resolve(name, lib, flags, c->rctx); \
        if (!target && !(flags & MACHO_BIND_WEAK_IMPORT)) return fail(c, "unresolved symbol %s", name); \
        uint64_t v = target ? (uint64_t)(uintptr_t)target + (uint64_t)addend : 0; \
        memcpy(a, &v, 8); \
        if (lazy) c->img->lazy_binds++; else c->img->binds++; \
    } while (0)

    while (p < end) {
        uint8_t op = *p++;
        unsigned imm = op & 0x0fu;
        uint64_t n, skip;
        switch (op & 0xf0u) {
        case BIND_OPCODE_DONE:
            break; /* lazy info has one DONE per entry, so keep going to the end */
        case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
            ordinal = (int)imm;
            break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
            if (uleb(&p, end, &n) != 0 || n > 0xffff) return fail(c, "bad bind ordinal");
            ordinal = (int)n;
            break;
        case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
            ordinal = imm == 0 ? 0 : (int)(int8_t)(0xf0u | imm); /* 0 self, -1 main exe, -2 flat lookup */
            break;
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
            flags = imm;
            const uint8_t *nul = memchr(p, '\0', (size_t)(end - p));
            if (!nul) return fail(c, "bind symbol name is not terminated");
            name = (const char *)p;
            p = nul + 1;
            break;
        }
        case BIND_OPCODE_SET_TYPE_IMM:
            type = imm;
            break;
        case BIND_OPCODE_SET_ADDEND_SLEB:
            if (sleb(&p, end, &addend) != 0) return fail(c, "truncated bind opcode");
            break;
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            seg = imm; have_seg = 1;
            if (uleb(&p, end, &off) != 0) return fail(c, "truncated bind opcode");
            break;
        case BIND_OPCODE_ADD_ADDR_ULEB:
            if (uleb(&p, end, &n) != 0) return fail(c, "truncated bind opcode");
            off += n;
            break;
        case BIND_OPCODE_DO_BIND:
            DO_BIND();
            off += 8;
            break;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
            if (uleb(&p, end, &n) != 0) return fail(c, "truncated bind opcode");
            DO_BIND();
            off += n + 8;
            break;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
            DO_BIND();
            off += (uint64_t)imm * 8 + 8;
            break;
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
            if (uleb(&p, end, &n) != 0 || uleb(&p, end, &skip) != 0) return fail(c, "truncated bind opcode");
            for (uint64_t i = 0; i < n; i++) { DO_BIND(); off += skip + 8; }
            break;
        case BIND_OPCODE_THREADED:
            return fail(c, "threaded binds (arm64e pointer authentication) are not supported");
        default:
            return fail(c, "unknown bind opcode 0x%02x", op);
        }
    }
#undef DO_BIND
    return 0;
}

static int is_pagezero(const macho_segment_t *s)
{
    return s->vmsize == 0 || (s->initprot == 0 && s->maxprot == 0 && s->filesize == 0 && s->fileoff == 0);
}

int macho_load(const macho_t *m, macho_resolver_fn resolve, void *rctx,
               macho_image_t *img, char *err, size_t errlen)
{
    memset(img, 0, sizeof *img);
    ctx_t c = { .m = m, .img = img, .resolve = resolve, .rctx = rctx, .err = err, .errlen = errlen };
    const uint64_t pg = (uint64_t)sysconf(_SC_PAGESIZE);

    if (m->filetype != MACHO_MH_EXECUTE) return fail(&c, "only executables can be loaded (filetype %s)", macho_filetype_name(m->filetype));
    if (m->has_chained_fixups && !m->has_dyld_info)
        return fail(&c, "this binary uses chained fixups, which are not implemented yet (see TRACE.md)");

    /* 1. work out the address range we need */
    uint64_t lo = UINT64_MAX, hi = 0;
    for (uint32_t i = 0; i < m->nsegments; i++) {
        const macho_segment_t *s = &m->segments[i];
        if (is_pagezero(s)) continue;
        if (s->vmsize < s->filesize) return fail(&c, "segment %s is smaller in memory than in the file", s->name);
        if (s->vmaddr + s->vmsize < s->vmaddr) return fail(&c, "segment %s wraps the address space", s->name);
        c.mapped[i] = 1;
        if (s->vmaddr < lo) lo = s->vmaddr;
        if (s->vmaddr + s->vmsize > hi) hi = s->vmaddr + s->vmsize;
    }
    if (lo == UINT64_MAX) return fail(&c, "no loadable segments");
    if (hi - lo > MAX_SPAN) return fail(&c, "image needs more than 8 GiB of address space");
    c.lo = lo;

    for (uint32_t i = 0; i < m->nsegments; i++) {
        if (!c.mapped[i]) continue;
        const macho_segment_t *a = &m->segments[i];
        if ((a->vmaddr - lo) % pg) return fail(&c, "segment %s is not aligned to the host page size", a->name);
        for (uint32_t j = i + 1; j < m->nsegments; j++) {
            if (!c.mapped[j]) continue;
            const macho_segment_t *b = &m->segments[j];
            uint64_t a_end = a->vmaddr + round_up(a->vmsize, pg), b_end = b->vmaddr + round_up(b->vmsize, pg);
            if (a->vmaddr < b_end && b->vmaddr < a_end) return fail(&c, "segments %s and %s overlap", a->name, b->name);
        }
    }

    /* 2. reserve the range, then make each segment writable and copy it in */
    size_t span = (size_t)round_up(hi - lo, pg);
    void *base = mmap(NULL, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) return fail(&c, "cannot reserve %zu bytes of address space", span);
    img->base = base;
    img->span = span;
    img->slide = (uintptr_t)base - (uintptr_t)lo;

    for (uint32_t i = 0; i < m->nsegments; i++) {
        if (!c.mapped[i]) continue;
        const macho_segment_t *s = &m->segments[i];
        uint8_t *mem = img->base + (s->vmaddr - lo);
        if (mprotect(mem, (size_t)round_up(s->vmsize, pg), PROT_READ | PROT_WRITE) != 0) {
            fail(&c, "cannot make segment %s writable", s->name);
            goto bad;
        }
        if (s->filesize) memcpy(mem, m->buf + s->fileoff, (size_t)s->filesize); /* range checked by macho_open */
    }

    /* 3. fixups (all binds are eager: lazy ones are resolved now, so stubs never need dyld_stub_binder) */
    if (m->has_dyld_info) {
        if (run_rebase(&c) != 0) goto bad;
        if (run_bind(&c, m->bind_off, m->bind_size, 0) != 0) goto bad;
        if (run_bind(&c, m->lazy_bind_off, m->lazy_bind_size, 1) != 0) goto bad;
        if (m->weak_bind_size) img->weak_binds_ignored = 1;
    }

    /* 4. entry point: LC_MAIN's offset is relative to the start of the file, which __TEXT maps */
    if (m->has_main) {
        for (uint32_t i = 0; i < m->nsegments; i++) {
            const macho_segment_t *s = &m->segments[i];
            if (!c.mapped[i] || s->fileoff != 0 || s->filesize == 0) continue;
            if (!(s->initprot & 4u) || m->entryoff >= s->filesize) { fail(&c, "LC_MAIN entry is not inside executable code"); goto bad; }
            img->entry = (uintptr_t)(s->vmaddr + img->slide + m->entryoff);
            break;
        }
        if (!img->entry) { fail(&c, "LC_MAIN entry has no __TEXT segment to live in"); goto bad; }
    }

    /* 5. final protections; read-only-after-fixup segments lose write permission */
    for (uint32_t i = 0; i < m->nsegments; i++) {
        if (!c.mapped[i]) continue;
        const macho_segment_t *s = &m->segments[i];
        uint8_t *mem = img->base + (s->vmaddr - lo);
        size_t len = (size_t)round_up(s->vmsize, pg);
        int prot = 0;
        if (s->initprot & 1u) prot |= PROT_READ;
        if ((s->initprot & 2u) && !(s->flags & SG_READ_ONLY)) prot |= PROT_WRITE;
        if (s->initprot & 4u) prot |= PROT_EXEC;
        if (mprotect(mem, len, prot) != 0) { fail(&c, "cannot protect segment %s", s->name); goto bad; }
        if (prot & PROT_EXEC) __builtin___clear_cache((char *)mem, (char *)mem + len);
    }
    return 0;

bad:
    macho_unload(img);
    return -1;
}

void macho_unload(macho_image_t *img)
{
    if (img->base) munmap(img->base, img->span);
    memset(img, 0, sizeof *img);
}
