/* No Foundation, no NSObject: a self-contained root class with class methods only. */
#include <stdio.h>

__attribute__((objc_root_class))
@interface Greeter
+ (int)answer;
+ (int)add:(int)a to:(int)b;
@end

@implementation Greeter
+ (int)answer { return 42; }
+ (int)add:(int)a to:(int)b { return a + b; }
@end

int main(void)
{
    printf("answer=%d sum=%d\n", [Greeter answer], [Greeter add:2 to:40]);
    return 0;
}
