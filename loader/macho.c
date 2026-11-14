#include "macho.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "macho.c assumes a little-endian host"
#endif

#define MH_MAGIC_64    0xfeedfacfu
#define MH_CIGAM_64    0xcffaedfeu
#define MH_MAGIC_32    0xfeedfaceu
#define FAT_MAGIC_LE   0xbebafecau /* 0xcafebabe read as little-endian */
#define FAT_MAGIC_64_LE 0xbfbafecau

#define LC_REQ_DYLD 0x80000000u
#define LC_SEGMENT_64 0x19u
#define LC_SYMTAB 0x2u
#define LC_UNIXTHREAD 0x5u
#define LC_LOAD_DYLIB 0xcu
#define LC_ID_DYLIB 0xdu
#define LC_LOAD_DYLINKER 0xeu
#define LC_UUID 0x1bu
#define LC_VERSION_MIN_MACOSX 0x24u
#define LC_VERSION_MIN_IPHONEOS 0x25u
#define LC_FUNCTION_STARTS 0x26u
#define LC_SOURCE_VERSION 0x2au
#define LC_ENCRYPTION_INFO_64 0x2cu
#define LC_VERSION_MIN_TVOS 0x2fu
#define LC_VERSION_MIN_WATCHOS 0x30u
#define LC_BUILD_VERSION 0x32u
#define LC_DYLD_INFO (0x22u)
#define LC_DYLD_INFO_ONLY (0x22u | LC_REQ_DYLD)
#define LC_LOAD_WEAK_DYLIB (0x18u | LC_REQ_DYLD)
#define LC_RPATH (0x1cu | LC_REQ_DYLD)
#define LC_REEXPORT_DYLIB (0x1fu | LC_REQ_DYLD)
#define LC_MAIN (0x28u | LC_REQ_DYLD)
#define LC_DYLD_EXPORTS_TRIE (0x33u | LC_REQ_DYLD)
#define LC_DYLD_CHAINED_FIXUPS (0x34u | LC_REQ_DYLD)
#define LC_DYSYMTAB 0xbu
#define LC_CODE_SIGNATURE 0x1du

#define S_ZEROFILL 0x1u
#define S_GB_ZEROFILL 0xcu
#define S_THREAD_LOCAL_ZEROFILL 0x12u

#define HDR_SIZE 32u
#define SEG64_SIZE 72u
#define SECT64_SIZE 80u
#define NLIST64_SIZE 16u

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || n == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void copy16(char dst[17], const uint8_t *src)
{
    memcpy(dst, src, 16);
    dst[16] = '\0';
}

/* [off, off+size) lies inside a buffer of len bytes, without overflow. */
static int in_range(uint64_t off, uint64_t size, size_t len)
{
    return off <= len && size <= len - off;
}

/* Pointer to a NUL-terminated string inside [start, end), else NULL. */
static const char *bounded_str(const uint8_t *start, const uint8_t *end)
{
    if (start >= end) return NULL;
    return memchr(start, '\0', (size_t)(end - start)) ? (const char *)start : NULL;
}

const uint8_t *macho_data(const macho_t *m, uint64_t off, uint64_t size)
{
    return in_range(off, size, m->len) ? m->buf + off : NULL;
}

static int parse_slice(macho_t *m, char *err, size_t errlen)
{
    const uint8_t *b = m->buf;
    size_t len = m->len;

    if (len < HDR_SIZE) { seterr(err, errlen, "file too small for a Mach-O header"); return -1; }
    uint32_t magic = rd32(b);
    if (magic == MH_CIGAM_64) { seterr(err, errlen, "big-endian Mach-O is not supported"); return -1; }
    if (magic == MH_MAGIC_32) { seterr(err, errlen, "32-bit Mach-O is not supported"); return -1; }
    if (magic != MH_MAGIC_64) { seterr(err, errlen, "not a Mach-O file (magic 0x%08x)", magic); return -1; }

    m->cputype    = rd32(b + 4);
    m->cpusubtype = rd32(b + 8);
    m->filetype   = rd32(b + 12);
    m->ncmds      = rd32(b + 16);
    m->sizeofcmds = rd32(b + 20);
    m->flags      = rd32(b + 24);

    uint64_t cmds_end = (uint64_t)HDR_SIZE + m->sizeofcmds;
    if (cmds_end > len) { seterr(err, errlen, "load commands extend past end of file"); return -1; }

    uint64_t off = HDR_SIZE;
    for (uint32_t i = 0; i < m->ncmds; i++) {
        if (off + 8 > cmds_end) { seterr(err, errlen, "load command %u is truncated", i); return -1; }
        uint32_t cmd = rd32(b + off);
        uint32_t cmdsize = rd32(b + off + 4);
        if (cmdsize < 8 || (cmdsize & 7u) || off + cmdsize > cmds_end) {
            seterr(err, errlen, "load command %u has invalid size %u", i, cmdsize);
            return -1;
        }
        const uint8_t *c = b + off;
        const uint8_t *cend = c + cmdsize;

        switch (cmd) {
        case LC_SEGMENT_64: {
            if (cmdsize < SEG64_SIZE) { seterr(err, errlen, "LC_SEGMENT_64 too small"); return -1; }
            if (m->nsegments >= MACHO_MAX_SEGMENTS) { seterr(err, errlen, "too many segments"); return -1; }
            macho_segment_t *s = &m->segments[m->nsegments];
            copy16(s->name, c + 8);
            s->vmaddr   = rd64(c + 24);
            s->vmsize   = rd64(c + 32);
            s->fileoff  = rd64(c + 40);
            s->filesize = rd64(c + 48);
            s->maxprot  = rd32(c + 56);
            s->initprot = rd32(c + 60);
            s->nsects   = rd32(c + 64);
            s->flags    = rd32(c + 68);
            if (s->filesize && !in_range(s->fileoff, s->filesize, len)) {
                seterr(err, errlen, "segment %s file range is outside the file", s->name);
                return -1;
            }
            if (s->nsects > (cmdsize - SEG64_SIZE) / SECT64_SIZE) {
                seterr(err, errlen, "segment %s section count does not fit its command", s->name);
                return -1;
            }
            if (s->nsects > MACHO_MAX_SECTIONS - m->nsections) {
                seterr(err, errlen, "too many sections");
                return -1;
            }
            s->first_section = m->nsections;
            for (uint32_t k = 0; k < s->nsects; k++) {
                const uint8_t *sp = c + SEG64_SIZE + (size_t)k * SECT64_SIZE;
                macho_section_t *sec = &m->sections[m->nsections++];
                copy16(sec->name, sp);
                copy16(sec->segname, sp + 16);
                sec->addr   = rd64(sp + 32);
                sec->size   = rd64(sp + 40);
                sec->offset = rd32(sp + 48);
                sec->align  = rd32(sp + 52);
                sec->reloff = rd32(sp + 56);
                sec->nreloc = rd32(sp + 60);
                sec->flags  = rd32(sp + 64);
                uint32_t type = sec->flags & 0xffu;
                int zerofill = type == S_ZEROFILL || type == S_GB_ZEROFILL || type == S_THREAD_LOCAL_ZEROFILL;
                if (!zerofill && sec->offset != 0 && !in_range(sec->offset, sec->size, len)) {
                    seterr(err, errlen, "section %s,%s file range is outside the file", sec->segname, sec->name);
                    return -1;
                }
            }
            m->nsegments++;
            break;
        }
        case LC_SYMTAB: {
            if (cmdsize < 24) { seterr(err, errlen, "LC_SYMTAB too small"); return -1; }
            m->symoff = rd32(c + 8);
            m->nsyms = rd32(c + 12);
            m->stroff = rd32(c + 16);
            m->strsize = rd32(c + 20);
            if (!in_range(m->symoff, (uint64_t)m->nsyms * NLIST64_SIZE, len) ||
                !in_range(m->stroff, m->strsize, len)) {
                seterr(err, errlen, "symbol table is outside the file");
                return -1;
            }
            m->has_symtab = 1;
            break;
        }
        case LC_LOAD_DYLIB:
        case LC_LOAD_WEAK_DYLIB:
        case LC_REEXPORT_DYLIB: {
            if (cmdsize < 24) { seterr(err, errlen, "dylib command too small"); return -1; }
            if (m->ndylibs >= MACHO_MAX_DYLIBS) { seterr(err, errlen, "too many dylibs"); return -1; }
            uint32_t nameoff = rd32(c + 8);
            if (nameoff < 12 || nameoff >= cmdsize) { seterr(err, errlen, "dylib name offset out of range"); return -1; }
            const char *nm = bounded_str(c + nameoff, cend);
            if (!nm) { seterr(err, errlen, "dylib name is not terminated"); return -1; }
            m->dylibs[m->ndylibs].name = nm;
            m->dylibs[m->ndylibs].cmd = cmd;
            m->ndylibs++;
            break;
        }
        case LC_LOAD_DYLINKER: {
            if (cmdsize < 12) { seterr(err, errlen, "dylinker command too small"); return -1; }
            uint32_t nameoff = rd32(c + 8);
            if (nameoff < 12 || nameoff >= cmdsize) { seterr(err, errlen, "dylinker name offset out of range"); return -1; }
            const char *nm = bounded_str(c + nameoff, cend);
            if (!nm) { seterr(err, errlen, "dylinker name is not terminated"); return -1; }
            m->dylinker = nm;
            break;
        }
        case LC_MAIN:
            if (cmdsize < 24) { seterr(err, errlen, "LC_MAIN too small"); return -1; }
            m->has_main = 1;
            m->entryoff = rd64(c + 8);
            m->stacksize = rd64(c + 16);
            break;
        case LC_UNIXTHREAD:
            m->has_unixthread = 1;
            break;
        case LC_UUID:
            if (cmdsize < 24) { seterr(err, errlen, "LC_UUID too small"); return -1; }
            memcpy(m->uuid, c + 8, 16);
            m->has_uuid = 1;
            break;
        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY: {
            if (cmdsize < 48) { seterr(err, errlen, "LC_DYLD_INFO too small"); return -1; }
            m->has_dyld_info = 1;
            m->rebase_off = rd32(c + 8);        m->rebase_size = rd32(c + 12);
            m->bind_off = rd32(c + 16);         m->bind_size = rd32(c + 20);
            m->weak_bind_off = rd32(c + 24);    m->weak_bind_size = rd32(c + 28);
            m->lazy_bind_off = rd32(c + 32);    m->lazy_bind_size = rd32(c + 36);
            if (!in_range(m->rebase_off, m->rebase_size, len) ||
                !in_range(m->bind_off, m->bind_size, len) ||
                !in_range(m->weak_bind_off, m->weak_bind_size, len) ||
                !in_range(m->lazy_bind_off, m->lazy_bind_size, len)) {
                seterr(err, errlen, "dyld info is outside the file");
                return -1;
            }
            break;
        }
        case LC_DYLD_CHAINED_FIXUPS:
        case LC_DYLD_EXPORTS_TRIE: {
            if (cmdsize < 16) { seterr(err, errlen, "linkedit data command too small"); return -1; }
            uint32_t doff = rd32(c + 8), dsize = rd32(c + 12);
            if (!in_range(doff, dsize, len)) { seterr(err, errlen, "linkedit data is outside the file"); return -1; }
            if (cmd == LC_DYLD_CHAINED_FIXUPS) {
                m->has_chained_fixups = 1;
                m->chained_fixups_off = doff;
                m->chained_fixups_size = dsize;
            } else {
                m->has_exports_trie = 1;
                m->exports_trie_off = doff;
                m->exports_trie_size = dsize;
            }
            break;
        }
        case LC_ENCRYPTION_INFO_64:
            if (cmdsize < 24) { seterr(err, errlen, "LC_ENCRYPTION_INFO_64 too small"); return -1; }
            m->has_encryption = 1;
            m->cryptoff = rd32(c + 8);
            m->cryptsize = rd32(c + 12);
            m->cryptid = rd32(c + 16);
            break;
        case LC_BUILD_VERSION:
            if (cmdsize < 24) { seterr(err, errlen, "LC_BUILD_VERSION too small"); return -1; }
            m->platform = rd32(c + 8);
            m->minos = rd32(c + 12);
            break;
        case LC_VERSION_MIN_MACOSX:
        case LC_VERSION_MIN_IPHONEOS:
        case LC_VERSION_MIN_TVOS:
        case LC_VERSION_MIN_WATCHOS:
            if (cmdsize < 16) { seterr(err, errlen, "version-min command too small"); return -1; }
            if (m->platform == 0) {
                m->platform = cmd == LC_VERSION_MIN_MACOSX ? 1u :
                              cmd == LC_VERSION_MIN_IPHONEOS ? 2u :
                              cmd == LC_VERSION_MIN_TVOS ? 3u : 4u;
                m->minos = rd32(c + 8);
            }
            break;
        default:
            break; /* unknown commands are skipped, as dyld does for non-required ones */
        }
        off += cmdsize;
    }
    return 0;
}

int macho_open(macho_t *m, const uint8_t *file, size_t len, char *err, size_t errlen)
{
    memset(m, 0, sizeof *m);
    if (len < 4) { seterr(err, errlen, "file too small"); return -1; }

    m->buf = file;
    m->len = len;

    uint32_t magic = rd32(file);
    if (magic == FAT_MAGIC_LE) {
        if (len < 8) { seterr(err, errlen, "truncated fat header"); return -1; }
        uint32_t n = be32(file + 4);
        if (n == 0 || n > 16) { seterr(err, errlen, "implausible fat arch count %u", n); return -1; }
        if (!in_range(8, (uint64_t)n * 20, len)) { seterr(err, errlen, "truncated fat arch table"); return -1; }
        int found = 0;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *a = file + 8 + (size_t)i * 20;
            if (be32(a) != MACHO_CPU_TYPE_ARM64) continue;
            uint32_t off = be32(a + 8), size = be32(a + 12);
            if (!in_range(off, size, len) || size < HDR_SIZE) {
                seterr(err, errlen, "arm64 slice is outside the file");
                return -1;
            }
            m->buf = file + off;
            m->len = size;
            found = 1;
            break;
        }
        if (!found) { seterr(err, errlen, "fat file has no arm64 slice"); return -1; }
    } else if (magic == FAT_MAGIC_64_LE) {
        seterr(err, errlen, "64-bit fat files are not supported yet");
        return -1;
    }
    return parse_slice(m, err, errlen);
}

const macho_segment_t *macho_find_segment(const macho_t *m, const char *name)
{
    for (uint32_t i = 0; i < m->nsegments; i++)
        if (strcmp(m->segments[i].name, name) == 0) return &m->segments[i];
    return NULL;
}

int macho_symbol(const macho_t *m, uint32_t idx, macho_sym_t *out)
{
    if (!m->has_symtab || idx >= m->nsyms) return -1;
    const uint8_t *p = macho_data(m, (uint64_t)m->symoff + (uint64_t)idx * NLIST64_SIZE, NLIST64_SIZE);
    if (!p) return -1;
    uint32_t strx = rd32(p);
    if (strx >= m->strsize) return -1;
    const uint8_t *strtab = macho_data(m, m->stroff, m->strsize);
    if (!strtab) return -1;
    const char *nm = bounded_str(strtab + strx, strtab + m->strsize);
    if (!nm) return -1;
    out->name  = nm;
    out->type  = p[4];
    out->sect  = p[5];
    out->desc  = rd16(p + 6);
    out->value = rd64(p + 8);
    return 0;
}

int macho_sym_is_import(const macho_sym_t *s)
{
    /* N_STAB bits clear, N_TYPE == N_UNDF, N_EXT set */
    return (s->type & 0xe0u) == 0 && (s->type & 0x0eu) == 0 && (s->type & 0x01u) != 0;
}

unsigned macho_sym_ordinal(const macho_sym_t *s)
{
    return (unsigned)((s->desc >> 8) & 0xffu);
}

const char *macho_filetype_name(uint32_t t)
{
    switch (t) {
    case 1: return "OBJECT";
    case 2: return "EXECUTE";
    case 3: return "FVMLIB";
    case 4: return "CORE";
    case 5: return "PRELOAD";
    case 6: return "DYLIB";
    case 7: return "DYLINKER";
    case 8: return "BUNDLE";
    case 9: return "DYLIB_STUB";
    case 10: return "DSYM";
    case 11: return "KEXT_BUNDLE";
    case 12: return "FILESET";
    default: return "UNKNOWN";
    }
}

const char *macho_cmd_name(uint32_t cmd)
{
    switch (cmd) {
    case LC_SEGMENT_64: return "LC_SEGMENT_64";
    case LC_SYMTAB: return "LC_SYMTAB";
    case LC_DYSYMTAB: return "LC_DYSYMTAB";
    case LC_UNIXTHREAD: return "LC_UNIXTHREAD";
    case LC_LOAD_DYLIB: return "LC_LOAD_DYLIB";
    case LC_ID_DYLIB: return "LC_ID_DYLIB";
    case LC_LOAD_DYLINKER: return "LC_LOAD_DYLINKER";
    case LC_UUID: return "LC_UUID";
    case LC_CODE_SIGNATURE: return "LC_CODE_SIGNATURE";
    case LC_VERSION_MIN_MACOSX: return "LC_VERSION_MIN_MACOSX";
    case LC_VERSION_MIN_IPHONEOS: return "LC_VERSION_MIN_IPHONEOS";
    case LC_FUNCTION_STARTS: return "LC_FUNCTION_STARTS";
    case 0x29u: return "LC_DATA_IN_CODE";
    case LC_SOURCE_VERSION: return "LC_SOURCE_VERSION";
    case LC_ENCRYPTION_INFO_64: return "LC_ENCRYPTION_INFO_64";
    case LC_BUILD_VERSION: return "LC_BUILD_VERSION";
    case LC_DYLD_INFO: return "LC_DYLD_INFO";
    case LC_DYLD_INFO_ONLY: return "LC_DYLD_INFO_ONLY";
    case LC_LOAD_WEAK_DYLIB: return "LC_LOAD_WEAK_DYLIB";
    case LC_RPATH: return "LC_RPATH";
    case LC_REEXPORT_DYLIB: return "LC_REEXPORT_DYLIB";
    case LC_MAIN: return "LC_MAIN";
    case LC_DYLD_EXPORTS_TRIE: return "LC_DYLD_EXPORTS_TRIE";
    case LC_DYLD_CHAINED_FIXUPS: return "LC_DYLD_CHAINED_FIXUPS";
    default: return "LC_?";
    }
}

const char *macho_platform_name(uint32_t p)
{
    switch (p) {
    case 1: return "macOS";
    case 2: return "iOS";
    case 3: return "tvOS";
    case 4: return "watchOS";
    case 5: return "bridgeOS";
    case 6: return "Mac Catalyst";
    case 7: return "iOS Simulator";
    case 8: return "tvOS Simulator";
    case 9: return "watchOS Simulator";
    case 11: return "visionOS";
    default: return "unknown";
    }
}
