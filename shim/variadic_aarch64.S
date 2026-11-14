/*
 * Variadic trampolines for the Apple arm64 calling convention.
 *
 * Apple (DarwinPCS) passes the fixed arguments of a variadic call in x0..x7 as usual, but every
 * variadic argument goes on the stack, one 8-byte slot each (doubles included: a double is stored
 * as raw bits at [sp+N], never in a v register). Linux/Android (AAPCS64) passes the first variadic
 * arguments in x0..x7 and v0..v7. So a guest's printf cannot simply be the host's printf.
 *
 * Each trampoline is entered with sp pointing at the guest's first variadic argument. It builds an
 * AAPCS64 va_list that says "no register arguments are left, the rest are on the stack, starting at
 * the entry sp", then tail-calls the matching v-function (vprintf, vsnprintf, ...), which reads the
 * arguments with ordinary va_arg.
 *
 * AAPCS64 va_list layout (32 bytes):
 *    +0  void *__stack      next stack argument
 *    +8  void *__gr_top     end of the saved general registers   (unused when __gr_offs == 0)
 *   +16  void *__vr_top     end of the saved vector registers    (unused when __vr_offs == 0)
 *   +24  int   __gr_offs    0 = no general-register arguments remain
 *   +28  int   __vr_offs    0 = no vector-register arguments remain
 * A va_list is a 32-byte struct, so as a function argument it is passed as a pointer to a copy.
 *
 * VARIADIC name, target, nfixed
 *   nfixed = number of named (fixed) arguments, i.e. the register that receives the va_list pointer.
 */
        .text

        .macro VARIADIC name, target, nfixed
        .globl  \name
        .type   \name, %function
        .balign 4
\name:
        stp     x29, x30, [sp, #-48]!       // frame record + 32 bytes of va_list; sp stays 16-aligned
        mov     x29, sp
        add     x9, sp, #48                 // entry sp: the guest's first variadic argument
        str     x9, [sp, #16]               // va_list.__stack
        stp     xzr, xzr, [sp, #24]         // va_list.__gr_top, __vr_top (unused)
        str     xzr, [sp, #40]              // va_list.__gr_offs and __vr_offs = 0
        add     x\nfixed, sp, #16           // pass the va_list by reference after the fixed arguments
        bl      \target
        ldp     x29, x30, [sp], #48
        ret
        .size   \name, . - \name
        .endm

        VARIADIC shim_printf,          vprintf,            1   // printf(fmt, ...)
        VARIADIC shim_sprintf,         vsprintf,           2   // sprintf(buf, fmt, ...)
        VARIADIC shim_snprintf,        vsnprintf,          3   // snprintf(buf, n, fmt, ...)
        VARIADIC shim_dprintf,         vdprintf,           2   // dprintf(fd, fmt, ...)
        VARIADIC shim_fprintf,         vfprintf,           2   // fprintf(file, fmt, ...)
        VARIADIC shim___snprintf_chk,  shim_vsnprintf_chk, 5   // __snprintf_chk(buf, n, flag, slen, fmt, ...)
        VARIADIC shim___sprintf_chk,   shim_vsprintf_chk,  4   // __sprintf_chk(buf, flag, slen, fmt, ...)

        .section .note.GNU-stack,"",%progbits
