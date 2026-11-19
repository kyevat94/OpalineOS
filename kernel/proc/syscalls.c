/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * System calls. Numbers, arguments and error codes follow Linux x86-64
 * (mixed ABI, project.md section 3.1); OpalineOS's own calls will use
 * numbers from 1000 on. Anything not implemented returns -ENOSYS.
 *
 * Every user pointer is checked against the calling process's page tables
 * before the kernel touches it (-EFAULT otherwise).
 */
#include <opaline/arch/syscall.h>
#include <opaline/console.h>
#include <opaline/errno.h>
#include <opaline/mm.h>
#include <opaline/proc.h>
#include <opaline/sched.h>

#define SYS_write          1
#define SYS_sched_yield   24
#define SYS_nanosleep     35
#define SYS_getpid        39
#define SYS_exit          60
#define SYS_gettid       186
#define SYS_clock_gettime 228
#define SYS_exit_group   231

struct timespec {
    i64 tv_sec;
    i64 tv_nsec;
};

static u64 page_table(void) { return thread_current()->process->page_table; }

static bool user_ok(u64 addr, u64 len, bool write) { return vmm_user_range_ok(page_table(), addr, len, write); }

static i64 sys_write(u64 fd, u64 buf, u64 len) {
    if (fd != 1 && fd != 2) return -EBADF;   /* only stdout and stderr (the console) exist so far */
    if (!user_ok(buf, len, false)) return -EFAULT;
    kputs((const char *)buf, len);
    return (i64)len;
}

static i64 sys_nanosleep(u64 req, u64 rem) {
    if (!user_ok(req, sizeof(struct timespec), false)) return -EFAULT;
    if (rem && !user_ok(rem, sizeof(struct timespec), true)) return -EFAULT;
    struct timespec t = *(const struct timespec *)req;
    if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000) return -EINVAL;
    thread_sleep_ms((u64)t.tv_sec * 1000 + ((u64)t.tv_nsec + 999999) / 1000000);
    if (rem) *(struct timespec *)rem = (struct timespec){0, 0};
    return 0;
}

static i64 sys_clock_gettime(u64 clock, u64 tp) {
    if (clock > 1) return -EINVAL;           /* CLOCK_REALTIME (0) and CLOCK_MONOTONIC (1): time since boot */
    if (!user_ok(tp, sizeof(struct timespec), true)) return -EFAULT;
    u64 ms = time_ms();
    *(struct timespec *)tp = (struct timespec){(i64)(ms / 1000), (i64)(ms % 1000) * 1000000};
    return 0;
}

void syscall_dispatch(struct syscall_frame *f) {
    i64 r;
    switch (f->rax) {
    case SYS_write:         r = sys_write(f->rdi, f->rsi, f->rdx); break;
    case SYS_sched_yield:   thread_yield(); r = 0; break;
    case SYS_nanosleep:     r = sys_nanosleep(f->rdi, f->rsi); break;
    case SYS_getpid:        r = (i64)thread_current()->process->pid; break;
    case SYS_gettid:        r = (i64)thread_current()->id; break;
    case SYS_clock_gettime: r = sys_clock_gettime(f->rdi, f->rsi); break;
    case SYS_exit:
    case SYS_exit_group:    process_exit((int)f->rdi);
    default:                r = -ENOSYS; break;
    }
    f->rax = (u64)r;
}
