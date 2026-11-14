# iDroid (working name)

Run iOS (arm64 Mach-O) apps on Android, **one app at a time**, with no emulator, no root and no reboot.

The CPU is already arm64, so nothing needs emulating. What iOS apps need is the operating system under them. We reimplement that layer in userspace, growing it from what real apps actually call.

## How it works

```
 iOS app (Mach-O, arm64)
        |
   Mach-O loader  ----  maps the binary + dylibs, applies fixups, binds imports
        |
   libSystem shim ----  Apple libc/syscalls -> bionic/Linux
        |
   Obj-C runtime  ----  adapted from open-source objc4 (+ libdispatch)
        |
   Foundation     ----  swift-corelibs-foundation / CoreFoundation
        |
   Graphics       ----  SDL + GLES first, UIKit/Metal-on-Vulkan later
        |
   Android (Termux, non-root)
```

## Rules

1. **One app at a time.** Every app runs through the shared runtime. No per-app hacks. If an app needs a fix, the fix goes into the layer.
2. **Failure is data.** Every unresolved symbol and unimplemented selector is logged to `TRACE.md`. That file is the to-do list.
3. **Open source, clean.** No Apple binaries in this repo, ever. Test apps are open-source builds produced by CI, or free apps you extract from your own account.
4. **No root, no cards, no ToS games.** Everything runs as a normal Termux process.
5. **Push early, push often.** The repo is the memory. Nothing important lives only on a phone.

## Layout

| Path | What |
|---|---|
| `loader/` | Mach-O parser and loader |
| `shim/` | libSystem shims |
| `objc/` | Objective-C runtime port |
| `foundation/` | Foundation / CoreFoundation pieces |
| `gfx/` | SDL/GLES display layer |
| `tools/` | `machodump` (inspect a Mach-O; `--imports` lists what it needs from the OS) and `idroid-run` (load and run an arm64 Mach-O) |
| `apps/` | per-app notes and test scripts (no binaries) |
| `tests/` | CI test suite |
| `TRACE.md` | unresolved symbols and selectors, by app |

## Try it

```bash
sudo apt-get install -y clang make qemu-user python3-pip && pip install --break-system-packages ziglang
make test       # unit tests (ASan + UBSan), any Linux host
make run-test   # runs real arm64 macOS binaries through the loader under qemu-aarch64
make run        # prints: hello from a Mach-O
```

On an arm64 host such as Termux, build the runner natively: `make RUN_CC=cc run-test`.

## Status

See [ROADMAP.md](ROADMAP.md). Contributors, human or AI: read [AGENTS.md](AGENTS.md) first.

## Prior art

Darling (macOS on Linux), Wine, Cycada (iOS apps on Android, research), HackiOS (real XNU kernel on phone hardware). We learn from all of them and copy none of their approach blindly.

## License

TBD (suggest Apache-2.0 or MIT for new code; objc4 and other ported parts keep their own licenses).
