# 7-day sprint

The bet: get a real iOS binary drawing pixels on Android in a week. Not all of iOS, one thin path through it.

**Ground rules**
- Each day has a goal, a proof, and a kill rule. If you are blocked for more than 4 hours, log it in `TRACE.md` and switch to the next item.
- A day counts as done only when CI proves it. "It works on my machine" doesn't count.
- Unfinished work rolls forward. It never gets quietly dropped. Write it down.

## Day 1: Foundations and the first loader
- Create the repo, a Codespace dev container (clang, lld, qemu-user, zig) and CI.
- Build a hello-world arm64 macOS Mach-O. `zig cc -target aarch64-macos` is the quickest way without a Mac; CI macOS runners are the fallback.
- Mach-O parser: headers, segments, load commands, symbol tables.
- **Proof:** the parser dumps the segments of the hello binary in CI.

## Day 2: Run it
- Loader: map segments, apply rebases, bind imported symbols to shims, jump to the entry point.
- Minimal libSystem shim: `write`, `exit`, `malloc`/`free`, `mmap`, `pthread` basics.
- **Proof:** the hello Mach-O prints to stdout under `qemu-aarch64` in CI.

## Day 3: Objective-C
Decision (2026-11-14): write a small runtime that reads Apple's Objective-C metadata directly, instead of porting objc4.
Why: objc4 depends on dyld shared-cache hooks, os_unfair_lock, pthread TLS keys and Apple-only C++ headers, so porting it is weeks. GNUstep's libobjc2 uses a different data layout than Apple's compiler emits. Everything a first guest needs (class lists, method lists, selector references) is plain data we can read, as `tests/objc/probe.m` shows.
- Done: variadic calling-convention shims (printf family), data symbols, stdio. See TRACE.md.
- Next: `objc/` runtime: `objc_msgSend` (assembly), selector interning, class registration from `__objc_classlist`, `__objc_empty_cache`, NSObject root class, `objc_alloc`/`retain`/`release`.
- libdispatch moves to Day 4: it builds on Linux upstream, so it fits with Foundation.
- **Proof:** the probe runs and prints `answer=42 sum=42`, then a guest subclassing NSObject with an ivar and instance methods runs in CI.

## Day 4: Foundation
- Bring in CoreFoundation and Foundation pieces: strings, arrays, dictionaries, `NSFileManager`, JSON.
- Log every unimplemented symbol to `TRACE.md` automatically.
- **Proof:** a Foundation-only command-line tool runs in CI.

## Day 5: Pixels
- SDL + GLES display layer. Map a minimal set of UIKit and CoreGraphics calls onto it.
- Use CI macOS runners to build an open-source iOS test app (candidates: LÖVE, Doom Classic).
- **Proof:** a window opens and draws a frame, in a screenshot attached to CI.

## Day 6: The phone
- Run everything in Termux on the Galaxy S23 FE. Test the unknowns first: `mmap` with exec permission, linker restrictions, GLES availability.
- Fix what breaks. Log what doesn't.
- **Proof:** the day 2 hello and the day 4 tool run on the phone.

## Day 7: Ship it
- Run the first real app as far as it goes. Count how far it got in `TRACE.md`.
- Write docs, tag `v0.1`, record a short demo.
- **Honest scoreboard:** list what ran, what didn't, and the next ten things in `TRACE.md`.

## Stretch (only if ahead)
Metal-on-Vulkan bridge, a decrypted-IPA loading flow, macOS command-line tools.

## Not in the week
Full UIKit, Metal, and big engines such as Unity or Unreal. They come after the foundation exists.
