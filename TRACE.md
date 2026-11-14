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
