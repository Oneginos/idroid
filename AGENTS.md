# Instructions for AI agents and contributors

Read this file first, every session. You have no memory of earlier sessions. This repo is the memory.

## Goal
Run arm64 iOS Mach-O apps on non-root Android (Termux) through a userspace compatibility layer. See `README.md` and `ROADMAP.md`.

## Before you change anything
1. Read `ROADMAP.md` and find the current day's goal.
2. Read the last 30 lines of `TRACE.md`.
3. Work on exactly one issue. Don't wander.

## Rules
- **No Apple binaries or proprietary code** in the repo. If a task seems to need one, stop and open an issue.
- **No per-app hacks.** Fixes go in the shared layers (`loader/`, `shim/`, `objc/`, `foundation/`, `gfx/`).
- **Every change needs a test** that runs in CI. If you can't test it, say why in the PR.
- **Log failures.** Unresolved symbols and unimplemented selectors go in `TRACE.md` in the format below, never silently stubbed.
- **Stubs must be loud.** A stub that returns a default value must log its name once.
- **Small commits, pushed often.**
- **No root, no sandbox escapes, no ToS circumvention.**

## TRACE.md format
```
## <app or test name> <date>
- MISSING symbol: _CFStringCreateWithCString (from Foundation)
- MISSING selector: -[UIView setNeedsDisplay]
- CRASH: <one-line cause> at <file:line>
```

## Review
The lead reviews diffs against the roadmap. If your change doesn't move the current day's goal, explain why it matters or drop it.

## Style
C11 or C++17 for the loader and shims. Keep dependencies minimal. Comment the non-obvious "why", not the "what".
