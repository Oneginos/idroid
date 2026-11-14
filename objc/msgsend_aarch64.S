/*
 * objc_msgSend and objc_msgSendSuper2.
 *
 * A message send must call the method with the caller's arguments untouched, so the trampoline saves
 * every argument register (x0-x7, the indirect-result register x8, and the vector registers q0-q7 that
 * carry float/double arguments), asks C to find the method, restores everything and tail-jumps to it.
 * The method returns straight to the original caller.
 *
 *   idroid_objc_lookup(self, sel)               -> IMP
 *   idroid_objc_lookup_super(objc_super*, sel)  -> IMP
 *
 * Frame (0xe0 bytes, 16-aligned):
 *   +0x00 x29, x30    +0x10 x0,x1   +0x20 x2,x3   +0x30 x4,x5   +0x40 x6,x7
 *   +0x50 x8 (+pad)   +0x60 q0,q1   +0x80 q2,q3   +0xa0 q4,q5   +0xc0 q6,q7
 */
        .text

        .macro SAVE_ARGS
        stp     x29, x30, [sp, #-0xe0]!
        mov     x29, sp
        stp     x0, x1, [sp, #0x10]
        stp     x2, x3, [sp, #0x20]
        stp     x4, x5, [sp, #0x30]
        stp     x6, x7, [sp, #0x40]
        str     x8,     [sp, #0x50]
        stp     q0, q1, [sp, #0x60]
        stp     q2, q3, [sp, #0x80]
        stp     q4, q5, [sp, #0xa0]
        stp     q6, q7, [sp, #0xc0]
        .endm

        .macro RESTORE_ARGS
        ldp     q6, q7, [sp, #0xc0]
        ldp     q4, q5, [sp, #0xa0]
        ldp     q2, q3, [sp, #0x80]
        ldp     q0, q1, [sp, #0x60]
        ldr     x8,     [sp, #0x50]
        ldp     x6, x7, [sp, #0x40]
        ldp     x4, x5, [sp, #0x30]
        ldp     x2, x3, [sp, #0x20]
        ldp     x0, x1, [sp, #0x10]
        .endm

        .globl  objc_msgSend
        .type   objc_msgSend, %function
        .balign 4
objc_msgSend:
        cbz     x0, 1f                      // a message to nil does nothing and returns zero
        SAVE_ARGS
        bl      idroid_objc_lookup          // x0 = self, x1 = sel  ->  x0 = IMP
        mov     x16, x0
        RESTORE_ARGS
        ldp     x29, x30, [sp], #0xe0
        br      x16
1:      mov     x1, xzr                     // x0 is already zero; zero the second integer and float results
        movi    v0.2d, #0
        movi    v1.2d, #0
        movi    v2.2d, #0
        movi    v3.2d, #0
        ret
        .size   objc_msgSend, . - objc_msgSend

/* x0 = struct objc_super { id receiver; Class current_class; }. Lookup starts at current_class's
 * superclass; the method is called with receiver in x0. */
        .globl  objc_msgSendSuper2
        .type   objc_msgSendSuper2, %function
        .balign 4
objc_msgSendSuper2:
        SAVE_ARGS
        bl      idroid_objc_lookup_super    // x0 = objc_super*, x1 = sel  ->  x0 = IMP
        mov     x16, x0
        RESTORE_ARGS
        ldr     x0, [x0]                    // receiver replaces the objc_super pointer
        ldp     x29, x30, [sp], #0xe0
        br      x16
        .size   objc_msgSendSuper2, . - objc_msgSendSuper2

        .section .note.GNU-stack,"",%progbits
