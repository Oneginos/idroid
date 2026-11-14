/* Guest that exercises several libSystem shims and returns a nonzero exit code. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    const char *msg = "shims ok\n";
    char *buf = malloc(64);
    if (!buf) return 1;
    memcpy(buf, msg, strlen(msg) + 1);
    write(1, buf, strlen(buf));
    free(buf);
    return 7;
}
