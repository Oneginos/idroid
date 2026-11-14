#define _GNU_SOURCE
#include "runtime.h"

#include "abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef objc_class_t *Class;
typedef struct objc_object { Class isa; } *id;
typedef const char *SEL; /* an interned selector string; compare by pointer once interned */
typedef void *IMP;

/* the assembly entry points */
extern void objc_msgSend(void);
extern void objc_msgSendSuper2(void);

typedef struct { id receiver; Class current_class; } objc_super_t;

static inline const objc_ro_t *ro_of(Class c) { return objc_class_ro(c); }
static inline int is_meta(Class c) { return (ro_of(c)->flags & RO_META) != 0; }

/* ---- loud failure -------------------------------------------------------------------------------
 * Same contract as the libSystem shims: never a silent no-op. Output is flushed first so nothing
 * printed before the failure is lost, and the line is in TRACE.md format. */
static void missing_method(id self, SEL sel)
{
    fflush(stdout);
    Class c = self->isa;
    fprintf(stderr, "[idroid] CRASH: guest sent a message the object does not implement\n"
                    "- MISSING selector: %c[%s %s]\n", is_meta(c) ? '+' : '-', ro_of(c)->name, sel);
    abort();
}

/* ---- method lookup ----------------------------------------------------------------------------
 * Selectors are matched by CONTENT, so a selector from any image, ours or a C string all agree, and
 * no guest memory has to be rewritten. The result is cached per (class, selector pointer). */
typedef struct { Class cls; SEL sel; IMP imp; } cache_ent_t;
static cache_ent_t *mcache;
static size_t mcache_cap, mcache_used;

static size_t cache_slot(Class c, SEL s, size_t cap)
{
    uint64_t h = ((uintptr_t)c >> 3) * 0x9e3779b97f4a7c15ull ^ ((uintptr_t)s >> 3) * 0xc2b2ae3d27d4eb4full;
    return (size_t)(h >> 20) & (cap - 1);
}

static IMP cache_get(Class c, SEL s)
{
    if (!mcache_cap) return NULL;
    for (size_t i = cache_slot(c, s, mcache_cap);; i = (i + 1) & (mcache_cap - 1)) {
        if (!mcache[i].cls) return NULL;
        if (mcache[i].cls == c && mcache[i].sel == s) return mcache[i].imp;
    }
}

static void cache_put(Class c, SEL s, IMP imp)
{
    if (mcache_used * 10 >= mcache_cap * 7) {
        size_t ncap = mcache_cap ? mcache_cap * 2 : 1024;
        cache_ent_t *nc = calloc(ncap, sizeof *nc);
        if (!nc) return;                        /* uncached lookups still work */
        for (size_t i = 0; i < mcache_cap; i++) {
            if (!mcache[i].cls) continue;
            size_t j = cache_slot(mcache[i].cls, mcache[i].sel, ncap);
            while (nc[j].cls) j = (j + 1) & (ncap - 1);
            nc[j] = mcache[i];
        }
        free(mcache);
        mcache = nc;
        mcache_cap = ncap;
    }
    size_t i = cache_slot(c, s, mcache_cap);
    while (mcache[i].cls) i = (i + 1) & (mcache_cap - 1);
    mcache[i] = (cache_ent_t){ c, s, imp };
    mcache_used++;
}

static IMP find_in_class(Class c, SEL sel)
{
    const void *list = ro_of(c)->baseMethods;
    uint32_t n = objc_mlist_count(list);
    for (uint32_t i = 0; i < n; i++) {
        objc_method_view_t m;
        if (objc_mlist_get(list, i, &m) == 0 && m.name && strcmp(m.name, sel) == 0) return m.imp;
    }
    return NULL;
}

static IMP lookup_imp(Class cls, SEL sel)
{
    IMP imp = cache_get(cls, sel);
    if (imp) return imp;
    for (Class c = cls; c; c = c->superclass) {
        imp = find_in_class(c, sel);
        if (imp) break;
        if (c->superclass == c) break;          /* defensive: a self-referencing chain */
    }
    if (!imp) return (IMP)missing_method;       /* not cached: the guest is about to abort */
    cache_put(cls, sel, imp);
    return imp;
}

IMP idroid_objc_lookup(id self, SEL sel) { return lookup_imp(self->isa, sel); }

IMP idroid_objc_lookup_super(objc_super_t *s, SEL sel)
{
    return lookup_imp(s->current_class->superclass, sel);
}

/* ---- instances and reference counts -------------------------------------------------------------
 * Live instances are tracked in a table. Anything NOT in it (class objects, constant strings, static
 * data) is immortal, so retain/release on those is a safe no-op. */
typedef struct node { const void *obj; uintptr_t rc; struct node *next; } node_t;
static node_t *buckets[4096];

static node_t *find_node(const void *o)
{
    for (node_t *n = buckets[((uintptr_t)o >> 4) & 4095]; n; n = n->next)
        if (n->obj == o) return n;
    return NULL;
}

static void drop_node(const void *o)
{
    node_t **pp = &buckets[((uintptr_t)o >> 4) & 4095];
    for (; *pp; pp = &(*pp)->next) {
        if ((*pp)->obj == o) { node_t *dead = *pp; *pp = dead->next; free(dead); return; }
    }
}

static id new_instance(Class cls)
{
    size_t size = ro_of(cls)->instanceSize;
    if (size < sizeof(Class)) size = sizeof(Class);
    id o = calloc(1, size);
    node_t *n = calloc(1, sizeof *n);
    if (!o || !n) { free(o); free(n); return NULL; }
    o->isa = cls;
    n->obj = o;
    n->rc = 1;
    n->next = buckets[((uintptr_t)o >> 4) & 4095];
    buckets[((uintptr_t)o >> 4) & 4095] = n;
    return o;
}

static SEL S(const char *name) { return objc_intern(name); }
#define SEND0(o, sel)            ((id (*)(id, SEL))objc_msgSend)((o), (sel))

static id rt_retain(id o)
{
    node_t *n = o ? find_node(o) : NULL;
    if (n) n->rc++;
    return o;
}

static void rt_release(id o)
{
    node_t *n = o ? find_node(o) : NULL;
    if (!n) return;
    if (--n->rc == 0) SEND0(o, S("dealloc"));   /* the root class's -dealloc frees it */
}

/* ---- autorelease pools: a stack of pending releases ---------------------------------------------- */
static id *pool;
static size_t pool_n, pool_cap;

static id rt_autorelease(id o)
{
    if (!o) return o;
    if (pool_n == pool_cap) {
        size_t nc = pool_cap ? pool_cap * 2 : 64;
        id *np = realloc(pool, nc * sizeof *np);
        if (!np) return o;                      /* leak rather than crash */
        pool = np;
        pool_cap = nc;
    }
    pool[pool_n++] = o;
    return o;
}

static void *rt_pool_push(void) { return (void *)(uintptr_t)(pool_n + 1); } /* token is never NULL */

static void rt_pool_pop(void *token)
{
    size_t mark = (size_t)(uintptr_t)token - 1;
    while (pool_n > mark) rt_release(pool[--pool_n]);
}

/* ---- ARC entry points: what compiled code calls for retain/release/alloc ---------------------- */
static id arc_retain(id o) { return rt_retain(o); }
static void arc_release(id o) { rt_release(o); }
static id arc_autorelease(id o) { return rt_autorelease(o); }
static id arc_retain_autoreleased_rv(id o) { return rt_retain(o); }
static id arc_autorelease_rv(id o) { return rt_autorelease(o); }
static id arc_retain_autorelease_rv(id o) { return rt_autorelease(rt_retain(o)); }

static void arc_store_strong(id *loc, id v)
{
    id old = *loc;
    if (old == v) return;
    rt_retain(v);
    *loc = v;
    rt_release(old);
}

/* alloc/new go through real message sends so a class may override +alloc or +new. */
static id arc_alloc(Class c)      { return c ? ((id (*)(id, SEL))objc_msgSend)((id)c, S("alloc")) : NULL; }
static id arc_alloc_init(Class c) { id o = arc_alloc(c); return o ? SEND0(o, S("init")) : NULL; }
static id arc_opt_new(Class c)    { return c ? ((id (*)(id, SEL))objc_msgSend)((id)c, S("new")) : NULL; }

/* ---- class registry ------------------------------------------------------------------------------ */
typedef struct cnode { Class cls; struct cnode *next; } cnode_t;
static cnode_t *class_buckets[512];

static size_t name_hash(const char *s) { size_t h = 5381; while (*s) h = h * 33 + (uint8_t)*s++; return h & 511; }

static void register_class(Class c)
{
    const char *name = ro_of(c)->name;
    for (cnode_t *n = class_buckets[name_hash(name)]; n; n = n->next) if (n->cls == c) return;
    cnode_t *n = calloc(1, sizeof *n);
    if (!n) return;
    n->cls = c;
    n->next = class_buckets[name_hash(name)];
    class_buckets[name_hash(name)] = n;
}

static Class get_class(const char *name)
{
    for (cnode_t *n = class_buckets[name_hash(name)]; n; n = n->next)
        if (strcmp(ro_of(n->cls)->name, name) == 0) return n->cls;
    return NULL;
}

static Class rt_object_getClass(id o) { return o ? o->isa : NULL; }
static const char *rt_class_getName(Class c) { return c ? ro_of(c)->name : "nil"; }

/* ---- NSObject: the root class every guest class derives from ------------------------------------
 * Built here, in the layout the compiler's data expects: instances are one pointer (isa); the class
 * is imported by guests as _OBJC_CLASS_$_NSObject and its metaclass as _OBJC_METACLASS_$_NSObject.
 * Only what real code needs first; the rest is logged in TRACE.md when something asks for it. */
static uint64_t empty_cache[2];   /* _objc_empty_cache: 16 zero bytes, referenced by every class */

static id ns_alloc(id cls, SEL c)       { (void)c; return new_instance((Class)cls); }
static id ns_new(id cls, SEL c)         { (void)c; id o = SEND0((id)cls, S("alloc")); return o ? SEND0(o, S("init")) : NULL; }
static id ns_class_self(id cls, SEL c)  { (void)c; return cls; }
static void ns_noop(id o, SEL c)        { (void)o; (void)c; }
static id ns_init(id o, SEL c)          { (void)c; return o; }
static id ns_self(id o, SEL c)          { (void)c; return o; }
static id ns_instance_class(id o, SEL c)         { (void)c; return (id)o->isa; }
static id ns_retain(id o, SEL c)        { (void)c; return rt_retain(o); }
static void ns_release(id o, SEL c)     { (void)c; rt_release(o); }
static id ns_autorelease(id o, SEL c)   { (void)c; return rt_autorelease(o); }
static uintptr_t ns_retainCount(id o, SEL c) { (void)c; node_t *n = find_node(o); return n ? n->rc : (uintptr_t)-1; }
/* ARC generates a ".cxx_destruct" method per class that releases that class's strong instance
 * variables. The real runtime calls each one, most-derived first, when the root -dealloc runs. Each is
 * looked up in its own class's method list only: a subclass without one must not re-run its parent's. */
static void ns_dealloc(id o, SEL c)
{
    (void)c;
    SEL destruct = S(".cxx_destruct");
    for (Class k = o->isa; k; k = k->superclass) {
        IMP imp = find_in_class(k, destruct);
        if (imp) ((void (*)(id, SEL))imp)(o, destruct);
        if (k->superclass == k) break;
    }
    drop_node(o);
    free(o);
}
static signed char ns_isEqual(id o, SEL c, id other) { (void)c; return o == other; }
static uintptr_t ns_hash(id o, SEL c)   { (void)c; return (uintptr_t)o; }

static signed char ns_isKindOfClass(id o, SEL c, id cls)
{
    (void)c;
    for (Class k = o->isa; k; k = k->superclass) { if ((id)k == cls) return 1; if (k->superclass == k) break; }
    return 0;
}

static signed char ns_respondsToSelector(id o, SEL c, SEL sel)
{
    (void)c;
    for (Class k = o->isa; k; k = k->superclass) { if (find_in_class(k, sel)) return 1; if (k->superclass == k) break; }
    return 0;
}

typedef struct { const char *name, *types; void *imp; } m_t;
typedef struct { uint32_t entsize, count; m_t m[16]; } mlist_t;

static const mlist_t ns_class_methods = { 24, 5, {
    { "alloc",      "@16@0:8", (void *)ns_alloc },
    { "new",        "@16@0:8", (void *)ns_new },
    { "class",      "#16@0:8", (void *)ns_class_self },
    { "self",       "@16@0:8", (void *)ns_class_self },
    { "initialize", "v16@0:8", (void *)ns_noop },
}};

static const mlist_t ns_inst_methods = { 24, 12, {
    { "init",             "@16@0:8",        (void *)ns_init },
    { "dealloc",          "v16@0:8",        (void *)ns_dealloc },
    { "class",            "#16@0:8",        (void *)ns_instance_class },
    { "self",             "@16@0:8",        (void *)ns_self },
    { "retain",           "@16@0:8",        (void *)ns_retain },
    { "release",          "Vv16@0:8",       (void *)ns_release },
    { "autorelease",      "@16@0:8",        (void *)ns_autorelease },
    { "retainCount",      "Q16@0:8",        (void *)ns_retainCount },
    { "isEqual:",         "B24@0:8@16",     (void *)ns_isEqual },
    { "hash",             "Q16@0:8",        (void *)ns_hash },
    { "isKindOfClass:",   "B24@0:8#16",     (void *)ns_isKindOfClass },
    { "respondsToSelector:", "B24@0:8:16",  (void *)ns_respondsToSelector },
}};

static objc_class_t ns_class, ns_meta;
static const objc_ro_t ns_ro = { .flags = RO_ROOT, .instanceStart = 8, .instanceSize = 8,
                                 .name = "NSObject", .baseMethods = &ns_inst_methods };
static const objc_ro_t ns_meta_ro = { .flags = RO_META | RO_ROOT, .instanceStart = 40, .instanceSize = 40,
                                      .name = "NSObject", .baseMethods = &ns_class_methods };

static objc_class_t ns_meta  = { &ns_meta,  &ns_class, empty_cache, 0, (uintptr_t)&ns_meta_ro };
static objc_class_t ns_class = { &ns_meta,  NULL,      empty_cache, 0, (uintptr_t)&ns_ro };

/* ---- symbols the loader can bind guest imports to ---------------------------------------------- */
typedef struct { const char *name; void *addr; } sym_t;

static const sym_t symbols[] = {
    { "_objc_msgSend",        (void *)objc_msgSend },
    { "_objc_msgSendSuper2",  (void *)objc_msgSendSuper2 },
    { "__objc_empty_cache",   (void *)empty_cache },          /* data */
    { "_OBJC_CLASS_$_NSObject",     (void *)&ns_class },      /* data */
    { "_OBJC_METACLASS_$_NSObject", (void *)&ns_meta },       /* data */
    { "_objc_retain",                        (void *)arc_retain },
    { "_objc_release",                       (void *)arc_release },
    { "_objc_autorelease",                   (void *)arc_autorelease },
    { "_objc_retainAutoreleasedReturnValue", (void *)arc_retain_autoreleased_rv },
    { "_objc_autoreleaseReturnValue",        (void *)arc_autorelease_rv },
    { "_objc_retainAutoreleaseReturnValue",  (void *)arc_retain_autorelease_rv },
    { "_objc_storeStrong",                   (void *)arc_store_strong },
    { "_objc_alloc",                         (void *)arc_alloc },
    { "_objc_alloc_init",                    (void *)arc_alloc_init },
    { "_objc_opt_new",                       (void *)arc_opt_new },
    { "_objc_autoreleasePoolPush",           (void *)rt_pool_push },
    { "_objc_autoreleasePoolPop",            (void *)rt_pool_pop },
    { "_objc_getClass",                      (void *)get_class },
    { "_objc_lookUpClass",                   (void *)get_class },
    { "_sel_registerName",                   (void *)objc_intern },
    { "_sel_getName",                        (void *)objc_intern },  /* a SEL is its own name */
    { "_object_getClass",                    (void *)rt_object_getClass },
    { "_class_getName",                      (void *)rt_class_getName },
};

void *objc_symbol(const char *name)
{
    for (size_t i = 0; i < sizeof symbols / sizeof symbols[0]; i++)
        if (strcmp(symbols[i].name, name) == 0) return symbols[i].addr;
    return NULL;
}

/* ---- registering a loaded image ---------------------------------------------------------------- */
static const macho_section_t *find_section(const macho_t *m, const char *name)
{
    for (uint32_t i = 0; i < m->nsections; i++)
        if (strcmp(m->sections[i].name, name) == 0) return &m->sections[i];
    return NULL;
}

/* Does the segment holding this section end up writable? Mirrors the protections macho_load applied. */
static int section_writable(const macho_t *m, const macho_section_t *sec)
{
    const macho_segment_t *seg = macho_find_segment(m, sec->segname);
    return seg && (seg->initprot & 2u) && !(seg->flags & 0x10u);
}

int objc_register_image(const macho_t *m, const macho_image_t *img, objc_image_stats_t *stats)
{
    objc_image_stats_t local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof *stats);

    register_class(&ns_class);                   /* NSObject is always available */

    const macho_section_t *cl = find_section(m, "__objc_classlist");
    if (cl) {
        Class *list = (Class *)(uintptr_t)(cl->addr + img->slide);
        for (uint64_t i = 0; i < cl->size / sizeof(Class); i++) {
            if (!list[i] || !ro_of(list[i])->name) continue;
            register_class(list[i]);
            stats->classes++;
        }
    }

    /* Selector references: compiled code loads a SEL from one of these slots and passes it to
     * objc_msgSend. Make each hold the canonical pointer, so guest code that compares selectors by
     * pointer (sel == @selector(x)) agrees with the runtime. Lookup itself does not depend on this. */
    const macho_section_t *sr = find_section(m, "__objc_selrefs");
    if (sr) {
        if (!section_writable(m, sr)) {
            stats->selrefs_ro = (uint32_t)(sr->size / sizeof(SEL));
        } else {
            SEL *slots = (SEL *)(uintptr_t)(sr->addr + img->slide);
            for (uint64_t i = 0; i < sr->size / sizeof(SEL); i++) {
                const char *canon = slots[i] ? objc_intern(slots[i]) : NULL;
                if (canon) { slots[i] = canon; stats->selrefs++; }
            }
        }
    }

    const macho_section_t *cat = find_section(m, "__objc_catlist");
    if (cat) stats->categories = (uint32_t)(cat->size / sizeof(void *));
    const macho_section_t *nl = find_section(m, "__objc_nlclslist");
    if (nl) stats->load_methods = (uint32_t)(nl->size / sizeof(void *));
    return 0;
}
