/*
 * Unit tests for loader/macho.c. Build with ASan+UBSan (see Makefile).
 *
 *   test_macho [path/to/real.macho]
 *
 * Synthetic tests build tiny Mach-O images in memory so the malformed cases
 * are exact. If a real arm64 Mach-O is passed, it is parsed too and then
 * mutated byte-by-byte to prove the parser never reads out of bounds.
 */
#include "macho.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- tiny image builder ------------------------------------------------- */

typedef struct { uint8_t b[4096]; size_t n; } img_t;

static void w32(img_t *i, uint32_t v) { memcpy(i->b + i->n, &v, 4); i->n += 4; }
static void w64(img_t *i, uint64_t v) { memcpy(i->b + i->n, &v, 8); i->n += 8; }
static void wname(img_t *i, const char *s) { memset(i->b + i->n, 0, 16); strncpy((char *)i->b + i->n, s, 16); i->n += 16; }
static void wpad(img_t *i, size_t to) { while (i->n < to) i->b[i->n++] = 0; }

static void header(img_t *i, uint32_t ncmds, uint32_t sizeofcmds)
{
    memset(i, 0, sizeof *i);
    w32(i, 0xfeedfacf);
    w32(i, MACHO_CPU_TYPE_ARM64);
    w32(i, 0);
    w32(i, MACHO_MH_EXECUTE);
    w32(i, ncmds);
    w32(i, sizeofcmds);
    w32(i, 0);
    w32(i, 0);
}

/* A segment with `nsects` sections; fileoff/filesize are caller-controlled. */
static void segment(img_t *i, const char *name, uint64_t fileoff, uint64_t filesize, uint32_t nsects)
{
    w32(i, 0x19);
    w32(i, 72 + 80 * nsects);
    wname(i, name);
    w64(i, 0x100000000ull);
    w64(i, 0x4000);
    w64(i, fileoff);
    w64(i, filesize);
    w32(i, 5); w32(i, 5);
    w32(i, nsects);
    w32(i, 0);
    for (uint32_t k = 0; k < nsects; k++) {
        wname(i, "__text"); wname(i, name);
        w64(i, 0x100000400ull); w64(i, 8);
        w32(i, 0x400); w32(i, 2); w32(i, 0); w32(i, 0); w32(i, 0x80000400);
        w32(i, 0); w32(i, 0); w32(i, 0);
    }
}

static void lc_main(img_t *i, uint64_t entryoff)
{
    w32(i, 0x80000028); w32(i, 24); w64(i, entryoff); w64(i, 0);
}

static void lc_dylib(img_t *i, const char *name)
{
    size_t total = (24 + strlen(name) + 1 + 7) & ~(size_t)7;
    w32(i, 0xc); w32(i, (uint32_t)total); w32(i, 24); w32(i, 0); w32(i, 0); w32(i, 0);
    size_t start = i->n;
    memcpy(i->b + i->n, name, strlen(name) + 1);
    i->n += strlen(name) + 1;
    wpad(i, start + (total - 24));
}

static void patch32(img_t *i, size_t off, uint32_t v) { memcpy(i->b + off, &v, 4); }

/* header + __TEXT(1 section) + LC_MAIN + libSystem, file padded to 0x800 */
static void good_image(img_t *i)
{
    header(i, 3, 0);
    size_t cmds_start = i->n;
    segment(i, "__TEXT", 0, 0x800, 1);
    lc_main(i, 0x400);
    lc_dylib(i, "/usr/lib/libSystem.B.dylib");
    patch32(i, 20, (uint32_t)(i->n - cmds_start));
    wpad(i, 0x800);
}

/* ---- tests -------------------------------------------------------------- */

static int open_img(const img_t *i, macho_t *m, char *err)
{
    return macho_open(m, i->b, i->n, err, 160);
}

static void test_good(void)
{
    img_t i; macho_t *m = malloc(sizeof *m); char err[160] = "";
    good_image(&i);
    CHECK(open_img(&i, m, err) == 0);
    CHECK(m->cputype == MACHO_CPU_TYPE_ARM64);
    CHECK(m->filetype == MACHO_MH_EXECUTE);
    CHECK(m->nsegments == 1 && strcmp(m->segments[0].name, "__TEXT") == 0);
    CHECK(m->nsections == 1 && strcmp(m->sections[0].name, "__text") == 0);
    CHECK(m->has_main && m->entryoff == 0x400);
    CHECK(m->ndylibs == 1 && strcmp(m->dylibs[0].name, "/usr/lib/libSystem.B.dylib") == 0);
    CHECK(macho_find_segment(m, "__TEXT") != NULL);
    CHECK(macho_find_segment(m, "__DATA") == NULL);
    CHECK(macho_data(m, 0x400, 8) != NULL);
    CHECK(macho_data(m, 0x7fc, 8) == NULL);
    CHECK(macho_data(m, UINT64_MAX, 8) == NULL);
    free(m);
}

static void expect_reject(const char *what, img_t *i)
{
    macho_t *m = malloc(sizeof *m); char err[160] = "";
    int rc = open_img(i, m, err);
    checks++;
    if (rc == 0) { failures++; fprintf(stderr, "FAIL: malformed image accepted: %s\n", what); }
    else CHECK(err[0] != '\0');
    if (getenv("VERBOSE")) printf("  rejected %-32s -> %s\n", what, err);
    free(m);
}

static void test_malformed(void)
{
    img_t i;

    good_image(&i); i.b[0] ^= 0xff;
    expect_reject("bad magic", &i);

    good_image(&i); patch32(&i, 20, 0x10000);
    expect_reject("sizeofcmds past EOF", &i);

    good_image(&i); patch32(&i, 36, 0);   /* first command's cmdsize = 0 */
    expect_reject("cmdsize 0", &i);

    good_image(&i); patch32(&i, 36, 76);  /* not a multiple of 8 */
    expect_reject("cmdsize unaligned", &i);

    good_image(&i); patch32(&i, 36, 0xfffffff8u);
    expect_reject("cmdsize huge", &i);

    good_image(&i); memcpy(i.b + 32 + 40, &(uint64_t){0x7ff}, 8);   /* fileoff 0x7ff */
    expect_reject("segment file range past EOF", &i);

    good_image(&i); memcpy(i.b + 32 + 40, &(uint64_t){UINT64_MAX - 4}, 8);
    expect_reject("segment fileoff overflow", &i);

    good_image(&i); patch32(&i, 32 + 64, 1000);   /* nsects does not fit the command */
    expect_reject("nsects too large", &i);

    good_image(&i); patch32(&i, 32 + 152 + 24 + 8, 5000); /* dylib name offset out of range */
    expect_reject("dylib name offset", &i);

    {   /* unterminated dylib name: fill the string area with non-NUL */
        good_image(&i);
        size_t dylib_cmd = 32 + 152 + 24;
        for (size_t k = dylib_cmd + 24; k < i.n && k < dylib_cmd + 56; k++) i.b[k] = 'A';
        expect_reject("unterminated dylib name", &i);
    }

    {   /* symtab out of range */
        header(&i, 1, 24);
        w32(&i, 0x2); w32(&i, 24); w32(&i, 0x100000); w32(&i, 100); w32(&i, 0); w32(&i, 0);
        expect_reject("symtab out of range", &i);
    }

    {   /* too small for a header */
        macho_t *m = malloc(sizeof *m); char err[160];
        CHECK(macho_open(m, (const uint8_t *)"\xcf\xfa\xed\xfe", 4, err, sizeof err) != 0);
        CHECK(macho_open(m, (const uint8_t *)"", 0, err, sizeof err) != 0);
        free(m);
    }
}

static void be32w(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* fat header (big-endian) with two arches: [0] cputype_a at 0x0800, [1] cputype_b at 0x1000. */
static void fat_header(uint8_t *fat, uint32_t cputype_a, uint32_t cputype_b, uint32_t size_b)
{
    be32w(fat + 0, 0xcafebabeu);
    be32w(fat + 4, 2);
    be32w(fat + 8 + 0, cputype_a);  be32w(fat + 8 + 4, 0);
    be32w(fat + 8 + 8, 0x0800);     be32w(fat + 8 + 12, 0x400); be32w(fat + 8 + 16, 12);
    be32w(fat + 28 + 0, cputype_b); be32w(fat + 28 + 4, 0);
    be32w(fat + 28 + 8, 0x1000);    be32w(fat + 28 + 12, size_b); be32w(fat + 28 + 16, 12);
}

static void test_fat(void)
{
    img_t good; good_image(&good);
    static uint8_t fat[0x2000];
    macho_t *m = malloc(sizeof *m); char err[160];

    /* x86_64 slice first, arm64 second: the arm64 one must be picked */
    memset(fat, 0, sizeof fat);
    fat_header(fat, MACHO_CPU_TYPE_X86_64, MACHO_CPU_TYPE_ARM64, (uint32_t)good.n);
    memcpy(fat + 0x1000, good.b, good.n);
    CHECK(macho_open(m, fat, sizeof fat, err, sizeof err) == 0);
    CHECK(m->buf == fat + 0x1000);
    CHECK(m->len == good.n);
    CHECK(m->has_main && m->ndylibs == 1);

    /* no arm64 slice at all */
    fat_header(fat, MACHO_CPU_TYPE_X86_64, MACHO_CPU_TYPE_X86_64, (uint32_t)good.n);
    CHECK(macho_open(m, fat, sizeof fat, err, sizeof err) != 0);

    /* arm64 slice claims to extend past the end of the file */
    fat_header(fat, MACHO_CPU_TYPE_X86_64, MACHO_CPU_TYPE_ARM64, 0x2000);
    CHECK(macho_open(m, fat, sizeof fat, err, sizeof err) != 0);

    /* absurd arch count */
    fat_header(fat, MACHO_CPU_TYPE_X86_64, MACHO_CPU_TYPE_ARM64, (uint32_t)good.n);
    be32w(fat + 4, 0x7fffffffu);
    CHECK(macho_open(m, fat, sizeof fat, err, sizeof err) != 0);
    free(m);
}

static void test_real(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { failures++; fprintf(stderr, "FAIL: cannot open %s\n", path); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    CHECK(fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);

    macho_t *m = malloc(sizeof *m); char err[160] = "";
    CHECK(macho_open(m, buf, (size_t)n, err, sizeof err) == 0);
    if (err[0]) fprintf(stderr, "  %s\n", err);
    CHECK(m->cputype == MACHO_CPU_TYPE_ARM64);
    CHECK(m->filetype == MACHO_MH_EXECUTE);
    CHECK(macho_find_segment(m, "__TEXT") != NULL);
    CHECK(macho_find_segment(m, "__LINKEDIT") != NULL);
    CHECK(m->has_main);
    CHECK(m->platform == MACHO_PLATFORM_MACOS);
    CHECK(m->has_encryption == 0 || m->cryptid == 0);
    CHECK(m->ndylibs >= 1);

    int saw_puts = 0, saw_libsystem = 0;
    for (uint32_t i = 0; i < m->nsyms; i++) {
        macho_sym_t s;
        if (macho_symbol(m, i, &s) == 0 && macho_sym_is_import(&s) && strcmp(s.name, "_puts") == 0) {
            saw_puts = 1;
            unsigned ord = macho_sym_ordinal(&s);
            saw_libsystem = ord >= 1 && ord <= m->ndylibs && strstr(m->dylibs[ord - 1].name, "libSystem") != NULL;
        }
    }
    CHECK(saw_puts);
    CHECK(saw_libsystem);

    /* Mutation sweep: every truncation length, then deterministic byte flips.
     * Pass criterion is simply "no crash and no sanitizer report". */
    macho_t *t = malloc(sizeof *t);
    char e2[160];
    for (long cut = 0; cut <= n && cut < 6000; cut++) macho_open(t, buf, (size_t)cut, e2, sizeof e2);
    uint32_t lcg = 12345;
    uint8_t *mut = malloc((size_t)n);
    for (int round = 0; round < 4000; round++) {
        memcpy(mut, buf, (size_t)n);
        lcg = lcg * 1664525u + 1013904223u;
        size_t pos = (lcg >> 8) % (n < 2048 ? (size_t)n : 2048);
        lcg = lcg * 1664525u + 1013904223u;
        mut[pos] = (uint8_t)(lcg >> 16);
        if (macho_open(t, mut, (size_t)n, e2, sizeof e2) == 0) {
            macho_sym_t s;
            for (uint32_t k = 0; k < t->nsyms && k < 64; k++) macho_symbol(t, k, &s);
        }
    }
    free(mut); free(t); free(m); free(buf);
}

int main(int argc, char **argv)
{
    test_good();
    test_malformed();
    test_fat();
    if (argc > 1) test_real(argv[1]);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
