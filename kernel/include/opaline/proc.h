/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * User processes (kernel/proc/).
 *
 * A user process has an address space (struct mm, opaline/uvm.h), file
 * descriptors, a current directory, signal handlers and one or more threads.
 * Threads of one process share all of it (clone with CLONE_THREAD); fork makes
 * a copy; execve replaces the program. A process ends when its last thread
 * exits, and stays a zombie until its parent (or, for processes started by the
 * kernel, the kernel) collects the exit status.
 *
 * Process and thread ids come from one counter: the main thread's id is the
 * process id, as on Linux.
 */
#ifndef OPALINE_PROC_H
#define OPALINE_PROC_H

#include <opaline/sched.h>
#include <opaline/types.h>

#define ARGV_MAX         4096        /* arguments + environment strings */
#define ARGS_BYTES_MAX   (128 * KiB)  /* their total size (ARG_MAX) */

/* Signals (Linux numbers) */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG  23
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28

/* Exit codes of processes killed by a CPU exception (128 + the POSIX signal number). */
#define EXIT_SIGILL  (128 + SIGILL)
#define EXIT_SIGFPE  (128 + SIGFPE)
#define EXIT_SIGSEGV (128 + SIGSEGV)

struct interrupt_frame;
struct mm;

/*
 * Starts a program file. `argv` and `envp` are NULL-terminated (may be NULL; envp NULL
 * gives the default environment). With a parent, the child inherits its open files and
 * current directory; without one, descriptors 0-2 are the terminal and the directory
 * is "/". NULL on error (`error` says why).
 */
#define SPAWN_FOREGROUND 1   /* the child gets a process group of its own, in the terminal's foreground */
struct process *process_spawn_path(const char *path, const char *const *argv, struct process *parent, int flags,
                                   int *error);

/* Waits for a process started by the kernel (parent NULL), frees it, returns its exit
   code (128 + signal if a signal killed it). */
int process_wait(struct process *p);
/* wait4() for user processes: waits for a child of `parent` (pid -1 = any, 0 = own group, < -1 = that
   group). options: WNOHANG 1, WUNTRACED 2 (report stopped children), WCONTINUED 8. */
i64 process_wait_child(struct process *parent, i64 pid, int *status, u64 options);

/* Job control: process groups, sessions, signals to a group. */
i64 process_setpgid(u64 pid, u64 pgid);
i64 process_getpgid(u64 pid);
i64 process_setsid(void);
i64 process_getsid(u64 pid);
bool process_group_exists(u64 pgid, u64 sid);
/* Sends sig to every process of the group; with `check`, only to those the caller may signal.
   0, -ESRCH (no such group) or -EPERM (none allowed). */
int process_signal_group(u64 pgid, int sig, bool check);
/* kill(-1): every process the caller may signal, except pid 1 and the caller itself. */
int process_signal_all(struct process *caller, int sig);
/* A child stopped or continued (p->report says which): wakes its parent's wait4, sends SIGCHLD. */
void process_notify_parent(struct process *p);

/* exit_group(): ends every thread of the calling process. */
NORETURN void process_exit(int code);
/* exit(): ends the calling thread (and the process with its last thread). */
NORETURN void thread_exit_user(int code);
/* clone(), fork() and vfork(): returns the new thread id or -errno. */
i64 process_clone(struct interrupt_frame *f, u64 flags, u64 stack, u64 parent_tid, u64 child_tid, u64 tls);
/* execve() with the arguments already in kernel memory. 0 (the frame now starts the program) or -errno. */
int process_execve(struct interrupt_frame *f, const char *path, const char *const *argv, const char *const *envp);

struct process *process_by_pid(u64 pid);       /* NULL if none (or a zombie) */
struct thread *thread_by_tid(u64 tid);
/* Ends the process because of signal `sig` (every thread leaves at its next return to user mode). */
void process_kill(struct process *p, int sig);

/* Signals (kernel/proc/signal.c) */
int signal_send_process(struct process *p, int sig);
int signal_send_thread(struct thread *t, int sig);
bool signal_blocked_or_ignored(struct thread *t, int sig);
/* Runs on every return to user mode: ends the thread if the process is ending, delivers signals. */
void user_return(struct interrupt_frame *f);
/* A CPU exception in user mode: becomes a signal (a handler may catch it) or kills the process. */
void process_fault(struct interrupt_frame *f);
void signals_exec_reset(struct process *p);
i64 sys_rt_sigaction(u64 sig, u64 act, u64 oact, u64 size);
i64 sys_rt_sigprocmask(u64 how, u64 set, u64 oset, u64 size);
i64 sys_rt_sigreturn(struct interrupt_frame *f);
i64 sys_sigaltstack(u64 ss, u64 old, u64 sp);
/* May an -EINTR system call start again after a SA_RESTART handler? (kernel/proc/syscalls.c) */
bool syscall_restartable(u64 nr);
i64 sys_rt_sigpending(u64 set, u64 size);
i64 sys_rt_sigsuspend(u64 set, u64 size);
i64 signal_suspend(u64 mask);   /* waits for a signal with `mask` blocked; -EINTR */

/* Interval timers (kernel/proc/itimer.c): setitimer, getitimer, alarm. */
i64 sys_setitimer(u64 which, u64 new_value, u64 old_value);
i64 sys_getitimer(u64 which, u64 value);
i64 sys_alarm(u64 seconds);
/* Timer interrupt (interrupts disabled): counts the timers down; p is the process that was
   running (NULL: none), user_mode whether it was in user mode. */
void itimer_tick(u64 now, struct process *p, bool user_mode);
void itimer_cancel_all(struct process *p);   /* the process ends */

/* Futexes (kernel/proc/futex.c) */
i64 sys_futex(u64 addr, u64 op, u64 val, u64 timeout, u64 addr2, u64 val3);
int futex_wake(struct mm *mm, u64 addr, int count);

/* ELF loading (kernel/proc/elf.c) */
struct elf_info {
    u64 entry;          /* entry point (relocated) */
    u64 base;           /* where an ET_DYN image was placed (0 for ET_EXEC) */
    u64 phdr;           /* address of the program headers in memory */
    u64 phnum;
    u64 end;            /* end of the highest segment */
    char interp[128];   /* PT_INTERP, "" if none */
};
bool elf_load(struct mm *mm, const void *elf, size_t size, u64 base, struct elf_info *info, const char **error);

/* Kernel randomness for AT_RANDOM and getrandom() (kernel/proc/random.c) */
void random_bytes(void *buf, u64 len);

bool user_selftest(void);   /* kernel/proc/selftest.c */
bool posix_selftest(void);  /* runs /apps/posixtest (stage E6) */

#endif
