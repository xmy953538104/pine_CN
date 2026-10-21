// SPDX-License-Identifier: MPL-2.0
// Own synthetic blob; no SDK instructions or addresses. No raw signal recovery.
.text
.p2align 4
.global invoke_fixture
.type invoke_fixture,%function
invoke_fixture:
    sub sp, sp, #256
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp q8, q9, [sp, #96]
    stp q10, q11, [sp, #128]
    stp q12, q13, [sp, #160]
    stp q14, q15, [sp, #192]
    str x0, [sp, #224]
    mrs x9, fpcr
    mrs x10, fpsr
    stp x9, x10, [sp, #232]
    mov x30, x1
    ldr x9, [x30, #256]
    msr nzcv, x9
    ldr x9, [x30, #264]
    msr fpcr, x9
    ldr x9, [x30, #272]
    msr fpsr, x9
    ldp q0, q1, [x30, #304]
    ldp q2, q3, [x30, #336]
    ldp q4, q5, [x30, #368]
    ldp q6, q7, [x30, #400]
    ldp q8, q9, [x30, #432]
    ldp q10, q11, [x30, #464]
    ldp q12, q13, [x30, #496]
    ldp q14, q15, [x30, #528]
    ldp q16, q17, [x30, #560]
    ldp q18, q19, [x30, #592]
    ldp q20, q21, [x30, #624]
    ldp q22, q23, [x30, #656]
    ldp q24, q25, [x30, #688]
    ldp q26, q27, [x30, #720]
    ldp q28, q29, [x30, #752]
    ldp q30, q31, [x30, #784]
    ldp x0, x1, [x30, #0]
    ldp x2, x3, [x30, #16]
    ldp x4, x5, [x30, #32]
    ldp x6, x7, [x30, #48]
    ldp x8, x9, [x30, #64]
    ldp x10, x11, [x30, #80]
    ldp x12, x13, [x30, #96]
    ldp x14, x15, [x30, #112]
    ldp x16, x17, [x30, #128]
    ldp x18, x19, [x30, #144]
    ldp x20, x21, [x30, #160]
    ldp x22, x23, [x30, #176]
    ldp x24, x25, [x30, #192]
    ldp x26, x27, [x30, #208]
    ldp x28, x29, [x30, #224]
    ldr x16, [sp, #224]
    blr x16
    ldp x9, x10, [sp, #232]
    msr fpcr, x9
    msr fpsr, x10
    ldp q8, q9, [sp, #96]
    ldp q10, q11, [sp, #128]
    ldp q12, q13, [sp, #160]
    ldp q14, q15, [sp, #192]
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #256
    ret
.size invoke_fixture, .-invoke_fixture

.section .text.fixture,"ax",%progbits
.p2align 4
.global fixture_begin, fixture_end, fixture_caller, fixture_call, fixture_after, fixture_other_caller
.global fixture_original, fixture_resume, fixture_proxy, fixture_tap, fixture_tail, fixture_control_literal
fixture_begin:
fixture_caller:
    stp x29, x30, [sp, #-16]!
fixture_call:
    bl fixture_plt
fixture_after:
    ldp x29, x30, [sp], #16
    ret
fixture_other_caller:
    stp x29, x30, [sp, #-16]!
    bl fixture_plt
    ldp x29, x30, [sp], #16
    ret

fixture_plt:
    // Owned synthetic PLT/GOT; BR preserves incoming LR. Not the real SDK resolver.
    ldr x16, fixture_control_literal
    add x16, x16, #24
    ldr x17, [x16]
    br x17
fixture_proxy:
    stp x15, x16, [sp, #-32]!
    str x17, [sp, #16]
    str xzr, [sp, #24]
    b .Ltap_capture
fixture_tap:
    // Controlled valid-stack fixture only. No guest/SDK installation path.
    stp x15, x16, [sp, #-32]!
    str x17, [sp, #16]
    mov x15, #1
    str x15, [sp, #24]
.Ltap_capture:
    ldr x16, fixture_control_literal
    ldr x16, [x16]
    cbz x16, .Ltap_restore
    ldr x17, [x16, #8]
    eor x17, x17, x30
    cbnz x17, .Ltap_restore
    ldaxr w17, [x16]
    cbnz w17, .Ltap_abandon
    mov w15, #1
    stlxr w17, w15, [x16]
    cbnz w17, .Ltap_restore
    str x0, [x16, #24]
    str x30, [x16, #32]
    add x15, sp, #32
    str x15, [x16, #40]
    adr x15, fixture_original
    str x15, [x16, #48]
    mrs x15, tpidr_el0
    str x15, [x16, #56]
    ldr x15, [sp, #24]
    str x15, [x16, #64]
    mov w15, #2
    stlr w15, [x16]
    b .Ltap_restore
.Ltap_abandon:
    clrex
.Ltap_restore:
    ldr x15, [sp, #24]
    cbnz x15, .Ldetour_restore
    ldr x17, [sp, #16]
    ldp x15, x16, [sp], #32
    b fixture_original
.Ldetour_restore:
    ldr x17, [sp, #16]
    ldp x15, x16, [sp], #32
    sub sp, sp, #32
fixture_tail:
    b fixture_resume

fixture_original:
    sub sp, sp, #32
fixture_resume:
    stp x16, x17, [sp, #-16]!
    ldr x16, fixture_control_literal
    ldr x16, [x16, #8]
    cbz x16, .Loriginal_continue
    stp x0, x1, [x16, #0]
    stp x2, x3, [x16, #16]
    stp x4, x5, [x16, #32]
    stp x6, x7, [x16, #48]
    stp x8, x9, [x16, #64]
    stp x10, x11, [x16, #80]
    stp x12, x13, [x16, #96]
    stp x14, x15, [x16, #112]
    ldr x17, [sp]
    str x17, [x16, #128]
    ldr x17, [sp, #8]
    str x17, [x16, #136]
    stp x18, x19, [x16, #144]
    stp x20, x21, [x16, #160]
    stp x22, x23, [x16, #176]
    stp x24, x25, [x16, #192]
    stp x26, x27, [x16, #208]
    stp x28, x29, [x16, #224]
    str x30, [x16, #240]
    add x17, sp, #16
    str x17, [x16, #248]
    mrs x17, nzcv
    str x17, [x16, #256]
    mrs x17, fpcr
    str x17, [x16, #264]
    mrs x17, fpsr
    str x17, [x16, #272]
    mrs x17, tpidr_el0
    str x17, [x16, #280]
    mrs x17, tpidrro_el0
    str x17, [x16, #288]
    stp q0, q1, [x16, #304]
    stp q2, q3, [x16, #336]
    stp q4, q5, [x16, #368]
    stp q6, q7, [x16, #400]
    stp q8, q9, [x16, #432]
    stp q10, q11, [x16, #464]
    stp q12, q13, [x16, #496]
    stp q14, q15, [x16, #528]
    stp q16, q17, [x16, #560]
    stp q18, q19, [x16, #592]
    stp q20, q21, [x16, #624]
    stp q22, q23, [x16, #656]
    stp q24, q25, [x16, #688]
    stp q26, q27, [x16, #720]
    stp q28, q29, [x16, #752]
    stp q30, q31, [x16, #784]
.Loriginal_continue:
    ldp x16, x17, [sp], #16
    // The original synthetic body chooses ordinary return or a no-return tail.
    stp x16, x17, [sp, #-16]!
    ldr x16, fixture_control_literal
    ldr x16, [x16, #16]
    cbz x16, .Loriginal_return
    // Original body is permitted to use IP0 for its own tail call.
    str x16, [sp]
    ldp x16, x17, [sp], #16
    blr x16
    // The controlled callback must not return, matching the no-return case.
    mov x0, #99
    mov x8, #93
    svc #0 // Linux fixture exit failure, never a resumable breakpoint.
.Loriginal_return:
    ldp x16, x17, [sp], #16
    add sp, sp, #32
    ret
.p2align 3
fixture_control_literal:
    .quad 0
fixture_end:
.section .note.GNU-stack,"",%progbits

