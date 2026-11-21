/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Pipes: a 4 KiB ring buffer between a read end and a write end.
 * read() waits for data and returns 0 (end of file) once every write end is
 * closed; write() waits for room and fails with -EPIPE once every read end is
 * closed. Writes of up to 4 KiB are atomic.
 */
#include <opaline/errno.h>
#include <opaline/proc.h>
#include <opaline/mm.h>
#include <opaline/sched.h>
#include <opaline/spinlock.h>
#include <opaline/string.h>
#include <opaline/vfs.h>

#define PIPE_SIZE 4096

struct pipe {
    u8 buf[PIPE_SIZE];
    u32 head, len;              /* head: next byte to read */
    int readers, writers;
    struct waitqueue wait;      /* readers and writers both wait here */
};

static i64 pipe_read(struct vnode *v, u64 off, void *out, u64 n) {
    (void)off;
    struct pipe *p = v->priv;
    if (n == 0) return 0;
    u64 f = irq_save();
    while (p->len == 0 && p->writers > 0) {
        if (thread_current()->io_nonblock) {
            irq_restore(f);
            return -EAGAIN;
        }
        if (waitqueue_wait_intr(&p->wait) < 0) {   /* a signal arrived */
            irq_restore(f);
            return -EINTR;
        }
    }
    u64 got = MIN(n, (u64)p->len);
    for (u64 i = 0; i < got; i++) ((u8 *)out)[i] = p->buf[(p->head + i) % PIPE_SIZE];
    p->head = (u32)((p->head + got) % PIPE_SIZE);
    p->len -= (u32)got;
    irq_restore(f);
    if (got) {
        waitqueue_wake_all(&p->wait);
        vfs_poll_notify();
    }
    return (i64)got;
}

static i64 pipe_write(struct vnode *v, u64 off, const void *in, u64 n) {
    (void)off;
    struct pipe *p = v->priv;
    u64 done = 0;
    while (done < n) {
        u64 f = irq_save();
        u64 want = MIN(n - done, (u64)PIPE_SIZE);   /* small writes go in one piece */
        if (thread_current()->io_nonblock && p->readers > 0 && PIPE_SIZE - p->len < want) {
            /* O_NONBLOCK: a small write goes whole or not at all; a big one takes what fits */
            u64 room = PIPE_SIZE - p->len;
            if (n - done <= PIPE_SIZE || room == 0) {
                irq_restore(f);
                return done ? (i64)done : -EAGAIN;
            }
            want = room;
        }
        while (p->readers > 0 && PIPE_SIZE - p->len < want)
            if (waitqueue_wait_intr(&p->wait) < 0) {
                irq_restore(f);
                return done ? (i64)done : -EINTR;
            }
        if (p->readers == 0) {
            irq_restore(f);
            struct thread *t = thread_current();
            if (!done && t->process->user) signal_send_thread(t, SIGPIPE);   /* as POSIX asks */
            return done ? (i64)done : -EPIPE;
        }
        for (u64 i = 0; i < want; i++) p->buf[(p->head + p->len + i) % PIPE_SIZE] = ((const u8 *)in)[done + i];
        p->len += (u32)want;
        done += want;
        irq_restore(f);
        waitqueue_wake_all(&p->wait);
        vfs_poll_notify();
    }
    return (i64)done;
}

static int pipe_stat(struct vnode *v, struct vnode_stat *st) {
    memset(st, 0, sizeof *st);
    st->ino = v->ino;
    st->mode = 0x1000 | 0600;   /* S_IFIFO */
    st->links = 1;
    return 0;
}

static void pipe_close(struct vnode *v, u32 flags) {
    struct pipe *p = v->priv;
    u64 f = irq_save();
    if ((flags & O_ACCMODE) == O_RDONLY) p->readers--;
    else p->writers--;
    irq_restore(f);
    waitqueue_wake_all(&p->wait);
    vfs_poll_notify();
}

static u32 pipe_poll(struct vnode *v, u32 flags) {
    struct pipe *p = v->priv;
    if ((flags & O_ACCMODE) == O_RDONLY)
        return (p->len ? POLLIN : 0) | (p->writers == 0 ? POLLHUP : 0);   /* EOF reads as POLLHUP */
    return (p->readers == 0 ? POLLERR : 0) | (PIPE_SIZE - p->len >= 1 ? POLLOUT : 0);
}

static void pipe_release(struct vnode *v) {
    kfree(v->priv);
    kfree(v);
}

static const struct vnode_ops pipe_ops = {
    .read = pipe_read, .write = pipe_write, .stat = pipe_stat, .close = pipe_close, .release = pipe_release,
    .poll = pipe_poll};

int pipe_create(struct file **read_end, struct file **write_end) {
    static u64 next_ino = 1;
    struct pipe *p = kcalloc(1, sizeof *p);
    struct vnode *v = kcalloc(1, sizeof *v);
    if (!p || !v) {
        kfree(p);
        kfree(v);
        return -ENOMEM;
    }
    p->readers = p->writers = 1;
    v->type = VN_PIPE;
    v->refs = 2;   /* one per end */
    v->ino = next_ino++;
    v->ops = &pipe_ops;
    v->priv = p;
    *read_end = file_new(v, O_RDONLY);
    *write_end = file_new(v, O_WRONLY);
    if (!*read_end || !*write_end) return -ENOMEM;
    return 0;
}
