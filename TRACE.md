# TRACE

Unresolved symbols, unimplemented selectors and crashes, by app and date. Newest at the bottom. This file is the to-do list. See `AGENTS.md` for the format.

## hello.macho 2026-11-13 (Day 1, from `machodump --imports`)
- MISSING symbol: _puts (libSystem) - needed by Day 2 shim
- MISSING symbol: dyld_stub_binder (libSystem) - lazy-binding helper; the loader must resolve stubs itself or provide this
- NOTE: zig emits classic dyld rebase/bind opcodes, not chained fixups. Real iOS binaries (iOS 15+) usually use chained fixups, so the loader needs both.
