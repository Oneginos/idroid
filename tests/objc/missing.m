/* A message the object does not implement must fail loudly and name the selector, never do nothing.
 * Run with no argument: an instance message. With any argument: a class message. */
#include <stdio.h>

__attribute__((objc_root_class))
@interface NSObject { Class isa; }
+ (id)new;
@end

@interface Thing : NSObject
@end

@implementation Thing
@end

int main(int argc, char **argv)
{
    (void)argv;
    puts("before");
    id t = [Thing new];
    if (argc > 1) [(id)[Thing class] zap];    /* class message */
    else          [t frobnicate];             /* instance message */
    puts("after");                            /* must never print */
    return 0;
}
