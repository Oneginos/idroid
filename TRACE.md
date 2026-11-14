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
- NOTE: variadic functions (printf, NSLog, open, fcntl, ioctl) cannot be plain passthroughs. Apple arm64 passes variadic arguments on the stack; AAPCS64 on Linux/Android passes them in registers. They need wrappers that read the guest's stack layout. This blocks anything that prints with a format string.
- NOTE: weak-bind opcodes (LC_DYLD_INFO weak_bind) are detected but not processed (`weak_binds_ignored`). C++ and Swift binaries need them.
- NOTE: threaded binds (arm64e) are refused with a clear error. Chained fixups are refused too; most iOS 15+ binaries use them, so this is the next loader task.
- NOTE: only /usr/lib/libSystem.B.dylib is provided. Any other linked library is reported as `MISSING library:` and its symbols bind to loud thunks.
