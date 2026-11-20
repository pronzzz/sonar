/*
 * EL1 exception vector table. Every exception saves the full integer and
 * FP/SIMD register state into a struct trap_frame (include/arch.h) on the
 * current stack and calls trap_dispatch(frame, kind) in C. Saving FP state
 * on every entry lets interrupt handlers use compiler-generated SIMD code.
 *
 * Each vector slot is only 128 bytes, so slots just record the kind and
 * branch to a shared stub.
 */

#define TF_Q        288
#define TF_FPCR     800
#define TF_SIZE     816

.macro vector_entry kind
    .balign 128
    sub     sp, sp, #TF_SIZE
    stp     x0, x1, [sp, #16 * 0]
    mov     x1, #\kind
    b       exception_common
.endm

    .section .text.vectors, "ax"
    .balign 2048
    .global exception_vectors
exception_vectors:
    vector_entry 0      /* current EL, SP_EL0: sync */
    vector_entry 1      /*                     irq  */
    vector_entry 2      /*                     fiq  */
    vector_entry 3      /*                     serror */
    vector_entry 4      /* current EL, SP_ELx: sync */
    vector_entry 5      /*                     irq  */
    vector_entry 6      /*                     fiq  */
    vector_entry 7      /*                     serror */
    vector_entry 8      /* lower EL, AArch64:  sync */
    vector_entry 9      /*                     irq  */
    vector_entry 10     /*                     fiq  */
    vector_entry 11     /*                     serror */
    vector_entry 12     /* lower EL, AArch32 (unsupported) */
    vector_entry 13
    vector_entry 14
    vector_entry 15

exception_common:
    stp     x2, x3, [sp, #16 * 1]
    stp     x4, x5, [sp, #16 * 2]
    stp     x6, x7, [sp, #16 * 3]
    stp     x8, x9, [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    mov     x19, x1                         // kind (x19 is saved above)
    mrs     x0, elr_el1
    stp     x30, x0, [sp, #240]             // x30, elr
    mrs     x0, spsr_el1
    mrs     x1, esr_el1
    stp     x0, x1, [sp, #256]              // spsr, esr
    mrs     x0, far_el1
    mrs     x1, sp_el0
    stp     x0, x1, [sp, #272]              // far, sp_el0
    add     x0, sp, #TF_Q
    stp     q0, q1, [x0, #32 * 0]
    stp     q2, q3, [x0, #32 * 1]
    stp     q4, q5, [x0, #32 * 2]
    stp     q6, q7, [x0, #32 * 3]
    stp     q8, q9, [x0, #32 * 4]
    stp     q10, q11, [x0, #32 * 5]
    stp     q12, q13, [x0, #32 * 6]
    stp     q14, q15, [x0, #32 * 7]
    stp     q16, q17, [x0, #32 * 8]
    stp     q18, q19, [x0, #32 * 9]
    stp     q20, q21, [x0, #32 * 10]
    stp     q22, q23, [x0, #32 * 11]
    stp     q24, q25, [x0, #32 * 12]
    stp     q26, q27, [x0, #32 * 13]
    stp     q28, q29, [x0, #32 * 14]
    stp     q30, q31, [x0, #32 * 15]
    mrs     x1, fpcr
    mrs     x2, fpsr
    add     x0, sp, #TF_FPCR
    stp     x1, x2, [x0]

    mov     x0, sp
    mov     x1, x19
    bl      trap_dispatch

    add     x0, sp, #TF_FPCR
    ldp     x1, x2, [x0]
    msr     fpcr, x1
    msr     fpsr, x2
    add     x0, sp, #TF_Q
    ldp     q0, q1, [x0, #32 * 0]
    ldp     q2, q3, [x0, #32 * 1]
    ldp     q4, q5, [x0, #32 * 2]
    ldp     q6, q7, [x0, #32 * 3]
    ldp     q8, q9, [x0, #32 * 4]
    ldp     q10, q11, [x0, #32 * 5]
    ldp     q12, q13, [x0, #32 * 6]
    ldp     q14, q15, [x0, #32 * 7]
    ldp     q16, q17, [x0, #32 * 8]
    ldp     q18, q19, [x0, #32 * 9]
    ldp     q20, q21, [x0, #32 * 10]
    ldp     q22, q23, [x0, #32 * 11]
    ldp     q24, q25, [x0, #32 * 12]
    ldp     q26, q27, [x0, #32 * 13]
    ldp     q28, q29, [x0, #32 * 14]
    ldp     q30, q31, [x0, #32 * 15]
    ldp     x0, x1, [sp, #272]              // far (ignored), sp_el0
    msr     sp_el0, x1
    ldp     x0, x1, [sp, #256]              // spsr, esr (ignored)
    msr     spsr_el1, x0
    ldp     x30, x0, [sp, #240]             // x30, elr
    msr     elr_el1, x0
    ldp     x0, x1, [sp, #16 * 0]
    ldp     x2, x3, [sp, #16 * 1]
    ldp     x4, x5, [sp, #16 * 2]
    ldp     x6, x7, [sp, #16 * 3]
    ldp     x8, x9, [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    add     sp, sp, #TF_SIZE
    eret
