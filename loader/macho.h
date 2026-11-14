/*
 * macho.h - bounds-checked parser for 64-bit little-endian Mach-O files.
 *
 * Written from the public Mach-O file format; no Apple headers are used.
 * Every offset and size in the input is untrusted: the parser validates it
 * against the buffer before touching it, so a malformed file yields an error
 * and never an out-of-bounds read.
 */
#ifndef IDROID_MACHO_H
#define IDROID_MACHO_H

#include <stddef.h>
#include <stdint.h>

#define MACHO_MAX_SEGMENTS 32
#define MACHO_MAX_SECTIONS 256
#define MACHO_MAX_DYLIBS   128

#define MACHO_CPU_TYPE_ARM64  0x0100000cu
#define MACHO_CPU_TYPE_X86_64 0x01000007u

/* filetype */
#define MACHO_MH_EXECUTE 2u
#define MACHO_MH_DYLIB   6u
#define MACHO_MH_DYLINKER 7u
#define MACHO_MH_BUNDLE  8u

/* platform ids (LC_BUILD_VERSION) */
#define MACHO_PLATFORM_MACOS 1u
#define MACHO_PLATFORM_IOS   2u

typedef struct {
    char     name[17];     /* sectname, NUL-terminated */
    char     segname[17];
    uint64_t addr, size;
    uint32_t offset, align, reloff, nreloc, flags;
} macho_section_t;

typedef struct {
    char     name[17];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    uint32_t maxprot, initprot, nsects, flags;
    uint32_t first_section; /* index into macho_t.sections */
} macho_segment_t;

typedef struct {
    const char *name;  /* points into the input buffer, NUL-terminated */
    uint32_t    cmd;   /* LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, ... */
} macho_dylib_t;

typedef struct {
    const char *name;  /* points into the input buffer, NUL-terminated */
    uint8_t     type, sect;
    uint16_t    desc;
    uint64_t    value;
} macho_sym_t;

typedef struct {
    /* The arm64 (or only) slice. For a fat file this is a view into it. */
    const uint8_t *buf;
    size_t         len;

    uint32_t cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags;

    macho_segment_t segments[MACHO_MAX_SEGMENTS];
    uint32_t        nsegments;
    macho_section_t sections[MACHO_MAX_SECTIONS];
    uint32_t        nsections;
    macho_dylib_t   dylibs[MACHO_MAX_DYLIBS];
    uint32_t        ndylibs;

    int      has_main;       /* LC_MAIN */
    uint64_t entryoff;       /* file-relative offset of main() in __TEXT */
    uint64_t stacksize;
    int      has_unixthread; /* LC_UNIXTHREAD (old-style entry) */

    uint32_t symoff, nsyms, stroff, strsize;
    int      has_symtab;

    int      has_dyld_info;  /* LC_DYLD_INFO(_ONLY): classic rebase/bind opcodes */
    uint32_t rebase_off, rebase_size;
    uint32_t bind_off, bind_size;
    uint32_t weak_bind_off, weak_bind_size;
    uint32_t lazy_bind_off, lazy_bind_size;
    int      has_chained_fixups;
    uint32_t chained_fixups_off, chained_fixups_size;
    int      has_exports_trie;
    uint32_t exports_trie_off, exports_trie_size;

    int      has_uuid;
    uint8_t  uuid[16];

    uint32_t platform;       /* MACHO_PLATFORM_*, 0 if unknown */
    uint32_t minos;          /* x.y.z packed as xxxxyyzz */

    int      has_encryption; /* LC_ENCRYPTION_INFO_64 */
    uint32_t cryptoff, cryptsize, cryptid; /* cryptid != 0 => App Store (FairPlay) encrypted */

    const char *dylinker;    /* LC_LOAD_DYLINKER path, or NULL */
} macho_t;

/*
 * Parse `file` (len bytes). Returns 0 on success, -1 on error with a message
 * in err. `file` must outlive `m`. For a fat file the arm64 slice is chosen.
 */
int macho_open(macho_t *m, const uint8_t *file, size_t len, char *err, size_t errlen);

/* Bounds-checked view of [off, off+size) in the slice, or NULL. */
const uint8_t *macho_data(const macho_t *m, uint64_t off, uint64_t size);

const macho_segment_t *macho_find_segment(const macho_t *m, const char *name);

/* Read symbol `idx` from LC_SYMTAB. Returns 0 on success, -1 if invalid. */
int macho_symbol(const macho_t *m, uint32_t idx, macho_sym_t *out);

/* True for an undefined external symbol, i.e. something to bind at load time. */
int macho_sym_is_import(const macho_sym_t *s);

/* 1-based library ordinal for an import (two-level namespace). 0 = this image,
 * 0xfe = dynamic lookup, 0xff = main executable. */
unsigned macho_sym_ordinal(const macho_sym_t *s);

const char *macho_filetype_name(uint32_t filetype);
const char *macho_cmd_name(uint32_t cmd);
const char *macho_platform_name(uint32_t platform);

#endif
