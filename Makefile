CC      ?= cc
ZIGCC   ?= python3 -m ziglang cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -O1 -Iloader -Ishim -Iobjc
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=undefined
BUILD   := build

# idroid-run executes arm64 guest code, so it is an aarch64 Linux program.
# Cross-built static with zig by default; on an arm64 host (Termux) use: make RUN_CC=cc
RUN_CC  ?= $(ZIGCC) -target aarch64-linux-musl

LOADER  := loader/macho.c loader/load.c
SHIMS   := shim/shims.c shim/variadic_aarch64.S
OBJC    := objc/abi.c objc/runtime.c objc/msgsend_aarch64.S
HEADERS := loader/macho.h loader/load.h shim/shims.h objc/abi.h objc/runtime.h
GUESTS  := $(BUILD)/hello.macho $(BUILD)/guest_shims.macho $(BUILD)/guest_missing.macho $(BUILD)/guest_printf.macho $(BUILD)/guest_stdio.macho \
           $(BUILD)/objc_probe.macho $(BUILD)/objc_counter.macho $(BUILD)/objc_missing.macho $(BUILD)/objc_arc.macho

# qemu-aarch64 is only needed when the host is not arm64.
RUNNER  := $(shell [ "$$(uname -m)" = aarch64 ] || echo qemu-aarch64)

.PHONY: all test run run-test dump imports clean
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

$(BUILD)/idroid-run: tools/idroid-run.c $(LOADER) $(SHIMS) $(OBJC) $(HEADERS) | $(BUILD)
	$(RUN_CC) $(CFLAGS) -O2 tools/idroid-run.c $(LOADER) $(SHIMS) $(OBJC) -o $@

$(BUILD)/test_macho: tests/test_macho.c loader/macho.c loader/macho.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_macho.c loader/macho.c -o $@

$(BUILD)/test_objc: tests/test_objc.c objc/abi.c objc/abi.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_objc.c objc/abi.c -o $@

$(BUILD)/test_load: tests/test_load.c $(LOADER) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_load.c $(LOADER) -o $@

# Unit tests: run on any Linux host, no emulator needed.
test: $(BUILD)/test_macho $(BUILD)/test_load $(BUILD)/test_objc $(BUILD)/hello.macho
	$(BUILD)/test_macho $(BUILD)/hello.macho
	$(BUILD)/test_load $(BUILD)/hello.macho
	$(BUILD)/test_objc

# End to end: actually execute the Mach-O's arm64 code.
run: $(BUILD)/idroid-run $(BUILD)/hello.macho
	$(RUNNER) $(BUILD)/idroid-run $(BUILD)/hello.macho

run-test: $(BUILD)/idroid-run $(GUESTS)
	sh tests/run_guests.sh "$(RUNNER)" $(BUILD)

# Objective-C guests, built with no Apple SDK (zig has no libobjc stub, hence dynamic_lookup).
OBJCFLAGS :=
$(BUILD)/objc_arc.macho: OBJCFLAGS = -fobjc-arc
$(BUILD)/objc_%.macho: tests/objc/%.m | $(BUILD)
	$(ZIGCC) -x objective-c -fno-sanitize=undefined -Wno-objc-method-access $(OBJCFLAGS) -target aarch64-macos $< -Wl,-undefined,dynamic_lookup -o $@

dump: $(BUILD)/machodump $(BUILD)/hello.macho
	$(BUILD)/machodump $(BUILD)/hello.macho

imports: $(BUILD)/machodump $(BUILD)/hello.macho
	$(BUILD)/machodump --imports $(BUILD)/hello.macho

clean:
	rm -rf $(BUILD)
