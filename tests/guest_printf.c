/* Guest that uses variadic libSystem functions. Under Apple's arm64 ABI every argument after the
 * format string is passed on the stack, so this fails (garbage or a crash) unless the shim rebuilds them. */
#include <stdio.h>

int main(void)
{
    char buf[64];

    /* ints, a string, a double, a 64-bit value, a char and hex: mixed widths and a float */
    printf("int=%d str=%s dbl=%.3f ll=%lld chr=%c hex=%x\n", -42, "abc", 3.14159, 1LL << 40, 'x', 255);

    snprintf(buf, sizeof buf, "%s-%d-%5.1f", "snp", 7, 2.5);
    puts(buf);

    /* ten ints: more arguments than the eight argument registers a naive passthrough would read */
    printf("many: %d %d %d %d %d %d %d %d %d %d\n", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);

    /* several doubles in a row: Apple keeps these on the stack, Linux would read v0..v7 */
    printf("dbls: %.1f %.1f %.1f %.1f\n", 1.5, 2.5, 3.5, 4.5);
    return 0;
}
