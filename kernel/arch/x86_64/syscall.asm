; SPDX-License-Identifier: GPL-3.0-or-later
; Copyright (C) 2026 The OpalineOS authors
;
; SYSCALL entry and the first jump into user mode.
;
; GS is only used here: while switching stacks, SWAPGS makes GS point at the
; per-CPU block (struct cpu_local in syscall.c: [0] kernel stack top,
; [8] scratch for the user stack pointer). Interrupts are off (SFMASK clears
; IF) until the kernel stack is in use and GS is swapped back.

bits 64
default rel

extern syscall_dispatch

section .text

global syscall_entry
syscall_entry:
    swapgs
    mov [gs:8], rsp             ; user stack pointer
    mov rsp, [gs:0]             ; this thread's kernel stack
    push qword [gs:8]           ; frame.rsp
    swapgs
    push r11                    ; frame.rflags
    push rcx                    ; frame.rip
    push rax                    ; frame.rax (system call number)
    push rbx
    push rbp
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9
    push r12
    push r13
    push r14
    push r15                    ; 16 pushes: the stack is still 16-byte aligned
    mov rdi, rsp                ; struct syscall_frame *
    sti
    call syscall_dispatch       ; may sleep, switch threads, or never return (exit)
    cli
    pop r15
    pop r14
    pop r13
    pop r12
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop rbx
    pop rax                     ; result
    pop rcx                     ; user RIP
    pop r11                     ; user RFLAGS
    pop rsp                     ; user stack
    o64 sysret

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
