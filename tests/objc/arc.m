/* Automatic reference counting: the default for all modern iOS code. Compiled with -fobjc-arc, so the
 * compiler inserts the retain/release calls itself and generates a ".cxx_destruct" method that releases
 * strong instance variables. Dropping the head of a linked list must free every node behind it. */
#include <stdio.h>

#define nil ((id)0)    /* normally from <objc/objc.h>, which we do not have */

__attribute__((objc_root_class))
@interface NSObject { Class isa; }
+ (id)alloc;
- (id)init;
- (void)dealloc;
@end

static int g_dead;

@interface Node : NSObject { @public Node *next; int tag; }
- (id)initWithTag:(int)t;
@end

@implementation Node
- (id)initWithTag:(int)t { self = [super init]; if (self) tag = t; return self; }
- (void)dealloc { g_dead++; }              /* ARC adds [super dealloc] and releases `next` for us */
@end

int main(void)
{
    @autoreleasepool {
        Node *chain = nil;
        for (int i = 0; i < 5; i++) {
            Node *n = [[Node alloc] initWithTag:i];
            n->next = chain;               /* strong store: the node now keeps the old head alive */
            chain = n;
        }
        printf("built head=%d second=%d dead=%d\n", chain->tag, chain->next->tag, g_dead);
        chain = nil;                       /* release the head: the whole chain should unwind */
        printf("dead after drop=%d\n", g_dead);
    }
    printf("dead at end=%d\n", g_dead);
    return 0;
}
