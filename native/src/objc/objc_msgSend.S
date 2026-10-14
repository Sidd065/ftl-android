// arm64 objc_msgSend / objc_msgSendSuper2 with a lock-free method cache.
// struct objc_cache { uint32 mask; uint32 occupied; struct { SEL sel; IMP imp; } buckets[]; }
// class layout: isa@0 superclass@8 cache@16 rw@24 bits@32

    .text
    .p2align 4
    .globl objc_msgSend
    .type objc_msgSend, %function
objc_msgSend:
    cbz     x0, .Lnil
    ldr     x16, [x0]
.Llookup:
    ldr     x10, [x16, #16]
    ldr     w11, [x10]
    eor     x12, x1, x1, lsr #4
    and     x12, x12, x11
    add     x13, x10, #8
1:
    add     x14, x13, x12, lsl #4
    ldp     x9, x17, [x14]
    cmp     x9, x1
    b.ne    2f
    cbz     x17, .Lslow
    br      x17
2:
    cbz     x9, .Lslow
    add     x12, x12, #1
    and     x12, x12, x11
    b       1b
.Lslow:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    sub     sp, sp, #208
    stp     q0, q1, [sp, #0]
    stp     q2, q3, [sp, #32]
    stp     q4, q5, [sp, #64]
    stp     q6, q7, [sp, #96]
    stp     x0, x1, [sp, #128]
    stp     x2, x3, [sp, #144]
    stp     x4, x5, [sp, #160]
    stp     x6, x7, [sp, #176]
    str     x8, [sp, #192]
    mov     x2, x16
    bl      objc_msg_lookup_slow
    mov     x17, x0
    ldp     q0, q1, [sp, #0]
    ldp     q2, q3, [sp, #32]
    ldp     q4, q5, [sp, #64]
    ldp     q6, q7, [sp, #96]
    ldp     x0, x1, [sp, #128]
    ldp     x2, x3, [sp, #144]
    ldp     x4, x5, [sp, #160]
    ldp     x6, x7, [sp, #176]
    ldr     x8, [sp, #192]
    mov     sp, x29
    ldp     x29, x30, [sp], #16
    br      x17
.Lnil:
    mov     x1, #0
    movi    d0, #0
    movi    d1, #0
    movi    d2, #0
    movi    d3, #0
    ret
    .size objc_msgSend, .-objc_msgSend

    .p2align 4
    .globl objc_msgSendSuper2
    .type objc_msgSendSuper2, %function
objc_msgSendSuper2:
    ldr     x16, [x0, #8]
    ldr     x16, [x16, #8]
    ldr     x0, [x0]
    b       .Llookup
    .size objc_msgSendSuper2, .-objc_msgSendSuper2

    .p2align 4
    .globl objc_msgSendSuper
    .type objc_msgSendSuper, %function
objc_msgSendSuper:
    ldr     x16, [x0, #8]
    ldr     x0, [x0]
    b       .Llookup
    .size objc_msgSendSuper, .-objc_msgSendSuper

// Returned for unrecognized selectors: behaves like messaging nil.
    .p2align 4
    .globl objc_nil_imp
    .type objc_nil_imp, %function
objc_nil_imp:
    mov     x0, #0
    mov     x1, #0
    movi    d0, #0
    movi    d1, #0
    movi    d2, #0
    movi    d3, #0
    ret
    .size objc_nil_imp, .-objc_nil_imp

    .section .note.GNU-stack,"",%progbits
