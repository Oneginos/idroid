/* Guest that calls a function we deliberately do not provide: it must fail loudly, never silently. */
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    puts("before");
    const char *h = getenv("HOME");
    puts(h ? "has home" : "no home");
    return 0;
}
