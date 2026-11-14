/*
 * Tests for loader/load.c. Runs on any Linux host: the loader only builds the
 * image in memory and never executes it, so the arm64 hello binary can be
 * mapped, relocated and inspected even on x86.
 *
 *   test_load path/to/hello.macho
 */
#define _GNU_SOURCE
#include "load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* ---- a resolver that hands out distinct fake addresses and remembers names ---- */

typedef struct { const char *names[32]; int n; int refuse_all; } rctx_t;
static uint8_t fake[32 * 8];

static void *fake_resolver(const char *sym, const char *lib, unsigned flags, void *ctx)
{
    rctx_t *r = ctx;
    (void)lib; (void)flags;
    if (r->refuse_all) return NULL;
    for (int i = 0; i < r->n; i++)
        if (strcmp(r->names[i], sym) == 0) return &fake[i * 8];
    if (r->n < 32) { r->names[r->n] = sym; return &fake[(r->n++) * 8]; }
    return &fake[0];
}

static void *fake_for(const rctx_t *r, const char *sym)
{
    for (int i = 0; i < r->n; i++)
        if (strcmp(r->names[i], sym) == 0) return &fake[i * 8];
    return NULL;
}

static void *any_resolver(const char *sym, const char *lib, unsigned flags, void *ctx)
{
    (void)sym; (void)lib; (void)flags; (void)ctx;
    return &fake[0];
}

static const macho_section_t *section(const macho_t *m, const char *name)
{
    for (uint32_t i = 0; i < m->nsections; i++)
        if (strcmp(m->sections[i].name, name) == 0) return &m->sections[i];
    return NULL;
}

/* permission string ("r-xp") the kernel reports for the page holding addr */
static int perms_at(uintptr_t addr, char out[5])
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[512];
    int rc = -1;
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3 && addr >= s && addr < e) {
            memcpy(out, perms, 4);
            out[4] = 0;
            rc = 0;
            break;
        }
    }
    fclose(f);
    return rc;
}

static void expected_perms(const macho_segment_t *s, char out[4])
{
    out[0] = (s->initprot & 1) ? 'r' : '-';
    out[1] = ((s->initprot & 2) && !(s->flags & 0x10)) ? 'w' : '-';
    out[2] = (s->initprot & 4) ? 'x' : '-';
    out[3] = 0;
}

static void test_hello(const uint8_t *buf, size_t len)
{
    macho_t *m = malloc(sizeof *m);
    macho_image_t img;
    rctx_t r = { .n = 0 };
    char err[200] = "";

    CHECK(macho_open(m, buf, len, err, sizeof err) == 0);
    CHECK(macho_load(m, fake_resolver, &r, &img, err, sizeof err) == 0);
    if (err[0]) fprintf(stderr, "  load error: %s\n", err);
    printf("  loaded: %u rebases, %u binds, %u lazy binds, slide 0x%lx\n",
           img.rebases, img.binds, img.lazy_binds, (unsigned long)img.slide);

    CHECK(img.base != NULL);
    CHECK(img.binds >= 1);       /* dyld_stub_binder in __got */
    CHECK(img.lazy_binds >= 1);  /* _puts through __la_symbol_ptr */
    CHECK(fake_for(&r, "_puts") != NULL);
    CHECK(fake_for(&r, "dyld_stub_binder") != NULL);

    /* the slots hold exactly the addresses the resolver returned */
    const macho_section_t *got = section(m, "__got");
    const macho_section_t *la = section(m, "__la_symbol_ptr");
    const macho_section_t *text = section(m, "__text");
    CHECK(got && la && text);
    if (got && la && text) {
        uint64_t v;
        memcpy(&v, (void *)(uintptr_t)(got->addr + img.slide), 8);
        CHECK(v == (uint64_t)(uintptr_t)fake_for(&r, "dyld_stub_binder"));
        memcpy(&v, (void *)(uintptr_t)(la->addr + img.slide), 8);
        CHECK(v == (uint64_t)(uintptr_t)fake_for(&r, "_puts"));

        /* code was copied byte-for-byte */
        CHECK(memcmp((void *)(uintptr_t)(text->addr + img.slide), buf + text->offset, (size_t)text->size) == 0);
    }

    /* every segment ended up with the protections the file asked for */
    for (uint32_t i = 0; i < m->nsegments; i++) {
        const macho_segment_t *s = &m->segments[i];
        if (s->vmsize == 0 || (s->initprot == 0 && s->filesize == 0)) continue; /* __PAGEZERO is not mapped */
        char want[4], got_p[5];
        expected_perms(s, want);
        CHECK(perms_at((uintptr_t)(s->vmaddr + img.slide), got_p) == 0);
        CHECK(memcmp(got_p, want, 3) == 0);
        if (memcmp(got_p, want, 3) != 0) fprintf(stderr, "  %s: wanted %s, kernel says %s\n", s->name, want, got_p);
    }

    /* entry point is main() inside executable __TEXT */
    const macho_segment_t *t = macho_find_segment(m, "__TEXT");
    CHECK(t != NULL);
    CHECK(img.entry == (uintptr_t)(t->vmaddr + img.slide + m->entryoff));
    char ep[5];
    CHECK(perms_at(img.entry, ep) == 0 && ep[2] == 'x');

    macho_unload(&img);
    CHECK(img.base == NULL);

    /* a second load of the same file works (unload really released the range) */
    CHECK(macho_load(m, fake_resolver, &r, &img, err, sizeof err) == 0);
    macho_unload(&img);
    free(m);
}

static void test_negative(const uint8_t *buf, size_t len)
{
    macho_t *m = malloc(sizeof *m);
    macho_image_t img;
    rctx_t r = { .n = 0 };
    char err[200];

    CHECK(macho_open(m, buf, len, err, sizeof err) == 0);

    /* an unresolved, non-weak symbol fails the load, names the symbol and leaves nothing mapped */
    r.refuse_all = 1;
    err[0] = 0;
    CHECK(macho_load(m, fake_resolver, &r, &img, err, sizeof err) != 0);
    CHECK(img.base == NULL);
    CHECK(strstr(err, "unresolved symbol") != NULL);

    /* chained fixups without classic dyld info are refused with a clear message */
    macho_t *c = malloc(sizeof *c);
    *c = *m;
    c->has_dyld_info = 0;
    c->has_chained_fixups = 1;
    err[0] = 0;
    CHECK(macho_load(c, any_resolver, NULL, &img, err, sizeof err) != 0);
    CHECK(strstr(err, "chained fixups") != NULL);

    /* only executables load */
    *c = *m;
    c->filetype = MACHO_MH_DYLIB;
    CHECK(macho_load(c, any_resolver, NULL, &img, err, sizeof err) != 0);

    /* entry offset outside __TEXT is refused */
    *c = *m;
    c->entryoff = 0x10000000;
    err[0] = 0;
    CHECK(macho_load(c, any_resolver, NULL, &img, err, sizeof err) != 0);
    CHECK(strstr(err, "LC_MAIN") != NULL);
    free(c);

    /* hostile bind stream: a segment index that does not exist.
     * opcodes: SET_SYMBOL "_x"; SET_SEGMENT_AND_OFFSET seg 15, offset 0; DO_BIND */
    uint8_t *copy = malloc(len);
    memcpy(copy, buf, len);
    CHECK(m->bind_size >= 8);
    const uint8_t evil[] = { 0x40, '_', 'x', 0x00, 0x7f, 0x00, 0x90, 0x00 };
    memcpy(copy + m->bind_off, evil, sizeof evil);
    macho_t *m2 = malloc(sizeof *m2);
    CHECK(macho_open(m2, copy, len, err, sizeof err) == 0);
    err[0] = 0;
    CHECK(macho_load(m2, any_resolver, NULL, &img, err, sizeof err) != 0);
    CHECK(strstr(err, "segment index") != NULL);

    /* hostile bind stream: an offset past the end of the segment (SET_SEGMENT seg 2, offset 0x7fffffff) */
    const uint8_t evil2[] = { 0x40, '_', 'x', 0x00, 0x72, 0xff, 0xff, 0xff, 0xff, 0x07, 0x90 };
    memcpy(copy, buf, len);
    CHECK(m->bind_size >= sizeof evil2);
    memcpy(copy + m->bind_off, evil2, sizeof evil2);
    CHECK(macho_open(m2, copy, len, err, sizeof err) == 0);
    err[0] = 0;
    CHECK(macho_load(m2, any_resolver, NULL, &img, err, sizeof err) != 0);
    CHECK(strstr(err, "past the end") != NULL);

    /* hostile rebase stream: a never-ending ULEB count must stop at the segment edge, not loop */
    const uint8_t evil3[] = { 0x11, 0x22, 0x00, 0x60, 0xff, 0xff, 0xff, 0xff, 0x7f }; /* type=pointer, seg2 off0, rebase 2^35 times */
    if (m->rebase_size >= sizeof evil3) {
        memcpy(copy, buf, len);
        memcpy(copy + m->rebase_off, evil3, sizeof evil3);
        CHECK(macho_open(m2, copy, len, err, sizeof err) == 0);
        err[0] = 0;
        CHECK(macho_load(m2, any_resolver, NULL, &img, err, sizeof err) != 0);
        CHECK(strstr(err, "past the end") != NULL);
    }

    free(m2);
    free(copy);
    free(m);
}

/* No crash, no leak of a half-built image, no sanitizer report, whatever the bytes. */
static void test_fuzz(const uint8_t *buf, size_t len)
{
    macho_t *m = malloc(sizeof *m);
    macho_image_t img;
    char err[200];
    uint8_t *mut = malloc(len);
    uint32_t lcg = 987654321u;
    int loaded = 0, rejected = 0;

    for (int round = 0; round < 6000; round++) {
        memcpy(mut, buf, len);
        int flips = 1 + (int)(lcg % 3);
        for (int k = 0; k < flips; k++) {
            lcg = lcg * 1664525u + 1013904223u;
            size_t pos = (lcg >> 8) % len;
            lcg = lcg * 1664525u + 1013904223u;
            mut[pos] = (uint8_t)(lcg >> 16);
        }
        if (macho_open(m, mut, len, err, sizeof err) != 0) { rejected++; continue; }
        if (macho_load(m, any_resolver, NULL, &img, err, sizeof err) == 0) { loaded++; macho_unload(&img); }
        else CHECK(img.base == NULL);
    }
    printf("  fuzz: %d loaded, %d rejected by the parser, rest failed in the loader\n", loaded, rejected);
    free(mut);
    free(m);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s hello.macho\n", argv[0]); return 2; }
    size_t len;
    uint8_t *buf = slurp(argv[1], &len);
    if (!buf) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }

    test_hello(buf, len);
    test_negative(buf, len);
    test_fuzz(buf, len);

    printf("%d checks, %d failures\n", checks, failures);
    free(buf);
    return failures ? 1 : 0;
}
