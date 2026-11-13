/*
 * machodump - print what idroid's Mach-O parser sees in a file.
 *
 *   machodump FILE             full dump
 *   machodump --imports FILE   one "symbol<TAB>library" line per imported symbol
 *                              (this list feeds the shim coverage check / TRACE.md)
 */
#include "macho.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc(n ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void prot(char out[4], uint32_t p)
{
    out[0] = (p & 1) ? 'r' : '-';
    out[1] = (p & 2) ? 'w' : '-';
    out[2] = (p & 4) ? 'x' : '-';
    out[3] = 0;
}

static const char *import_lib(const macho_t *m, unsigned ordinal)
{
    if (ordinal == 0) return "(self)";
    if (ordinal == 0xfe) return "(dynamic lookup)";
    if (ordinal == 0xff) return "(main executable)";
    if (ordinal <= m->ndylibs) return m->dylibs[ordinal - 1].name;
    return "(?)";
}

static void print_imports(const macho_t *m)
{
    for (uint32_t i = 0; i < m->nsyms; i++) {
        macho_sym_t s;
        if (macho_symbol(m, i, &s) != 0 || !macho_sym_is_import(&s)) continue;
        printf("%s\t%s\n", s.name, import_lib(m, macho_sym_ordinal(&s)));
    }
}

static void print_version(uint32_t v, char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u", v >> 16, (v >> 8) & 0xff, v & 0xff);
}

int main(int argc, char **argv)
{
    int imports_only = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--imports") == 0) imports_only = 1;
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: %s [--imports] FILE\n", argv[0]);
        return 2;
    }

    size_t len;
    uint8_t *buf = slurp(path, &len);
    if (!buf) { fprintf(stderr, "%s: cannot read file\n", path); return 1; }

    macho_t *m = malloc(sizeof *m);
    char err[160];
    if (!m || macho_open(m, buf, len, err, sizeof err) != 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        free(m); free(buf);
        return 1;
    }

    if (imports_only) {
        print_imports(m);
        free(m); free(buf);
        return 0;
    }

    char ver[32];
    printf("file:      %s (%zu bytes)\n", path, len);
    printf("cputype:   0x%08x%s  subtype 0x%x\n", m->cputype,
           m->cputype == MACHO_CPU_TYPE_ARM64 ? " (arm64)" :
           m->cputype == MACHO_CPU_TYPE_X86_64 ? " (x86_64)" : "", m->cpusubtype);
    printf("filetype:  %s  flags 0x%08x\n", macho_filetype_name(m->filetype), m->flags);
    if (m->platform) {
        print_version(m->minos, ver, sizeof ver);
        printf("platform:  %s, minimum OS %s\n", macho_platform_name(m->platform), ver);
    }
    if (m->has_uuid) {
        printf("uuid:      ");
        for (int i = 0; i < 16; i++) printf("%02X%s", m->uuid[i], (i == 3 || i == 5 || i == 7 || i == 9) ? "-" : "");
        printf("\n");
    }
    if (m->dylinker) printf("dylinker:  %s\n", m->dylinker);

    printf("\nload commands (%u, %u bytes):\n", m->ncmds, m->sizeofcmds);
    {
        /* Safe: macho_open already validated every command's bounds. */
        const uint8_t *p = m->buf + 32;
        for (uint32_t i = 0; i < m->ncmds; i++) {
            uint32_t cmd, sz;
            memcpy(&cmd, p, 4);
            memcpy(&sz, p + 4, 4);
            printf("  [%2u] %-24s size %u\n", i, macho_cmd_name(cmd), sz);
            p += sz;
        }
    }

    printf("\nsegments:\n");
    for (uint32_t i = 0; i < m->nsegments; i++) {
        const macho_segment_t *s = &m->segments[i];
        char ip[4], mp[4];
        prot(ip, s->initprot);
        prot(mp, s->maxprot);
        printf("  %-12s vm 0x%010llx +0x%-8llx file 0x%-8llx +0x%-8llx prot %s/%s\n",
               s->name, (unsigned long long)s->vmaddr, (unsigned long long)s->vmsize,
               (unsigned long long)s->fileoff, (unsigned long long)s->filesize, ip, mp);
        for (uint32_t k = 0; k < s->nsects; k++) {
            const macho_section_t *sec = &m->sections[s->first_section + k];
            printf("      %-18s addr 0x%010llx size 0x%-8llx offset 0x%x\n", sec->name,
                   (unsigned long long)sec->addr, (unsigned long long)sec->size, sec->offset);
        }
    }

    printf("\nlinked libraries (%u):\n", m->ndylibs);
    for (uint32_t i = 0; i < m->ndylibs; i++)
        printf("  %2u  %s%s\n", i + 1, m->dylibs[i].name,
               m->dylibs[i].cmd == (0x18u | 0x80000000u) ? "  (weak)" : "");

    printf("\nentry:     ");
    if (m->has_main) printf("LC_MAIN entryoff 0x%llx, stack %llu\n",
                            (unsigned long long)m->entryoff, (unsigned long long)m->stacksize);
    else if (m->has_unixthread) printf("LC_UNIXTHREAD\n");
    else printf("none\n");

    printf("fixups:    %s%s\n",
           m->has_chained_fixups ? "chained fixups" : (m->has_dyld_info ? "classic dyld info" : "none"),
           m->has_chained_fixups ? "" : "");
    if (m->has_chained_fixups)
        printf("           data at 0x%x, %u bytes\n", m->chained_fixups_off, m->chained_fixups_size);

    if (m->has_encryption && m->cryptid != 0)
        printf("encrypted: YES (cryptid %u, range 0x%x +0x%x) - needs a decrypted binary\n",
               m->cryptid, m->cryptoff, m->cryptsize);
    else
        printf("encrypted: no\n");

    uint32_t nimports = 0;
    for (uint32_t i = 0; i < m->nsyms; i++) {
        macho_sym_t s;
        if (macho_symbol(m, i, &s) == 0 && macho_sym_is_import(&s)) nimports++;
    }
    printf("\nimports (%u):\n", nimports);
    for (uint32_t i = 0; i < m->nsyms; i++) {
        macho_sym_t s;
        if (macho_symbol(m, i, &s) != 0 || !macho_sym_is_import(&s)) continue;
        printf("  %-32s %s\n", s.name, import_lib(m, macho_sym_ordinal(&s)));
    }

    free(m);
    free(buf);
    return 0;
}
