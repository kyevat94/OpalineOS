; SPDX-License-Identifier: GPL-3.0-or-later
; Copyright (C) 2026 The OpalineOS authors
;
; Kernel entry point and small CPU helpers.
; The bootloader jumps here with RDI = struct boot_info * (see boot/bootinfo.h).

bits 64
default rel

section .bss
align 16
global kernel_stack_bottom, kernel_stack_top
kernel_stack_bottom:
    resb 64 * 1024
kernel_stack_top:

section .text.entry
global _start
extern kmain
_start:
    cli
    lea rsp, [kernel_stack_top]
    xor ebp, ebp                ; end of the frame-pointer chain (stack traces stop here)
    call kmain                  ; RDI = boot_info, passed through
.halt:
    cli
    hlt
    jmp .halt

section .text
; void gdt_load(const struct gdt_pointer *gdtr)
; Loads the GDT and reloads every segment register (CS through a far return).
global gdt_load
gdt_load:
    lgdt [rdi]
    mov ax, 0x10                ; kernel data
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    pop rax                     ; return address
    push qword 0x08             ; kernel code
    push rax
    retfq

; void tss_load(u16 selector)
global tss_load
tss_load:
    ltr di
    ret

; void idt_load(const struct idt_pointer *idtr)
global idt_load
idt_load:
    lidt [rdi]
    ret
