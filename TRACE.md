# TRACE

Unresolved symbols, unimplemented selectors and crashes, by app and date. Newest at the bottom. This file is the to-do list. See `AGENTS.md` for the format.

## hello.macho 2026-11-13 (Day 1, from `machodump --imports`)
- MISSING symbol: _puts (libSystem) - needed by Day 2 shim
- MISSING symbol: dyld_stub_binder (libSystem) - lazy-binding helper; the loader must resolve stubs itself or provide this
- NOTE: zig emits classic dyld rebase/bind opcodes, not chained fixups. Real iOS binaries (iOS 15+) usually use chained fixups, so the loader needs both.

## guest_shims.macho 2026-11-14 (Day 2)
- MISSING symbol: ___memcpy_chk (libSystem) - fixed: the fortified memcpy/memmove/memset/strcpy/strncpy variants are now shimmed with real size checks
- NOTE: any plain memcpy compiled by Apple-style toolchains may become a ___*_chk call, so expect more of these (___strcat_chk, ___snprintf_chk, ...)

## Open issues found while building Day 2 (not yet fixed)
- DONE (Day 3): variadic functions. printf, sprintf, snprintf, dprintf, fprintf and the fortified ___snprintf_chk/___sprintf_chk are assembly trampolines (shim/variadic_aarch64.S) that rebuild a Linux va_list from Apple's stack layout. vprintf/vsprintf/vsnprintf/vfprintf convert Apple's pointer-style va_list. Still variadic and NOT done: open, fcntl, ioctl, NSLog (needs Objective-C first), scanf family, syslog.
- NOTE: weak-bind opcodes (LC_DYLD_INFO weak_bind) are detected but not processed (`weak_binds_ignored`). C++ and Swift binaries need them.
- NOTE: threaded binds (arm64e) are refused with a clear error. Chained fixups are refused too; most iOS 15+ binaries use them, so this is the next loader task.
- NOTE: only /usr/lib/libSystem.B.dylib is provided. Any other linked library is reported as `MISSING library:` and its symbols bind to loud thunks.

## guest_printf.macho 2026-11-14 (Day 3)
- FIXED: ___stack_chk_guard is a DATA symbol (the stack-protector canary). It was bound to a code thunk, which only worked because nothing wrote to it. Shims now have real data symbols (___stack_chk_guard, ___stdoutp, ___stderrp).
- NOTE: the resolver cannot tell a function import from a data import. Unprovided DATA symbols still get a code thunk, so reading one returns thunk bytes instead of failing. Needs a symbol-kind table or a check against the import's section.
- NOTE: Apple's inline stdio macros (putc, getc, getchar, feof, ferror) read FILE fields directly. FILE here is the host's, so those will misbehave. Not supported.
- NOTE: zig debug builds link a UBSan runtime that adds ~90 imports. Test guests are built with -fno-sanitize=undefined.
- NOTE: on a real app the load-time MISSING list will be hundreds of lines. Add IDROID_TRACE=<file> to write it as TRACE.md-format output instead of stderr.

## objc/probe.m 2026-11-14 (Day 3 probe, tests/objc/probe.m)
- MISSING symbol: _objc_msgSend (flat-namespace import, no library)
- MISSING symbol: __objc_empty_cache (DATA, referenced by the class and metaclass `cache` fields)
- FINDING: the Objective-C compiler works under zig without any Apple SDK. Link with `-Wl,-undefined,dynamic_lookup` (zig has no libobjc stub). Foundation headers are unavailable, so test guests declare their own root classes.
- FINDING: zig's clang emits CLASSIC 24-byte method entries (name, types, imp pointers), not relative/small method lists. Real iOS binaries built for iOS 14+ usually use RELATIVE method lists (entsizeAndFlags bit 31), so the runtime must read both.
- FINDING: metadata sections present: __objc_classlist, __objc_classrefs, __objc_const (class_ro_t + method lists), __objc_data (class objects), __objc_selrefs, __objc_methname/methtype/classname.

## objc runtime 2026-11-14 (Day 3, part 2) - what exists, and what does NOT
- DONE: objc_msgSend / objc_msgSendSuper2 (assembly, preserves x0-x8 and q0-q7), method lookup by selector content with a per-class cache, NSObject (alloc, new, init, dealloc, class, self, retain, release, autorelease, retainCount, isEqual:, hash, isKindOfClass:, respondsToSelector:), instances with reference counts, autorelease pools, the ARC entry points (retain, release, autorelease, storeStrong, alloc, alloc_init, opt_new, the *ReturnValue family), class registry (objc_getClass), and ARC's .cxx_destruct on dealloc.
- DONE: loud failure for an unimplemented selector: `- MISSING selector: -[Class sel]` / `+[Class sel]`, then abort.
- TESTED: classic 24-byte method lists end to end (real compiled guests); small/relative method lists (both selector flavours) only against hand-built lists in tests/test_objc.c. Real iOS 14+ binaries use the small format, so expect to find bugs the first time one runs.
- MISSING: categories (counted and reported at load, not applied). +load is not called. +initialize is not called automatically on first message. Protocols (conformsToProtocol:, protocol method lists). Properties and ivar metadata (class_getInstanceVariable, KVC). Method resolution and forwarding (resolveInstanceMethod:, forwardInvocation:). Class/runtime mutation APIs (class_addMethod, method_exchangeImplementations, objc_allocateClassPair).
- MISSING: blocks (_NSConcreteStackBlock, _Block_copy, _Block_release). Needed before libdispatch or almost any real app.
- MISSING: exceptions (@try/@catch/@throw): objc_exception_throw and the C++ unwinder personality.
- MISSING: weak references (objc_storeWeak, objc_loadWeak), associated objects, tagged pointers (NSNumber, small NSString), non-pointer isa.
- MISSING: NSString, NSLog, NSArray and every other Foundation class. Constant strings (@"...") need ___CFConstantStringClassReference. This is Day 4.
- KNOWN DIFFERENCE: objc_autoreleaseReturnValue / objc_retainAutoreleasedReturnValue are not fused. A returned object stays in the autorelease pool until it drains, where Apple's runtime hands it over directly. Legal under ARC, but object lifetimes can end later than on a real device.
- KNOWN LIMIT: not thread-safe. No locks around the class table, instance table, method cache or interning table. Needs fixing before pthread shims and libdispatch.
- KNOWN LIMIT: live instances are tracked in a side table (anything not in it is treated as immortal) instead of inline refcounts; fine for correctness, slower than the real thing.
- NOTE: the test compiler has no Apple SDK, so guests declare NSObject, BOOL and nil themselves. No Foundation headers means no real Foundation-based guest can be built here yet.

