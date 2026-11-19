/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * The panic screen. The first line always starts with "PANIC: " on COM1,
 * which the automated tests look for.
 */
#include <opaline/arch/cpu.h>
#include <opaline/arch/idt.h>
#include <opaline/console.h>
#include <opaline/mm.h>
#include <opaline/panic.h>
#include <opaline/sched.h>
#include <opaline/serial.h>
#include <opaline/string.h>

extern char __kernel_start[], __kernel_end[];

static const char *const exception_names[32] = {
    "Divide error (#DE)",           "Debug (#DB)",
    "Non-maskable interrupt (NMI)", "Breakpoint (#BP)",
    "Overflow (#OF)",               "Bound range exceeded (#BR)",
    "Invalid opcode (#UD)",         "Device not available (#NM)",
    "Double fault (#DF)",           "Coprocessor segment overrun",
    "Invalid TSS (#TS)",            "Segment not present (#NP)",
    "Stack-segment fault (#SS)",    "General protection fault (#GP)",
    "Page fault (#PF)",             "Reserved exception 15",
    "x87 floating-point error (#MF)", "Alignment check (#AC)",
    "Machine check (#MC)",          "SIMD floating-point error (#XM)",
    "Virtualization exception (#VE)", "Control protection (#CP)",
    "Reserved exception 22",        "Reserved exception 23",
    "Reserved exception 24",        "Reserved exception 25",
    "Reserved exception 26",        "Reserved exception 27",
    "Hypervisor injection (#HV)",   "VMM communication (#VC)",
    "Security exception (#SX)",     "Reserved exception 31",
};

static volatile int panicking;

/* Clears the screen and prints the panic header. Returns false on a nested panic. */
static bool panic_begin(void) {
    cpu_cli();
    if (panicking++) {
        static const char msg[] = "\nPANIC: nested panic, halting\n";
        serial_write(msg, sizeof msg - 1);
        return false;
    }
    if (console_has_screen()) console_clear(COLOR_BACKGROUND);
    console_set_color(COLOR_ERROR);
    return true;
}

bool panic_in_progress(void) { return panicking != 0; }

static void print_thread(void) {
    struct thread *t = thread_current();
    if (t) kprintf("  Thread: \"%s\" (id %lu, process %lu)\n", t->name, t->id, t->process ? t->process->pid : 0);
}

static NORETURN void panic_end(void) {
    console_set_color(COLOR_SECONDARY);
    kprintf("\nThe system has stopped. Restart the computer.\n");
    cpu_halt_forever();
}

/* A frame pointer worth following: in the kernel image or a kernel stack, and mapped. */
static bool is_stack_address(u64 a) {
    bool kernel_image = a >= (u64)__kernel_start && a < (u64)__kernel_end;
    bool thread_stack = a >= 0xFFFFB00000000000ULL && a < 0xFFFFB10000000000ULL;
    return (kernel_image || thread_stack) && vmm_translate(a) != VMM_NOT_MAPPED &&
           vmm_translate(a + 8) != VMM_NOT_MAPPED;
}

/* Follows the RBP chain (the kernel is built with frame pointers). */
static void stack_trace(u64 rip, u64 rbp) {
    kprintf("Stack trace:\n");
    kprintf("  #0  %p\n", (void *)rip);
    for (int i = 1; i < 16 && rbp && !(rbp & 7) && is_stack_address(rbp); i++) {
        u64 *frame = (u64 *)rbp;
        u64 ret = frame[1];
        if (!ret) break;
        kprintf("  #%-2d %p\n", i, (void *)ret);
        rbp = frame[0];
    }
}

void panic(const char *fmt, ...) {
    if (!panic_begin()) cpu_halt_forever();
    kprintf("PANIC: ");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");
    console_set_color(COLOR_TEXT);
    print_thread();
    stack_trace((u64)__builtin_return_address(0), (u64)__builtin_frame_address(0));
    panic_end();
}

static void describe_page_fault(u64 error) {
    kprintf("  Cause: %s %s in %s mode%s%s\n",
            error & 1 ? "protection violation on" : "not-present page on",
            error & 16 ? "instruction fetch" : error & 2 ? "write" : "read",
            error & 4 ? "user" : "kernel",
            error & 8 ? ", reserved bit set" : "",
            error & 32 ? ", protection key" : "");
}

void panic_exception(const struct interrupt_frame *f) {
    if (!panic_begin()) cpu_halt_forever();
    const char *name = f->vector < 32 ? exception_names[f->vector] : "Unexpected interrupt";
    kprintf("PANIC: %s at RIP=%p\n", name, (void *)f->rip);
    console_set_color(COLOR_TEXT);
    print_thread();
    kprintf("  Vector %lu, error code %#lx\n", f->vector, f->error);
    if (f->vector == 14) {
        kprintf("  CR2=%p (the address that was accessed)\n", (void *)read_cr2());
        describe_page_fault(f->error);
    }
    /* a thread stack that ran into its guard page: the stack pointer sits just above an unmapped page */
    bool in_stack_region = f->rsp >= 0xFFFFB00000000000ULL && f->rsp < 0xFFFFB10000000000ULL;
    if ((f->vector == 8 || f->vector == 14) && in_stack_region && vmm_translate(f->rsp - 8) == VMM_NOT_MAPPED)
        kprintf("  Cause: kernel stack overflow (the thread used up its stack and hit the guard page)\n");
    kprintf("\n");
    kprintf("  RAX=%p  RBX=%p  RCX=%p\n", (void *)f->rax, (void *)f->rbx, (void *)f->rcx);
    kprintf("  RDX=%p  RSI=%p  RDI=%p\n", (void *)f->rdx, (void *)f->rsi, (void *)f->rdi);
    kprintf("  RBP=%p  RSP=%p  R8 =%p\n", (void *)f->rbp, (void *)f->rsp, (void *)f->r8);
    kprintf("  R9 =%p  R10=%p  R11=%p\n", (void *)f->r9, (void *)f->r10, (void *)f->r11);
    kprintf("  R12=%p  R13=%p  R14=%p\n", (void *)f->r12, (void *)f->r13, (void *)f->r14);
    kprintf("  R15=%p  RFLAGS=%p\n", (void *)f->r15, (void *)f->rflags);
    kprintf("  CS=%#lx  SS=%#lx  CR0=%p  CR3=%p  CR4=%p\n", f->cs, f->ss, (void *)read_cr0(),
            (void *)read_cr3(), (void *)read_cr4());
    kprintf("\n");
    stack_trace(f->rip, f->rbp);
    panic_end();
}
