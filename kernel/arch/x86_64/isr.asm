; SPDX-License-Identifier: GPL-3.0-or-later
; Copyright (C) 2026 The OpalineOS authors
;
; Interrupt service routine stubs for the 32 CPU exceptions.
; Each stub pushes a uniform frame (see struct interrupt_frame in
; opaline/arch/idt.h) and calls interrupt_dispatch(frame).

bits 64
default rel

extern interrupt_dispatch

; exceptions without an error code: push a 0 so the frame is always the same
%macro ISR_NOERR 1
isr_%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

; exceptions where the CPU already pushed an error code
%macro ISR_ERR 1
isr_%1:
    push qword %1
    jmp isr_common
%endmacro

section .text

ISR_NOERR 0     ; #DE divide error
ISR_NOERR 1     ; #DB debug
ISR_NOERR 2     ;     NMI
ISR_NOERR 3     ; #BP breakpoint
ISR_NOERR 4     ; #OF overflow
ISR_NOERR 5     ; #BR bound range
ISR_NOERR 6     ; #UD invalid opcode
ISR_NOERR 7     ; #NM device not available
ISR_ERR   8     ; #DF double fault
ISR_NOERR 9     ;     coprocessor segment overrun
ISR_ERR   10    ; #TS invalid TSS
ISR_ERR   11    ; #NP segment not present
ISR_ERR   12    ; #SS stack fault
ISR_ERR   13    ; #GP general protection
ISR_ERR   14    ; #PF page fault
ISR_NOERR 15    ;     reserved
ISR_NOERR 16    ; #MF x87 floating point
ISR_ERR   17    ; #AC alignment check
ISR_NOERR 18    ; #MC machine check
ISR_NOERR 19    ; #XM SIMD floating point
ISR_NOERR 20    ; #VE virtualization
ISR_ERR   21    ; #CP control protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28    ; #HV hypervisor injection
ISR_ERR   29    ; #VC VMM communication
ISR_ERR   30    ; #SX security
ISR_NOERR 31

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
    call interrupt_dispatch
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
%rep 32
    dq isr_ %+ i
%assign i i + 1
%endrep
