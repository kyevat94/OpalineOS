/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * User processes (kernel/proc/).
 *
 * A user process has its own address space: the ELF segments of the program,
 * a 64 KiB stack just below USER_STACK_TOP (with an unmapped guard page under
 * it) and, shared with every process, the kernel half. For now each process
 * has one thread, which enters user mode at the ELF entry point.
 */
#ifndef OPALINE_PROC_H
#define OPALINE_PROC_H

#include <opaline/sched.h>
#include <opaline/types.h>

#define USER_STACK_TOP   0x00007FFFFFFFE000ULL
#define USER_STACK_PAGES 16
#define USER_LOAD_MIN    0x0000000000400000ULL   /* programs may not map the first 4 MiB */

/* Exit codes of processes killed by a CPU exception (128 + the POSIX signal number). */
#define EXIT_SIGILL  (128 + 4)
#define EXIT_SIGFPE  (128 + 8)
#define EXIT_SIGSEGV (128 + 11)

/* Creates a process from an ELF executable in memory and starts it. NULL on error (logged). */
struct process *process_spawn(const char *name, const void *elf, size_t size);
/* Waits for the process to end, frees it and returns its exit code. */
int process_wait(struct process *p);
/* Ends the calling user process. */
NORETURN void process_exit(int code);
/* Called for a CPU exception raised in user mode: reports it and kills the process. */
struct interrupt_frame;
NORETURN void process_fault(const struct interrupt_frame *f);

/* ELF loading (kernel/proc/elf.c): maps the segments into `page_table`. */
bool elf_load(u64 page_table, const void *elf, size_t size, u64 *entry, const char **error);

/* Programs built into the kernel image (tools/build.py generates the table). */
struct embedded_program {
    const char *name;
    const u8 *start;
    const u8 *end;
};
const struct embedded_program *embedded_program_find(const char *name);

bool user_selftest(void);   /* kernel/proc/selftest.c */

#endif
