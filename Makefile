CC      ?= cc
ZIGCC   ?= python3 -m ziglang cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -O1 -Iloader -Ishim
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=undefined
BUILD   := build

# idroid-run executes arm64 guest code, so it is an aarch64 Linux program.
# Cross-built static with zig by default; on an arm64 host (Termux) use: make RUN_CC=cc
RUN_CC  ?= $(ZIGCC) -target aarch64-linux-musl

LOADER  := loader/macho.c loader/load.c
SHIMS   := shim/shims.c shim/variadic_aarch64.S
HEADERS := loader/macho.h loader/load.h shim/shims.h
GUESTS  := $(BUILD)/hello.macho $(BUILD)/guest_shims.macho $(BUILD)/guest_missing.macho $(BUILD)/guest_printf.macho $(BUILD)/guest_stdio.macho

# qemu-aarch64 is only needed when the host is not arm64.
RUNNER  := $(shell [ "$$(uname -m)" = aarch64 ] || echo qemu-aarch64)

.PHONY: all test run run-test objc-probe dump imports clean
all: $(BUILD)/machodump $(BUILD)/idroid-run $(GUESTS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/machodump: tools/machodump.c loader/macho.c loader/macho.h | $(BUILD)
	$(CC) $(CFLAGS) tools/machodump.c loader/macho.c -o $@

# Real arm64 macOS executables, cross-compiled without a Mac (zig bundles the libSystem stubs).
$(BUILD)/hello.macho: tests/hello.c | $(BUILD)
	$(ZIGCC) -target aarch64-macos $< -o $@

# -fno-sanitize=undefined: zig's debug builds link a UBSan runtime that drags in ~90 unrelated imports
$(BUILD)/guest_%.macho: tests/guest_%.c | $(BUILD)
	$(ZIGCC) -target aarch64-macos -fno-sanitize=undefined $< -o $@

$(BUILD)/idroid-run: tools/idroid-run.c $(LOADER) $(SHIMS) $(HEADERS) | $(BUILD)
	$(RUN_CC) $(CFLAGS) -O2 tools/idroid-run.c $(LOADER) $(SHIMS) -o $@

$(BUILD)/test_macho: tests/test_macho.c loader/macho.c loader/macho.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_macho.c loader/macho.c -o $@

$(BUILD)/test_load: tests/test_load.c $(LOADER) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_load.c $(LOADER) -o $@

# Unit tests: run on any Linux host, no emulator needed.
test: $(BUILD)/test_macho $(BUILD)/test_load $(BUILD)/hello.macho
	$(BUILD)/test_macho $(BUILD)/hello.macho
	$(BUILD)/test_load $(BUILD)/hello.macho

# End to end: actually execute the Mach-O's arm64 code.
run: $(BUILD)/idroid-run $(BUILD)/hello.macho
	$(RUNNER) $(BUILD)/idroid-run $(BUILD)/hello.macho

run-test: $(BUILD)/idroid-run $(GUESTS)
	sh tests/run_guests.sh "$(RUNNER)" $(BUILD)

# Objective-C guest built with no Apple SDK (zig has no libobjc stub, hence dynamic_lookup).
$(BUILD)/objc_probe.macho: tests/objc/probe.m | $(BUILD)
	$(ZIGCC) -x objective-c -fno-sanitize=undefined -target aarch64-macos $< -Wl,-undefined,dynamic_lookup -o $@

# Not in run-test until the runtime exists: shows what the guest still needs, as a TRACE list.
objc-probe: $(BUILD)/idroid-run $(BUILD)/objc_probe.macho
	-$(RUNNER) $(BUILD)/idroid-run $(BUILD)/objc_probe.macho

dump: $(BUILD)/machodump $(BUILD)/hello.macho
	$(BUILD)/machodump $(BUILD)/hello.macho

imports: $(BUILD)/machodump $(BUILD)/hello.macho
	$(BUILD)/machodump --imports $(BUILD)/hello.macho

clean:
	rm -rf $(BUILD)
