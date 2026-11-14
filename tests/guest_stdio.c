/* Guest that uses va_list-taking functions, stdout/stderr FILE pointers and the stack protector.
 *
 * - say() is a variadic function defined in the guest. Its va_list is Apple's: a plain pointer to the
 *   next stack argument. It is handed to vprintf, which on Linux expects a 32-byte struct instead.
 * - stdout/stderr are the data symbols ___stdoutp / ___stderrp, not functions.
 * - copy() has a local char array, so the compiler guards it with ___stack_chk_guard. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static int copy(const char *src)
{
    char local[32];
    strncpy(local, src, sizeof local - 1);
    local[sizeof local - 1] = 0;
    return (int)strlen(local);
}

int main(void)
{
    say("va: %d %s %.2f %lld\n", 5, "ok", 2.25, 9000000000LL);
    fprintf(stdout, "to stdout %d\n", 1);
    fprintf(stderr, "to stderr %d\n", 2);
    fputs("fputs line\n", stdout);
    say("copied %d bytes\n", copy("stack protector"));
    return 0;
}
