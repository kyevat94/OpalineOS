/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Futexes: the kernel half of every user-space lock (pthread mutexes,
 * condition variables, pthread_join). FUTEX_WAIT sleeps while a 32-bit word
 * still holds the expected value; FUTEX_WAKE wakes sleepers on that word.
 * Waiters are keyed by (address space, address); without FUTEX_PRIVATE_FLAG, an
 * address inside a shared mapping is keyed by (memory object, offset) instead, so
 * processes sharing that memory wait on the same futex.
 */
#include <opaline/errno.h>
#include <opaline/proc.h>
#include <opaline/spinlock.h>
#include <opaline/uvm.h>

#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_REQUEUE        3
#define FUTEX_CMP_REQUEUE    4
#define FUTEX_WAIT_BITSET    9
#define FUTEX_WAKE_BITSET   10
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256

struct waiter {
    void *mm;                /* key: address space or memory object */
    u64 addr;                /* key: address or offset */
    struct thread *t;
    bool woken;
    struct waiter *next;
};

static struct waiter *waiters;   /* changed with interrupts disabled */

static void unlink(struct waiter *w) {
    for (struct waiter **p = &waiters; *p; p = &(*p)->next)
        if (*p == w) {
            *p = w->next;
            return;
        }
}

static int wake_key(void *mm, u64 addr, int count) {
    int n = 0;
    u64 f = irq_save();
    for (struct waiter **p = &waiters; *p && n < count;) {
        struct waiter *w = *p;
        if (w->mm == mm && w->addr == addr) {
            *p = w->next;
            w->woken = true;
            thread_wake(w->t);
            n++;
        } else {
            p = &w->next;
        }
    }
    irq_restore(f);
    return n;
}

/* timeout: relative (FUTEX_WAIT) or absolute (FUTEX_WAIT_BITSET); NULL waits for ever. */
int futex_wake(struct mm *mm, u64 addr, int count) { return wake_key(mm, addr, count); }

/* uaddr: the user address (to read); key: where waiters are filed */
static i64 futex_wait(struct mm *mm, u64 uaddr, void *key, u64 addr, u32 val, u64 timeout, bool absolute) {
    u64 ms = 0;
    if (timeout) {
        if (!mm_user_access(mm, timeout, 16, false)) return -EFAULT;
        i64 sec = ((const i64 *)timeout)[0], nsec = ((const i64 *)timeout)[1];
        if (sec < 0 || nsec < 0 || nsec >= 1000000000) return -EINVAL;
        u64 want = (u64)sec * 1000 + ((u64)nsec + 999999) / 1000000;
        if (absolute) {
            u64 now = time_ms();   /* every clock counts from boot for now */
            if (want <= now) return -ETIMEDOUT;
            want -= now;
        } else if (want == 0) {
            if (sec == 0 && nsec == 0) {
                if (*(volatile u32 *)uaddr != val) return -EAGAIN;
                return -ETIMEDOUT;
            }
            want = 1;
        }
        ms = want;
    }
    struct waiter w = {key, addr, thread_current(), false, NULL};
    u64 f = irq_save();
    if (*(volatile u32 *)uaddr != val) {
        irq_restore(f);
        return -EAGAIN;
    }
    w.next = waiters;
    waiters = &w;
    int r = thread_block(ms, true);
    if (!w.woken) unlink(&w);
    irq_restore(f);
    return w.woken ? 0 : r;
}

static i64 futex_requeue(void *mm, u64 uaddr, u64 addr, int wake, int move, void *mm2, u64 addr2, bool cmp,
                         u32 val3) {
    u64 f = irq_save();
    if (cmp && *(volatile u32 *)uaddr != val3) {
        irq_restore(f);
        return -EAGAIN;
    }
    int n = 0, moved = 0;
    for (struct waiter **p = &waiters; *p;) {
        struct waiter *w = *p;
        if (w->mm != mm || w->addr != addr) {
            p = &w->next;
            continue;
        }
        if (n < wake) {
            *p = w->next;
            w->woken = true;
            thread_wake(w->t);
            n++;
            continue;
        }
        if (moved >= move) break;
        w->mm = mm2;
        w->addr = addr2;
        moved++;
        p = &w->next;
    }
    irq_restore(f);
    return n + (cmp ? moved : 0);
}

i64 sys_futex(u64 addr, u64 op, u64 val, u64 timeout, u64 addr2, u64 val3) {
    struct mm *mm = thread_current()->process->mm;
    if (addr % 4) return -EINVAL;
    if (!mm_user_access(mm, addr, 4, false)) return -EFAULT;
    bool private_op = op & FUTEX_PRIVATE_FLAG;
    int cmd = (int)(op & ~(u64)(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME));
    void *key;
    u64 off;
    mm_futex_key(mm, addr, private_op, &key, &off);
    int n = (int)MIN(val, 0x7FFFFFFFULL), n2 = (int)MIN(timeout, 0x7FFFFFFFULL);
    switch (cmd) {
    case FUTEX_WAIT: return futex_wait(mm, addr, key, off, (u32)val, timeout, false);
    case FUTEX_WAIT_BITSET:
        if (!(u32)val3) return -EINVAL;
        return futex_wait(mm, addr, key, off, (u32)val, timeout, true);   /* bitsets other than "all" act as "all" */
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET: return wake_key(key, off, n);
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE: {
        if (addr2 % 4 || !mm_user_access(mm, addr2, 4, false)) return -EFAULT;
        void *key2;
        u64 off2;
        mm_futex_key(mm, addr2, private_op, &key2, &off2);
        return futex_requeue(key, addr, off, n, n2, key2, off2, cmd == FUTEX_CMP_REQUEUE, (u32)val3);
    }
    default: return -ENOSYS;
    }
}
