/* An NSObject subclass exercising what real Objective-C code leans on, with no Foundation headers
 * (we have no Apple SDK). NSObject is declared here and imported from the runtime at load time,
 * exactly as a real app imports it from Foundation. Manual reference counting (no -fobjc-arc). */
#include <stdio.h>

typedef signed char BOOL;

__attribute__((objc_root_class))
@interface NSObject { Class isa; }
+ (id)alloc;
+ (id)new;
- (id)init;
- (void)dealloc;
- (id)retain;
- (void)release;
- (unsigned long)retainCount;
- (BOOL)isKindOfClass:(Class)c;
- (BOOL)respondsToSelector:(SEL)s;
+ (Class)class;
@end

static int g_live;

@interface Counter : NSObject { @public int count; int step; }
- (id)initWithStep:(int)s;
- (void)bump;
- (int)count;
- (double)scale:(double)f;     /* double in, double out: lives in vector registers, not x0..x7 */
+ (int)live;
@end

@implementation Counter
- (id)initWithStep:(int)s { self = [super init]; if (self) { step = s; g_live++; } return self; }
- (void)bump { count += step; }
- (int)count { return count; }
- (double)scale:(double)f { return f * step; }
- (void)dealloc { g_live--; [super dealloc]; }
+ (int)live { return g_live; }
@end

@interface Doubler : Counter
- (void)bump;                  /* overrides, and calls its superclass twice */
@end

@implementation Doubler
- (void)bump { [super bump]; [super bump]; }
@end

int main(void)
{
    Counter *c = [[Counter alloc] initWithStep:5];
    [c bump]; [c bump]; [c bump];
    printf("count=%d direct=%d live=%d\n", [c count], c->count, [Counter live]);
    printf("scaled=%.2f\n", [c scale:1.5]);

    Doubler *d = [[Doubler alloc] initWithStep:3];
    [d bump];
    printf("doubler=%d live=%d\n", [d count], [Counter live]);

    BOOL k1 = [d isKindOfClass:[Counter class]];
    BOOL k2 = [c isKindOfClass:[Doubler class]];
    BOOL r1 = [c respondsToSelector:@selector(bump)];
    BOOL r2 = [c respondsToSelector:@selector(nope)];
    printf("kind=%d,%d responds=%d,%d\n", k1, k2, r1, r2);

    unsigned long a = [c retainCount];
    [c retain];
    unsigned long b = [c retainCount];
    [c release];
    printf("retain=%lu,%lu\n", a, b);

    Counter *nothing = 0;
    printf("nil=%d nilscale=%.2f\n", [nothing count], [nothing scale:2.0]);

    [d release];
    [c release];
    printf("live after release=%d\n", [Counter live]);
    return 0;
}
