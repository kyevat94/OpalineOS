/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Processes, threads and the scheduler (kernel/sched/sched.c).
 *
 * A process owns an address space; threads run inside a process. Until stage
 * E4 there is only the kernel process (pid 0) and its kernel threads.
 *
 * Scheduling is preemptive round-robin: the LAPIC timer interrupts 1000 times
 * per second and a thread that used up its time slice (10 ms) goes to the back
 * of the ready queue. When nothing is ready, the idle thread halts the CPU.
 */
#ifndef OPALINE_SCHED_H
#define OPALINE_SCHED_H

#include <opaline/cred.h>
#include <opaline/types.h>

#define TIMER_HZ        1000
#define TIME_SLICE_MS   10
#define THREAD_STACK_PAGES 4      /* 16 KiB kernel stack per thread */

enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DEAD,
};

struct file;
struct vnode;
struct mm;

/* A list of blocked threads (see waitqueue_wait below). */
struct waitqueue {
    struct thread *head;
};

#define PROCESS_MAX_FDS 64
#define NSIG 65                   /* signals 1..64, as in Linux */

/* What a process does with a signal (struct k_sigaction of Linux rt_sigaction). */
struct sigaction_k {
    u64 handler;              /* 0 = default, 1 = ignore, else the handler address */
    u64 flags;                /* SA_* */
    u64 restorer;             /* where the handler returns to (SA_RESTORER) */
    u64 mask;                 /* signals blocked while the handler runs */
};

enum process_state { PROCESS_ALIVE, PROCESS_ZOMBIE };

/* An interval timer (setitimer), in timer ticks. ITIMER_REAL: `value` is the tick it expires at;
   ITIMER_VIRTUAL and ITIMER_PROF: the ticks left. 0 = not running. */
struct itimer {
    u64 value, interval;
};

struct process {
    u64 pid;
    char name[32];
    u64 page_table;           /* physical address of the PML4 (mm->page_table for user processes) */
    u64 thread_count;         /* threads not yet freed (maintained by the scheduler) */
    bool user;                /* has a user-mode address space */
    struct thread *main_thread;
    struct mm *mm;            /* user memory (stage E6); shared by CLONE_VM */
    struct cred cred;         /* user and groups (opaline/cred.h) */
    u32 umask;
    /* job control (stage SE1) */
    u64 pgid, sid;            /* process group and session */
    bool ctty;                /* the terminal is its controlling terminal */
    bool stopped;             /* SIGSTOP/SIGTSTP...: every thread waits for SIGCONT */
    int stop_sig;
    int report;               /* for the parent's wait4: 1 stopped, 2 continued, 0 nothing new */
    struct waitqueue stop_wq;
    /* files (stage E5) */
    struct file *fds[PROCESS_MAX_FDS];
    u8 fd_flags[PROCESS_MAX_FDS];   /* FD_CLOEXEC */
    struct vnode *cwd;
    char cwd_path[256];
    /* family and lifetime (stage E6) */
    struct process *parent;   /* NULL: started by the kernel, or its parent ended */
    bool orphan;              /* nobody will wait: freed when it ends */
    enum process_state state;
    int refs;                 /* one per thread not yet freed, one until reaped */
    int live_threads;         /* threads that have not exited */
    struct thread *threads;   /* linked through thread->proc_next */
    bool exiting;             /* exit_group or a fatal signal: every thread must leave */
    int exit_code;            /* for exit_group */
    int signal;               /* set when killed by a signal (POSIX signal number) */
    bool vfork_done;          /* a CLONE_VFORK child exec'd or ended */
    struct thread *exec_thread;   /* a thread in execve, waiting for the others to end */
    /* signals (stage E6) */
    struct sigaction_k sigactions[NSIG];
    u64 sigpending;           /* sent to the process, taken by any thread that does not block it */
    /* interval timers (stage SE4, kernel/proc/itimer.c): kept across execve, not inherited by fork */
    struct itimer itimers[3];
    struct process *itimer_next;   /* list of processes whose ITIMER_REAL runs */
    struct process *all_next;
    struct process *reap_next;   /* temporary list while a parent ends */
};

struct thread {
    u8 fpu[512] ALIGNED(16);  /* FPU/SSE registers (fxsave area), saved on every switch */
    u64 id;
    char name[32];
    enum thread_state state;
    struct process *process;
    u64 rsp;                  /* saved stack pointer while not running */
    u64 stack_top;            /* 0 for the boot thread (static stack) */
    u64 wake_tick;            /* when a blocked thread times out (0: no timeout) */
    int exit_code;
    bool joinable;
    bool interruptible;       /* blocked in a wait that thread_interrupt may end */
    int wake_reason;          /* result of thread_block: 0, -ETIMEDOUT or -EINTR */
    volatile bool intr;       /* a signal or exit is pending: interruptible waits end at once */
    struct waitqueue *wq;     /* wait queue the thread is blocked on */
    struct thread *joiner;    /* thread waiting in thread_join */
    void (*on_free)(struct thread *t);   /* called just before the thread is freed */
    struct thread *next;      /* run queue / wait queue / dead list */
    struct thread *sleep_next;/* list of threads with a timeout */
    struct thread *all_next;  /* list of every thread */
    /* user threads (stage E6) */
    u64 tid;                  /* thread id seen by programs (the pid for the main thread) */
    struct thread *proc_next; /* the process's threads */
    u64 fs_base;              /* FS segment base: thread-local storage */
    u64 clear_child_tid;      /* user address zeroed and futex-woken at exit */
    u64 sigmask;              /* blocked signals */
    u64 sigpending;           /* sent to this thread */
    u64 saved_sigmask;        /* sigsuspend: the mask to restore */
    bool restore_sigmask;
    u64 altstack_sp, altstack_size;
    int altstack_flags;
    bool iret_return;         /* the user registers were rewritten: return through IRETQ */
    bool exit_pending;        /* another thread's execve: end at the next return to user mode */
    bool restart_syscall;     /* a stop interrupted it: an EINTR system call starts again */
    u64 fault_addr;           /* CR2 of its last user page fault (read before interrupts come back on) */
    bool io_nonblock;        /* the file being read or written has O_NONBLOCK: -EAGAIN, never wait */
    void *start_frame;        /* new user thread: the registers it starts with (kmalloc'd) */
};

typedef void (*thread_fn)(void *arg);

/* Turns the boot context into the first thread ("kmain") and creates the idle thread. */
void sched_init(void);
/* Called by the timer interrupt (interrupts disabled); `user_mode`: it interrupted user code. */
void sched_tick(bool user_mode);

struct thread *thread_create(const char *name, thread_fn fn, void *arg);   /* NULL if out of memory */
/* Same, in another process (its address space is used while the thread runs). */
struct thread *thread_create_in(struct process *p, const char *name, thread_fn fn, void *arg);
struct process *sched_kernel_process(void);
struct thread *thread_current(void);
void thread_yield(void);
void thread_sleep_ms(u64 ms);
/* Sleeps unless interrupted: 0 when the time passed, -EINTR if thread_interrupt woke it. */
int thread_sleep_intr(u64 ms);
NORETURN void thread_exit(int code);
int thread_join(struct thread *t);    /* waits for t to finish, frees it, returns its exit code */
void thread_detach(struct thread *t); /* nobody will join: freed automatically when it exits */

/*
 * Wait queues: a thread blocks until another one wakes the queue.
 * Pattern (interrupts must be disabled around the check and the wait):
 *     u64 f = irq_save();
 *     while (!condition) waitqueue_wait(&wq);
 *     ...
 *     irq_restore(f);
 * waitqueue_wait_intr() also returns -EINTR when the thread is interrupted (a signal
 * arrived, or the process is ending); the caller then gives up and returns -EINTR.
 */
void waitqueue_wait(struct waitqueue *wq);      /* interrupts disabled */
int waitqueue_wait_intr(struct waitqueue *wq);  /* interrupts disabled; 0 or -EINTR */
/* Interruptible, at most `ms` milliseconds (0: no limit): 0, -ETIMEDOUT or -EINTR. */
int waitqueue_wait_timeout(struct waitqueue *wq, u64 ms);
void waitqueue_wake_all(struct waitqueue *wq);

/*
 * Lower-level blocking (interrupts disabled): the current thread blocks until
 * thread_wake(), the timeout (ms, 0 = none) or, if `interruptible`,
 * thread_interrupt(). Returns 0, -ETIMEDOUT or -EINTR.
 */
int thread_block(u64 timeout_ms, bool interruptible);
void thread_wake(struct thread *t);             /* interrupts disabled */
/* Sets t->intr and ends an interruptible wait of t. */
void thread_interrupt(struct thread *t);
/* Every thread (for lookups by the process code); interrupts disabled while walking it. */
struct thread *sched_all_threads(void);
/* Frees the dead threads nobody joins now, instead of waiting for the idle thread. */
void sched_free_dead(void);
/* Loads the initial FPU/SSE state into the registers (a new program starts clean). */
void thread_reset_fpu(void);
/* The MXCSR bits this CPU supports (MXCSR_MASK from fxsave, read once in sched_init). */
u32 fpu_mxcsr_mask(void);

/* Sleeping lock: the waiting thread blocks instead of spinning (may be held across disk I/O). */
struct mutex {
    struct thread *owner;
    struct waitqueue waiters;
};
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);

u64 sched_ticks(void);                /* timer ticks since the timer started */
static inline u64 time_ms(void) { return sched_ticks() * 1000 / TIMER_HZ; }
u64 sched_context_switches(void);
u64 sched_task_count(void);           /* threads that have not ended, the idle thread aside */

bool sched_selftest(void);            /* kernel/sched/selftest.c */

#endif
