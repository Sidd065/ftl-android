// Assembly helpers for Darwin ABI differences.
//
// Apple arm64 passes *all* variadic arguments on the stack (8-byte slots),
// while AAPCS64 (Linux/Android) uses registers first. These trampolines hand
// the address of the variadic stack area to C implementations which build an
// AAPCS64 va_list pointing at it.

    .text

// int open(const char *path, int flags, ...)  -> darwin_open_impl(path, flags, mode)
    .globl darwin_open
    .p2align 2
darwin_open:
    ldr     w2, [sp]
    b       darwin_open_impl

// int fcntl(int fd, int cmd, ...)  -> darwin_fcntl_impl(fd, cmd, arg)
    .globl darwin_fcntl
    .p2align 2
darwin_fcntl:
    ldr     x2, [sp]
    b       darwin_fcntl_impl

// int __sprintf_chk(char *s, int flag, size_t slen, const char *fmt, ...)
    .globl darwin_sprintf_chk
    .p2align 2
darwin_sprintf_chk:
    mov     x4, sp
    b       darwin_sprintf_chk_impl

// int asprintf(char **ret, const char *fmt, ...)
    .globl darwin_asprintf
    .p2align 2
darwin_asprintf:
    mov     x2, sp
    b       darwin_asprintf_impl

// int sprintf(char *s, const char *fmt, ...)
    .globl darwin_sprintf
    .p2align 2
darwin_sprintf:
    mov     x2, sp
    b       darwin_sprintf_impl

// int snprintf(char *s, size_t n, const char *fmt, ...)
    .globl darwin_snprintf
    .p2align 2
darwin_snprintf:
    mov     x3, sp
    b       darwin_snprintf_impl

// int printf(const char *fmt, ...)
    .globl darwin_printf
    .p2align 2
darwin_printf:
    mov     x1, sp
    b       darwin_printf_impl

// int fprintf(FILE *f, const char *fmt, ...)
    .globl darwin_fprintf
    .p2align 2
darwin_fprintf:
    mov     x2, sp
    b       darwin_fprintf_impl

// int sscanf(const char *s, const char *fmt, ...)
    .globl darwin_sscanf
    .p2align 2
darwin_sscanf:
    mov     x2, sp
    b       darwin_sscanf_impl

// void NSLog(NSString *fmt, ...)
    .globl darwin_NSLog
    .p2align 2
darwin_NSLog:
    mov     x1, sp
    b       darwin_NSLog_impl

// setjmp/longjmp fitting Darwin's 192-byte jmp_buf (int[48]).
// layout: x19..x28 (0..72), x29 (80), x30 (88), sp (96), d8..d15 (104..160)
    .globl darwin_setjmp
    .p2align 2
darwin_setjmp:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x1, sp
    str     x1, [x0, #96]
    stp     d8, d9, [x0, #104]
    stp     d10, d11, [x0, #120]
    stp     d12, d13, [x0, #136]
    stp     d14, d15, [x0, #152]
    mov     w0, #0
    ret

    .globl darwin_longjmp
    .p2align 2
darwin_longjmp:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x2, [x0, #96]
    mov     sp, x2
    ldp     d8, d9, [x0, #104]
    ldp     d10, d11, [x0, #120]
    ldp     d12, d13, [x0, #136]
    ldp     d14, d15, [x0, #152]
    cmp     w1, #0
    csinc   w0, w1, wzr, ne
    ret

// struct { float s, c; } __sincosf_stret(float)
    .globl darwin_sincosf_stret
    .p2align 2
darwin_sincosf_stret:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    add     x0, sp, #16
    add     x1, sp, #20
    bl      sincosf
    ldr     s0, [sp, #16]
    ldr     s1, [sp, #20]
    ldp     x29, x30, [sp], #32
    ret

// struct { double s, c; } __sincos_stret(double)
    .globl darwin_sincos_stret
    .p2align 2
darwin_sincos_stret:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    add     x0, sp, #16
    add     x1, sp, #24
    bl      sincos
    ldr     d0, [sp, #16]
    ldr     d1, [sp, #24]
    ldp     x29, x30, [sp], #32
    ret

    .section .note.GNU-stack,"",%progbits
