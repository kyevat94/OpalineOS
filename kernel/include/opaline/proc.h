/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * User processes (kernel/proc/).
 *
 * A user process has its own address space: the ELF segments of the program,
 * a 64 KiB stack just below USER_STACK_TOP (with an unmapped guard page under
 * it) and, shared with every process, the kernel half. Each process has one
 * thread, which enters user mode at the ELF entry point with argc/argv on its
 * stack (System V ABI), file descriptors and a current directory.
 */
#ifndef OPALINE_PROC_H
#define OPALINE_PROC_H

#include <opaline/sched.h>
#include <opaline/types.h>

#define USER_STACK_TOP   0x00007FFFFFFFE000ULL
#define USER_STACK_PAGES 16
#define USER_LOAD_MIN    0x0000000000400000ULL   /* programs may not map the first 4 MiB */
#define ARGV_MAX         64
#define ARGV_BYTES_MAX   8192

/* Exit codes of processes killed by a CPU exception (128 + the POSIX signal number). */
#define SIGILL  4
#define SIGFPE  8
#define SIGSEGV 11
#define EXIT_SIGILL  (128 + SIGILL)
#define EXIT_SIGFPE  (128 + SIGFPE)
#define EXIT_SIGSEGV (128 + SIGSEGV)

/*
 * Starts a program from an ELF image in memory. `argv` is NULL-terminated (may be NULL).
 * With a parent, the child inherits its open files and current directory; without one,
 * descriptors 0-2 are the terminal and the directory is "/". NULL on error (`error` says why).
 */
struct process *process_spawn(const char *name, const void *elf, size_t size, const char *const *argv,
                              struct process *parent, int *error);
/* Same, reading the program from a file (relative to the parent's directory). */
struct process *process_spawn_path(const char *path, const char *const *argv, struct process *parent, int *error);

/* Waits for a process started by the kernel (parent NULL), frees it, returns its exit code. */
int process_wait(struct process *p);
/* wait4() for user processes: waits for a child of `parent` (pid -1 = any). */
i64 process_wait_child(struct process *parent, i64 pid, int *status, bool nohang);

/* Ends the calling user process. */
NORETURN void process_exit(int code);
/* Called for a CPU exception raised in user mode: reports it and kills the process. */
struct interrupt_frame;
NORETURN void process_fault(const struct interrupt_frame *f);

/* ELF loading (kernel/proc/elf.c): maps the segments into `page_table`. */
bool elf_load(u64 page_table, const void *elf, size_t size, u64 *entry, const char **error);

bool user_selftest(void);   /* kernel/proc/selftest.c */

#endif
