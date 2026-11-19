; SPDX-License-Identifier: LGPL-3.0-or-later
; Copyright (C) 2026 The OpalineOS authors
;
; Program entry point. The kernel starts a process here with
; [rsp] = argc, [rsp+8] = argv[], then envp[] and the auxiliary vector.

bits 64
default rel

extern main
extern _exit

section .text
global _start
_start:
    xor ebp, ebp                ; end of the frame-pointer chain
    mov rdi, [rsp]              ; argc
    lea rsi, [rsp + 8]          ; argv
    and rsp, -16
    call main
    mov edi, eax
    call _exit
.unreachable:
    jmp .unreachable
