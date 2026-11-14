#!/bin/sh
# End-to-end tests: run real arm64 Mach-O guests through idroid-run.
# usage: run_guests.sh RUNNER BUILD_DIR   (RUNNER is empty on arm64 hosts, qemu-aarch64 elsewhere)
RUNNER="$1"
B="${2:-build}"
RUN="$RUNNER $B/idroid-run"
fail=0

# Guests abort on purpose here. A core file would hold a copy of the environment (CI and Codespaces
# keep tokens there), so never allow one, and never let it land in the working tree.
ulimit -c 0

ok()  { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

# 1. hello: output and exit code 0
out=$(IDROID_VERBOSE=1 $RUN "$B/hello.macho" 2>"$B/hello.err"); rc=$?
[ "$out" = "hello from a Mach-O" ] && [ $rc -eq 0 ] && ok "hello prints and exits 0" || { bad "hello (rc=$rc, out='$out')"; cat "$B/hello.err"; }
grep -q "1 lazy binds" "$B/hello.err" && ok "lazy bind applied eagerly" || bad "lazy bind stats missing"

# 2. several shims, exit status passes through main's return value
out=$($RUN "$B/guest_shims.macho" 2>/dev/null); rc=$?
[ "$out" = "shims ok" ] && ok "write/malloc/memcpy/strlen shims" || bad "guest_shims output '$out'"
[ $rc -eq 7 ] && ok "main's return value becomes the exit code" || bad "guest_shims exit code $rc (wanted 7)"

# 3. an unprovided function fails loudly, names the symbol, and output before it is not lost
out=$($RUN "$B/guest_missing.macho" 2>"$B/missing.err"); rc=$?
[ "$out" = "before" ] && ok "output before the missing call is flushed" || bad "guest_missing stdout '$out'"
[ $rc -ne 0 ] && ok "missing symbol aborts (rc=$rc)" || bad "guest_missing exited 0"
grep -q "MISSING symbol: _getenv" "$B/missing.err" && ok "TRACE-format line names _getenv" || { bad "no MISSING line for _getenv"; cat "$B/missing.err"; }

# 4. variadics: arguments after the format string arrive on the stack under Apple's ABI
out=$($RUN "$B/guest_printf.macho" 2>"$B/printf.err"); rc=$?
want="int=-42 str=abc dbl=3.142 ll=1099511627776 chr=x hex=ff
snp-7-  2.5
many: 1 2 3 4 5 6 7 8 9 10
dbls: 1.5 2.5 3.5 4.5"
[ "$out" = "$want" ] && [ $rc -eq 0 ] && ok "printf/snprintf variadics read Apple's stack layout" \
    || { bad "guest_printf (rc=$rc)"; echo "--- got:"; echo "$out"; echo "--- wanted:"; echo "$want"; cat "$B/printf.err"; }

# 5. va_list pointers, stdout/stderr data symbols, stack protector canary
out=$($RUN "$B/guest_stdio.macho" 2>"$B/stdio.err"); rc=$?
want="va: 5 ok 2.25 9000000000
to stdout 1
fputs line
copied 15 bytes"
[ "$out" = "$want" ] && [ $rc -eq 0 ] && ok "vprintf(va_list), fprintf(stdout), fputs, stack protector" \
    || { bad "guest_stdio stdout (rc=$rc)"; echo "--- got:"; echo "$out"; echo "--- wanted:"; echo "$want"; }
[ "$(cat "$B/stdio.err")" = "to stderr 2" ] && ok "fprintf(stderr) reaches stderr, nothing else does" \
    || { bad "guest_stdio stderr"; cat "$B/stdio.err"; }

# 6. refusals: not a Mach-O, missing file
echo "not a macho" > "$B/junk.bin"
$RUN "$B/junk.bin" >/dev/null 2>&1 && bad "junk file was accepted" || ok "non-Mach-O file is refused"
$RUN "$B/does-not-exist" >/dev/null 2>&1 && bad "missing file was accepted" || ok "missing file is refused"

[ $fail -eq 0 ] && echo "run-test OK" || { echo "run-test FAILED"; exit 1; }
