#include "arm64_context.h"

.text
.p2align 2
.global cf_rotate_bridge
.hidden cf_rotate_bridge
.type cf_rotate_bridge, %function
.hidden cf_adaptive_apply
.hidden cf_original_rotate
cf_rotate_bridge:
    .cfi_startproc
    sub sp, sp, #CF_CONTEXT_SIZE
    .cfi_def_cfa_offset CF_CONTEXT_SIZE
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    .cfi_offset 30, -560
    add x16, sp, #CF_CONTEXT_SIZE
    str x16, [sp, #CF_CONTEXT_SP]
    stp q0, q1, [sp, #256]
    stp q2, q3, [sp, #288]
    stp q4, q5, [sp, #320]
    stp q6, q7, [sp, #352]
    stp q8, q9, [sp, #384]
    stp q10, q11, [sp, #416]
    stp q12, q13, [sp, #448]
    stp q14, q15, [sp, #480]
    stp q16, q17, [sp, #512]
    stp q18, q19, [sp, #544]
    stp q20, q21, [sp, #576]
    stp q22, q23, [sp, #608]
    stp q24, q25, [sp, #640]
    stp q26, q27, [sp, #672]
    stp q28, q29, [sp, #704]
    stp q30, q31, [sp, #736]
    mrs x16, nzcv
    str x16, [sp, #CF_CONTEXT_NZCV]
    mrs x16, fpcr
    str x16, [sp, #CF_CONTEXT_FPCR]
    mrs x16, fpsr
    str x16, [sp, #CF_CONTEXT_FPSR]
    mov x0, sp
    bl cf_adaptive_apply
    ldr x16, [sp, #CF_CONTEXT_NZCV]
    msr nzcv, x16
    ldr x16, [sp, #CF_CONTEXT_FPCR]
    msr fpcr, x16
    ldr x16, [sp, #CF_CONTEXT_FPSR]
    msr fpsr, x16
    ldp q0, q1, [sp, #256]
    ldp q2, q3, [sp, #288]
    ldp q4, q5, [sp, #320]
    ldp q6, q7, [sp, #352]
    ldp q8, q9, [sp, #384]
    ldp q10, q11, [sp, #416]
    ldp q12, q13, [sp, #448]
    ldp q14, q15, [sp, #480]
    ldp q16, q17, [sp, #512]
    ldp q18, q19, [sp, #544]
    ldp q20, q21, [sp, #576]
    ldp q22, q23, [sp, #608]
    ldp q24, q25, [sp, #640]
    ldp q26, q27, [sp, #672]
    ldp q28, q29, [sp, #704]
    ldp q30, q31, [sp, #736]
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    .cfi_restore 30
    add sp, sp, #CF_CONTEXT_SIZE
    .cfi_def_cfa_offset 0
    // Tail-call the original BL destination with its original SP and LR.
    // All registers are restored except ABI scratch X17 used by the veneer.
    adrp x17, cf_original_rotate
    add x17, x17, :lo12:cf_original_rotate
    ldar x17, [x17]
    br x17
    .cfi_endproc
.size cf_rotate_bridge, .-cf_rotate_bridge
.section .note.GNU-stack,"",%progbits
