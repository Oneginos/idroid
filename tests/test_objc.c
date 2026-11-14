/*
 * Unit tests for objc/abi.c: method-list decoding in all three on-disk formats, and selector interning.
 * Everything is built by hand in memory, so this runs on any host under ASan + UBSan, and covers the
 * compact "small" formats that our test compiler does not emit but real iOS binaries do.
 */
#include "abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put_off(uint8_t *field, const void *target)   /* int32 offset relative to the field's own address */
{
    int32_t off = (int32_t)((const uint8_t *)target - field);
    memcpy(field, &off, 4);
}

static void test_large(void)
{
    static const char *n[] = { "answer", "add:to:" };
    static const char *t[] = { "i16@0:8", "i24@0:8i16i20" };
    static uint8_t buf[8 + 2 * 24];
    put32(buf, 24);       /* entsize 24, no flags */
    put32(buf + 4, 2);
    for (int i = 0; i < 2; i++) {
        void *imp = (void *)(uintptr_t)(0x1000 + i * 0x10);
        memcpy(buf + 8 + i * 24, &n[i], 8);
        memcpy(buf + 8 + i * 24 + 8, &t[i], 8);
        memcpy(buf + 8 + i * 24 + 16, &imp, 8);
    }
    objc_method_view_t v;
    CHECK(objc_mlist_count(buf) == 2);
    CHECK(objc_mlist_get(buf, 0, &v) == 0 && strcmp(v.name, "answer") == 0 && strcmp(v.types, "i16@0:8") == 0 && v.imp == (void *)0x1000);
    CHECK(objc_mlist_get(buf, 1, &v) == 0 && strcmp(v.name, "add:to:") == 0 && v.imp == (void *)0x1010);
    CHECK(objc_mlist_get(buf, 2, &v) != 0);   /* one past the end */
}

/* Layout: [header 8][2 entries x 12][strings][selector-reference slots][pretend IMPs] */
static void build_small(uint8_t *buf, int direct)
{
    memset(buf, 0, 256);
    put32(buf, 12 | OBJC_ML_SMALL | (direct ? OBJC_ML_DIRECT_SEL : 0));
    put32(buf + 4, 2);

    char *s0 = (char *)buf + 40, *s1 = (char *)buf + 48;
    strcpy(s0, "tick");
    strcpy(s1, "tock:");
    const char *ty = (char *)buf + 56;
    strcpy((char *)buf + 56, "v16@0:8");
    uint8_t *slot0 = buf + 72, *slot1 = buf + 80;                       /* selector-reference slots (8 bytes each) */
    const char *p0 = s0, *p1 = s1;
    memcpy(slot0, &p0, 8);
    memcpy(slot1, &p1, 8);
    uint8_t *imp0 = buf + 96, *imp1 = buf + 104;

    for (int i = 0; i < 2; i++) {
        uint8_t *e = buf + 8 + i * 12;
        put_off(e, direct ? (void *)(i ? s1 : s0) : (void *)(i ? slot1 : slot0));
        put_off(e + 4, ty);
        put_off(e + 8, i ? imp1 : imp0);
    }
}

static void test_small(int direct)
{
    static uint8_t buf[256];
    build_small(buf, direct);
    objc_method_view_t v;
    CHECK(objc_mlist_count(buf) == 2);
    CHECK(objc_mlist_get(buf, 0, &v) == 0);
    CHECK(strcmp(v.name, "tick") == 0 && strcmp(v.types, "v16@0:8") == 0 && v.imp == (void *)(buf + 96));
    CHECK(objc_mlist_get(buf, 1, &v) == 0);
    CHECK(strcmp(v.name, "tock:") == 0 && v.imp == (void *)(buf + 104));
    CHECK(objc_mlist_get(buf, 2, &v) != 0);
}

static void test_bad_input(void)
{
    objc_method_view_t v;
    CHECK(objc_mlist_count(NULL) == 0);
    CHECK(objc_mlist_get(NULL, 0, &v) != 0);

    uint8_t odd[8 + 16] = {0};                  /* entsize 16 is not a layout we know */
    put32(odd, 16);
    put32(odd + 4, 1);
    CHECK(objc_mlist_get(odd, 0, &v) != 0);

    uint8_t empty[8] = {0};                      /* a valid empty large list */
    put32(empty, 24);
    CHECK(objc_mlist_count(empty) == 0);
    CHECK(objc_mlist_get(empty, 0, &v) != 0);
}

static void test_intern(void)
{
    char a[16], b[16];
    strcpy(a, "setValue:");
    strcpy(b, "setValue:");                      /* same text, different address */
    const char *ca = objc_intern(a), *cb = objc_intern(b);
    CHECK(ca && ca == cb);
    CHECK(ca != a && ca != b);                   /* canonical string is a private copy */
    CHECK(strcmp(ca, "setValue:") == 0);
    CHECK(objc_intern("value") != ca);
    CHECK(objc_intern(NULL) == NULL);
    CHECK(objc_intern("") != NULL);

    /* growth: many strings, earlier pointers must stay valid and stable */
    const char *keep[64];
    char name[32];
    for (int i = 0; i < 64; i++) { snprintf(name, sizeof name, "keep%d:", i); keep[i] = objc_intern(name); }
    size_t before = objc_intern_count();
    for (int i = 0; i < 5000; i++) { snprintf(name, sizeof name, "bulk%d", i); CHECK(objc_intern(name) != NULL); }
    CHECK(objc_intern_count() == before + 5000);
    int stable = 1;
    for (int i = 0; i < 64; i++) {
        snprintf(name, sizeof name, "keep%d:", i);
        if (objc_intern(name) != keep[i] || strcmp(keep[i], name) != 0) stable = 0;
    }
    CHECK(stable);
    CHECK(objc_intern_count() == before + 5000);  /* re-interning added nothing */
}

int main(void)
{
    test_large();
    test_small(0);   /* indirect: name -> selector-reference slot -> string */
    test_small(1);   /* direct:   name -> string */
    test_bad_input();
    test_intern();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
