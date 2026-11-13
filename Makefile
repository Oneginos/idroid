CC      ?= cc
ZIGCC   ?= python3 -m ziglang cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -O1 -Iloader
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=undefined
BUILD   := build

.PHONY: all test dump imports clean
all: $(BUILD)/machodump $(BUILD)/hello.macho

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/machodump: tools/machodump.c loader/macho.c loader/macho.h | $(BUILD)
	$(CC) $(CFLAGS) tools/machodump.c loader/macho.c -o $@

# A real arm64 macOS executable, cross-compiled without a Mac (zig bundles the libSystem stubs).
$(BUILD)/hello.macho: tests/hello.c | $(BUILD)
	$(ZIGCC) -target aarch64-macos $< -o $@

$(BUILD)/test_macho: tests/test_macho.c loader/macho.c loader/macho.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_macho.c loader/macho.c -o $@

test: $(BUILD)/test_macho $(BUILD)/hello.macho
	$(BUILD)/test_macho $(BUILD)/hello.macho

dump: $(BUILD)/machodump $(BUILD)/hello.macho
	$(BUILD)/machodump $(BUILD)/hello.macho

imports: $(BUILD)/machodump $(BUILD)/hello.macho
	$(BUILD)/machodump --imports $(BUILD)/hello.macho

clean:
	rm -rf $(BUILD)
