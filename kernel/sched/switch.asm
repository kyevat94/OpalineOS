; SPDX-License-Identifier: GPL-3.0-or-later
; Copyright (C) 2026 The OpalineOS authors
;
; Context switch between kernel threads.

bits 64
default rel

section .text

; void switch_context(u64 *old_rsp, u64 new_rsp)
; Saves the callee-saved registers on the current stack, stores the stack
; pointer in *old_rsp, switches to new_rsp and restores that thread's
; registers. Returns in the other thread, wherever it called switch_context
; (or in thread_trampoline for a new thread). Everything else was already
; saved by the C caller, as the calling convention requires.
global switch_context
switch_context:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; First code a new thread runs. thread_create() puts the entry function in
; r12 and its argument in r13 (see the initial frame in sched.c).
global thread_trampoline
extern thread_exit
thread_trampoline:
    sti                         ; the scheduler switched here with interrupts off
    mov rdi, r13
    call r12
    xor edi, edi                ; the function returned: exit code 0
    call thread_exit
.unreachable:
    hlt
    jmp .unreachable
