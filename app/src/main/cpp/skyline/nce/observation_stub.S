// SPDX-License-Identifier: MPL-2.0
// Template of the dedicated post-BL, pre-original-PLT producer. X16/X17 only;
// original authenticated PLT overwrites both. No stack access, flag-setting
// instruction, C++ call, TLS write, syscall, allocation or retry loop.
.text
.p2align 2
.global PineObservationStubBegin, PineObservationStubTail, PineObservationStubEnd
PineObservationStubBegin:
    mrs x16,tpidr_el0
    ldr x16,[x16,#0x2e0]
    cbz x16,.Ltail
    ldr x17,[x16,#8]
    ldar x17,[x17]
    ldr x16,[x16,#16]
    eor x16,x16,x17
    cbnz x16,.Ltail
    mrs x16,tpidr_el0
    ldr x16,[x16,#0x2e0]
    ldr x17,[x16,#24]
    ldar x17,[x17]
    ldr x16,[x16,#32]
    eor x16,x16,x17
    cbnz x16,.Ltail
    mrs x16,tpidr_el0
    ldr x17,[x16,#0x2e0]
    ldr x16,[x17,#40]
    ldar x16,[x16]
    sub x16,x16,#1
    cbnz x16,.Ltail
    ldar x16,[x17]
    cbnz x16,.Ltail
    mov x16,#1
    str x16,[x17]
    str x0,[x17,#48]
    str x30,[x17,#56]
    mov x16,sp
    str x16,[x17,#64]
    adr x16,.
    str x16,[x17,#72]
    // Recheck lifetime/generations after payload. A late writer keeps its old
    // slot/owner; failed revalidation leaves Writing, never a fabricated record.
    ldr x16,[x17,#40]
    ldar x16,[x16]
    sub x16,x16,#1
    cbnz x16,.Ltail
    ldr x16,[x17,#8]
    ldar x16,[x16]
    ldr x17,[x17,#16]
    eor x16,x16,x17
    cbnz x16,.Ltail
    mrs x16,tpidr_el0
    ldr x17,[x16,#0x2e0]
    ldr x16,[x17,#24]
    ldar x16,[x16]
    ldr x17,[x17,#32]
    eor x16,x16,x17
    cbnz x16,.Ltail
    mrs x16,tpidr_el0
    ldr x17,[x16,#0x2e0]
    mov x16,#2
    stlr x16,[x17]
.Ltail:
PineObservationStubTail:
    b . // emitter replaces exactly this B with the original PLT target
PineObservationStubEnd:
.section .note.GNU-stack,"",%progbits
