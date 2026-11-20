; SPDX-License-Identifier: GPL-3.0-or-later
; Copyright (C) 2026 The OpalineOS authors
;
; SYSCALL entry and the ways back to user mode.
;
; GS is only used here: while switching stacks, SWAPGS makes GS point at the
; per-CPU block (struct cpu_local in syscall.c: [0] kernel stack top,
; [8] scratch for the user stack pointer). Interrupts are off (SFMASK clears
; IF) until the kernel stack is in use and GS is swapped back.
;
; The entry saves the same frame as an interrupt (struct interrupt_frame), so
; signals, fork and execve handle both alike. The usual way back is SYSRET;
; when the kernel rewrote the user registers (a signal handler starts, a
; handler returns, a new program starts), syscall_dispatch returns non-zero and
; the frame is restored in full with IRETQ (SYSRET would lose RCX and R11).

bits 64
default rel

extern syscall_dispatch

section .text

%macro POP_GPRS 0
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
%endmacro

global syscall_entry
syscall_entry:
    swapgs
    mov [gs:8], rsp             ; user stack pointer
    mov rsp, [gs:0]             ; this thread's kernel stack
    push qword 0x1B             ; frame.ss
    push qword [gs:8]           ; frame.rsp
    swapgs
    push r11                    ; frame.rflags
    push qword 0x23             ; frame.cs
    push rcx                    ; frame.rip
    push qword 0                ; frame.error
    push qword 0x100            ; frame.vector: 0x100 marks a system call
    push rax                    ; system call number, then the result
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
    push r15                    ; 22 pushes: the stack is still 16-byte aligned
    mov rdi, rsp                ; struct interrupt_frame *
    sti
    call syscall_dispatch       ; may sleep, switch threads, or never return (exit)
    cli
    test eax, eax
    jnz .full
    POP_GPRS
    add rsp, 16                 ; vector + error
    mov rcx, [rsp]              ; user RIP
    mov r11, [rsp + 16]         ; user RFLAGS
    mov rsp, [rsp + 24]         ; user stack
    o64 sysret
.full:
    POP_GPRS
    add rsp, 16
    iretq

; void return_to_user(struct interrupt_frame *f): loads every register from the
; frame (on the current kernel stack) and enters user mode. Used by new threads.
global return_to_user
return_to_user:
    cli
    mov rsp, rdi
    POP_GPRS
    add rsp, 16
    iretq

; void jump_to_user(u64 rip, u64 rsp)
global jump_to_user
jump_to_user:
    cli
    push qword 0x1B             ; SS: user data, RPL 3
    push rsi                    ; RSP
    push qword 0x202            ; RFLAGS: interrupts on
    push qword 0x23             ; CS: user code, RPL 3
    push rdi                    ; RIP
    xor eax, eax                ; do not leak kernel values into user registers
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq
