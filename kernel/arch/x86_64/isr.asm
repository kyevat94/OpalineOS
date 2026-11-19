; SPDX-License-Identifier: GPL-3.0-or-later
; Copyright (C) 2026 The OpalineOS authors
;
; Interrupt service routine stubs for all 256 vectors (0-31: CPU exceptions,
; 32-255: hardware and software interrupts). Each stub pushes a uniform frame
; (see struct interrupt_frame in opaline/arch/idt.h) and calls
; interrupt_dispatch(frame).

bits 64
default rel

extern interrupt_dispatch

section .text

; Exceptions where the CPU pushes an error code: 8, 10-14, 17, 21, 29, 30.
; For every other vector the stub pushes a 0 so the frame is always the same.
%assign i 0
%rep 256
isr_ %+ i:
%if i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30
    push qword i
%else
    push qword 0
    push qword i
%endif
    jmp isr_common
%assign i i + 1
%endrep

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp                ; struct interrupt_frame *
    mov rbx, rsp                ; rbx is callee-saved: remember the frame
    and rsp, -16                ; the C ABI wants a 16-byte aligned stack
    call interrupt_dispatch     ; may switch to another thread and come back later
    mov rsp, rbx
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                 ; vector + error code
    iretq

section .rodata
global isr_stubs
isr_stubs:
%assign i 0
%rep 256
    dq isr_ %+ i
%assign i i + 1
%endrep
