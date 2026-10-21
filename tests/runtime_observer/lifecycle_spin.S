// SPDX-License-Identifier: MPL-2.0
.text
.global LifecycleSpin
.type LifecycleSpin,%function
LifecycleSpin:
    adrp x16, LifecycleGuestEntered
    add x16, x16, :lo12:LifecycleGuestEntered
    mov x17, #1
    stlr x17, [x16]
1:  b 1b
.size LifecycleSpin, .-LifecycleSpin
.section .note.GNU-stack,"",%progbits
