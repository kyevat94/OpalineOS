/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Processes and user threads: starting programs, execve, clone/fork/vfork,
 * exit and wait.
 *
 * Lifetime: a process is referenced by each of its threads until the thread is
 * freed, and once more until its exit status is collected (wait4 by the
 * parent, process_wait for processes the kernel started, or nobody for
 * orphans). When the last thread exits, the process releases its memory and
 * files at once and becomes a zombie; the struct goes with the last reference.
 */
#include <opaline/arch/cpu.h>
#include <opaline/arch/idt.h>
#include <opaline/arch/syscall.h>
#include <opaline/console.h>
#include <opaline/cred.h>
#include <opaline/errno.h>
#include <opaline/mm.h>
#include <opaline/panic.h>
#include <opaline/proc.h>
#include <opaline/spinlock.h>
#include <opaline/string.h>
#include <opaline/uvm.h>
#include <opaline/vfs.h>

#define CLONE_VM             0x00000100
#define CLONE_SIGHAND        0x00000800
#define CLONE_VFORK          0x00004000
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

static u64 next_id = 1;                    /* process and thread ids */
static struct process *all_processes;
static spinlock_t list_lock = SPINLOCK_INIT;
static struct waitqueue child_exit_wq;     /* woken whenever a process ends */
static struct waitqueue vfork_wq;          /* woken when a vfork child exec'd or ended */
static struct waitqueue dethread_wq;       /* execve waits here for the other threads to end */

static const char *const default_env[] = {"HOME=/home", "PATH=/apps:/system/bin", "LANG=C.UTF-8", NULL};

static u64 new_id(void) {
    u64 f = irq_save();
    u64 id = next_id++;
    irq_restore(f);
    return id;
}

static const char *base_name(const char *path) {
    const char *name = path;
    for (const char *s = path; *s; s++)
        if (*s == '/' && s[1]) name = s + 1;
    return name;
}

static void set_name(char *dst, u64 size, const char *src) {
    u64 i = 0;
    for (; i < size - 1 && src[i] && src[i] != '/'; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* ---- the process list and references ------------------------------------------------- */

static void list_add(struct process *p) {
    u64 f = spin_lock(&list_lock);
    p->all_next = all_processes;
    all_processes = p;
    spin_unlock(&list_lock, f);
}

static void list_remove(struct process *p) {
    u64 f = spin_lock(&list_lock);
    for (struct process **q = &all_processes; *q; q = &(*q)->all_next)
        if (*q == p) {
            *q = p->all_next;
            break;
        }
    spin_unlock(&list_lock, f);
}

static void process_put(struct process *p) {
    if (__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    list_remove(p);
    kfree(p);
}

struct process *process_by_pid(u64 pid) {
    struct process *found = NULL;
    u64 f = spin_lock(&list_lock);
    for (struct process *p = all_processes; p; p = p->all_next)
        if (p->pid == pid && p->state == PROCESS_ALIVE) {
            found = p;
            break;
        }
    spin_unlock(&list_lock, f);
    return found;
}

struct thread *thread_by_tid(u64 tid) {
    struct thread *found = NULL;
    u64 f = spin_lock(&list_lock);
    for (struct process *p = all_processes; p && !found; p = p->all_next)
        for (struct thread *t = p->threads; t; t = t->proc_next)
            if (t->tid == tid) {
                found = t;
                break;
            }
    spin_unlock(&list_lock, f);
    return found;
}

/* ---- files --------------------------------------------------------------------------- */

static void setup_files(struct process *p, struct process *parent) {
    if (parent) {
        for (int i = 0; i < PROCESS_MAX_FDS; i++)
            if ((p->fds[i] = parent->fds[i])) {
                file_ref(p->fds[i]);
                p->fd_flags[i] = parent->fd_flags[i];
            }
        p->cwd = parent->cwd;
        memcpy(p->cwd_path, parent->cwd_path, sizeof p->cwd_path);
    } else {
        struct vnode *tty = tty_vnode();
        for (int i = 0; i < 3; i++) {
            vnode_ref(tty);
            p->fds[i] = file_new(tty, O_RDWR);
        }
        p->cwd = vfs_root();
        p->cwd_path[0] = '/';
        p->cwd_path[1] = 0;
    }
    if (p->cwd) vnode_ref(p->cwd);
}

static void close_files(struct process *p) {
    for (int i = 0; i < PROCESS_MAX_FDS; i++) {
        file_put(p->fds[i]);
        p->fds[i] = NULL;
        p->fd_flags[i] = 0;
    }
    vnode_put(p->cwd);
    p->cwd = NULL;
}

static struct process *process_new(const char *name, struct process *parent) {
    struct process *p = kcalloc(1, sizeof *p);
    if (!p) return NULL;
    set_name(p->name, sizeof p->name, name);
    p->user = true;
    p->parent = parent;
    p->refs = 1;   /* until the exit status is collected */
    p->state = PROCESS_ALIVE;
    p->pid = new_id();
    if (parent) {
        p->cred = parent->cred;
        p->umask = parent->umask;
        p->pgid = parent->pgid;
        p->sid = parent->sid;
        p->ctty = parent->ctty;
    } else {
        p->umask = 022;   /* root, as the kernel itself */
        /* started by the kernel: leads a new session that owns the terminal, in the foreground */
        p->pgid = p->sid = p->pid;
        p->ctty = true;   /* process_spawn_path gives it the terminal once the program loaded */
    }
    setup_files(p, parent);
    return p;
}

/* ---- user threads -------------------------------------------------------------------- */

/* First code of a user thread: enter user mode with the registers it was created with. */
static void user_thread_entry(void *arg) {
    struct interrupt_frame f = *(struct interrupt_frame *)arg;
    kfree(arg);
    return_to_user(&f);
}

static void user_thread_free(struct thread *t) { process_put(t->process); }

struct thread_setup {
    u64 tid, fs_base, sigmask, clear_child_tid;
};

/*
 * Creates a thread of p that starts in user mode with the registers in f: 0, -ENOMEM, or -EAGAIN
 * while p is ending or another thread's execve ends the others (it would not know of this one).
 */
static int new_user_thread(struct process *p, const struct interrupt_frame *f, const struct thread_setup *s,
                           struct thread **out) {
    struct interrupt_frame *copy = kmalloc(sizeof *copy);
    if (!copy) return -ENOMEM;
    *copy = *f;
    u64 flags = irq_save();   /* everything is set before the thread can run (execve's checks included) */
    struct thread *t = p->exiting || p->exec_thread ? NULL : thread_create_in(p, p->name, user_thread_entry, copy);
    if (!t) {
        int err = p->exiting || p->exec_thread ? -EAGAIN : -ENOMEM;
        irq_restore(flags);
        kfree(copy);
        return err;
    }
    t->joinable = false;      /* the idle thread frees it once it ended */
    t->on_free = user_thread_free;
    t->tid = s->tid;
    t->fs_base = s->fs_base;
    t->sigmask = s->sigmask;
    t->clear_child_tid = s->clear_child_tid;
    u64 lf = spin_lock(&list_lock);
    t->proc_next = p->threads;
    p->threads = t;
    spin_unlock(&list_lock, lf);
    p->live_threads++;
    p->refs++;
    irq_restore(flags);
    *out = t;
    return 0;
}

/* ---- loading a program --------------------------------------------------------------- */

static u64 count_strings(const char *const *v, u64 *bytes) {
    u64 n = 0;
    while (v && v[n]) *bytes += strlen(v[n++]) + 1;
    return n;
}

enum {
    AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6, AT_BASE = 7, AT_FLAGS = 8, AT_ENTRY = 9,
    AT_UID = 11, AT_EUID = 12, AT_GID = 13, AT_EGID = 14, AT_PLATFORM = 15, AT_HWCAP = 16, AT_CLKTCK = 17,
    AT_SECURE = 23, AT_RANDOM = 25, AT_EXECFN = 31,
};
#define AUX_COUNT 18

/*
 * The initial stack, as the System V ABI expects at _start:
 *   [rsp] argc, argv[0..argc-1], NULL, envp[...], NULL, auxv pairs, AT_NULL;
 *   the strings and AT_RANDOM's 16 bytes above them, up to USER_STACK_TOP.
 */
static int build_stack(struct mm *mm, const char *path, const char *const *argv, const char *const *envp,
                       const struct elf_info *info, u64 interp_base, const struct cred *c, u64 *sp_out) {
    static const char platform[] = "x86_64";
    u64 bytes = strlen(path) + 1 + sizeof platform + 16;
    u64 argc = count_strings(argv, &bytes), envc = count_strings(envp, &bytes);
    if (argc + envc > ARGV_MAX || bytes > ARGS_BYTES_MAX) return -E2BIG;

    u64 strings_size = ALIGN_UP(bytes, 16);
    u64 str_base = USER_STACK_TOP - strings_size;
    u64 nwords = 1 + (argc + 1) + (envc + 1) + 2 * AUX_COUNT;
    u64 sp = ALIGN_DOWN(str_base - nwords * 8, 16);
    char *blob = kcalloc(1, strings_size);
    u64 *w = kmalloc(nwords * 8);
    if (!blob || !w) {
        kfree(blob);
        kfree(w);
        return -ENOMEM;
    }
    u64 at = 0;
    random_bytes(blob, 16);   /* AT_RANDOM first: it is 16-byte aligned there */
    u64 random_addr = str_base;
    at = 16;
#define PUT(s)                                         \
    ({                                                 \
        u64 n_ = strlen(s) + 1;                        \
        memcpy(blob + at, (s), n_);                    \
        u64 a_ = str_base + at;                        \
        at += n_;                                      \
        a_;                                            \
    })
    u64 execfn = PUT(path), plat = PUT(platform);
    u64 k = 0;
    w[k++] = argc;
    for (u64 i = 0; i < argc; i++) w[k++] = PUT(argv[i]);
    w[k++] = 0;
    for (u64 i = 0; i < envc; i++) w[k++] = PUT(envp[i]);
    w[k++] = 0;
#undef PUT
    u32 ca, cb, cc, cd;
    cpuid(1, 0, &ca, &cb, &cc, &cd);
    const u64 aux[AUX_COUNT][2] = {
        {AT_PHDR, info->phdr}, {AT_PHENT, 56}, {AT_PHNUM, info->phnum}, {AT_PAGESZ, PAGE_SIZE},
        {AT_BASE, interp_base}, {AT_FLAGS, 0}, {AT_ENTRY, info->entry}, {AT_UID, c->uid}, {AT_EUID, c->euid},
        {AT_GID, c->gid}, {AT_EGID, c->egid}, {AT_SECURE, c->euid != c->uid || c->egid != c->gid}, {AT_RANDOM, random_addr}, {AT_EXECFN, execfn}, {AT_PLATFORM, plat},
        {AT_HWCAP, cd}, {AT_CLKTCK, 100}, {AT_NULL, 0},
    };
    for (u64 i = 0; i < AUX_COUNT; i++) {
        w[k++] = aux[i][0];
        w[k++] = aux[i][1];
    }
    bool ok = mm_write(mm, str_base, blob, strings_size) && mm_write(mm, sp, w, nwords * 8);
    kfree(blob);
    kfree(w);
    if (!ok) return -ENOMEM;
    *sp_out = sp;
    return 0;
}

/*
 * Loads the program at `path` (and its interpreter) into the empty address space mm.
 * `cred` comes in as the caller's and goes out as the program's: a set-user-id or
 * set-group-id file runs with its owner's (or group's) effective id.
 */
static int exec_image(struct mm *mm, const char *path, struct vnode *cwd, const char *const *argv,
                      const char *const *envp, struct cred *cred, u64 *entry, u64 *sp) {
    /* one lookup: the file checked is the file loaded, even if the path changes meanwhile */
    struct vnode *v;
    int err = vfs_lookup(path, cwd, &v);
    if (err < 0) return err;
    struct vnode_stat st;
    err = v->ops->stat(v, &st);
    if (err == 0 && (st.mode & S_IFMT) != S_IFREG) err = -EACCES;
    if (err == 0) err = vfs_permission(v, MAY_EXEC);
    void *elf;
    u64 size;
    if (err == 0) err = vfs_read_vnode(v, &elf, &size);
    vnode_put(v);
    if (err < 0) return err;
    if (st.mode & S_ISUID) cred->euid = st.uid;
    if (st.mode & S_ISGID) cred->egid = st.gid;
    cred->suid = cred->euid;
    cred->sgid = cred->egid;
    struct elf_info info;
    const char *why;
    bool ok = elf_load(mm, elf, size, PIE_BASE, &info, &why);
    kfree(elf);
    if (!ok) {
        kprintf("Cannot start \"%s\": %s\n", path, why);
        return -ENOEXEC;
    }
    *entry = info.entry;
    u64 interp_base = 0;
    if (info.interp[0]) {
        err = vfs_read_file(info.interp, NULL, &elf, &size);
        if (err < 0) {
            kprintf("Cannot start \"%s\": interpreter %s not found\n", path, info.interp);
            return err;
        }
        struct elf_info ii;
        ok = elf_load(mm, elf, size, INTERP_BASE, &ii, &why);
        kfree(elf);
        if (ok && (!ii.base || ii.interp[0])) {
            ok = false;
            why = "the interpreter is not a shared object";
        }
        if (!ok) {
            kprintf("Cannot start \"%s\": %s: %s\n", path, info.interp, why);
            return -ENOEXEC;
        }
        *entry = ii.entry;
        interp_base = ii.base;
    }
    mm->brk_start = mm->brk = ALIGN_UP(info.end, PAGE_SIZE);
    i64 r = mm_map(mm, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, PROT_READ | PROT_WRITE,
                   MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, NULL, 0, true);
    if (r < 0) return (int)r;
    return build_stack(mm, path, argv, envp ? envp : default_env, &info, interp_base, cred, sp);
}

static void user_frame(struct interrupt_frame *f, u64 entry, u64 sp) {
    memset(f, 0, sizeof *f);
    f->rip = entry;
    f->rsp = sp;
    f->cs = 0x23;
    f->ss = 0x1B;
    f->rflags = 0x202;   /* interrupts on */
}

struct process *process_spawn_path(const char *path, const char *const *argv, struct process *parent, int flags,
                                   int *error) {
    struct process *p = process_new(base_name(path), parent);
    if (p && (flags & SPAWN_FOREGROUND)) p->pgid = p->pid;   /* a job of its own (a shell's command) */
    struct mm *mm = p ? mm_create() : NULL;
    if (!mm) {
        if (p) {
            close_files(p);
            kfree(p);
        }
        *error = -ENOMEM;
        return NULL;
    }
    u64 entry, sp;
    *error = exec_image(mm, path, parent ? parent->cwd : NULL, argv, NULL, &p->cred, &entry, &sp);
    if (*error < 0) {
        mm_put(mm);
        close_files(p);
        kfree(p);
        return NULL;
    }
    p->mm = mm;
    p->page_table = mm->page_table;
    struct interrupt_frame f;
    user_frame(&f, entry, sp);
    list_add(p);
    /* the terminal's foreground: a new session from the kernel, or a shell's job (only now that the
       program exists: a failed start must not take the terminal away from the shell) */
    if (!parent || ((flags & SPAWN_FOREGROUND) && p->ctty && p->sid == tty_session()))
        tty_set_foreground(p->sid, p->pgid);
    struct thread_setup s = {.tid = p->pid};
    if (new_user_thread(p, &f, &s, &p->main_thread) < 0) {
        list_remove(p);
        mm_put(mm);
        close_files(p);
        kfree(p);
        *error = -ENOMEM;
        return NULL;
    }
    return p;
}

int process_execve(struct interrupt_frame *f, const char *path, const char *const *argv, const char *const *envp) {
    struct thread *t = thread_current();
    struct process *p = t->process;
    if (p->exec_thread) return -EAGAIN;   /* another thread is already in execve */
    struct mm *mm = mm_create();
    if (!mm) return -ENOMEM;
    u64 entry, sp;
    struct cred cred = p->cred;
    int err = exec_image(mm, path, p->cwd, argv, envp, &cred, &entry, &sp);
    if (err < 0) {
        mm_put(mm);
        return err;
    }
    /* the new program is loaded: the other threads end now (as on Linux), then this one carries on */
    u64 lf = spin_lock(&list_lock);
    p->exec_thread = t;
    for (struct thread *o = p->threads; o; o = o->proc_next)
        if (o != t) {
            o->exit_pending = true;   /* user_return ends it */
            thread_interrupt(o);
        }
    spin_unlock(&list_lock, lf);
    u64 fl = irq_save();
    while (p->live_threads > 1) waitqueue_wait(&dethread_wq);
    p->exec_thread = NULL;
    irq_restore(fl);
    if (p->exiting) {   /* exit_group or a fatal signal meanwhile: this thread ends too */
        mm_put(mm);
        return -EINTR;
    }
    t->tid = p->pid;   /* the thread that remains is the main thread */
    p->main_thread = t;
    p->cred = cred;

    /* no way back from here: switch to the new program */
    struct mm *old = p->mm;
    fl = irq_save();
    p->mm = mm;
    p->page_table = mm->page_table;
    write_cr3(mm->page_table);
    irq_restore(fl);
    mm_put(old);   /* a vfork parent may still use it */

    set_name(p->name, sizeof p->name, base_name(path));
    set_name(t->name, sizeof t->name, base_name(path));
    for (int i = 0; i < PROCESS_MAX_FDS; i++)
        if (p->fds[i] && (p->fd_flags[i] & 1)) {   /* FD_CLOEXEC */
            file_put(p->fds[i]);
            p->fds[i] = NULL;
            p->fd_flags[i] = 0;
        }
    signals_exec_reset(p);
    t->fs_base = 0;
    wrmsr(MSR_FS_BASE, 0);
    t->clear_child_tid = 0;
    t->altstack_sp = t->altstack_size = 0;
    t->altstack_flags = 2;   /* SS_DISABLE */
    thread_reset_fpu();

    fl = irq_save();
    p->vfork_done = true;
    irq_restore(fl);
    waitqueue_wake_all(&vfork_wq);

    user_frame(f, entry, sp);
    t->iret_return = true;
    return 0;
}

/* ---- clone, fork, vfork --------------------------------------------------------------- */

static bool put_user_u32(u64 addr, u32 value) {
    struct mm *mm = thread_current()->process->mm;
    if (!mm_user_access(mm, addr, 4, true)) return false;
    *(volatile u32 *)addr = value;
    return true;
}

i64 process_clone(struct interrupt_frame *f, u64 flags, u64 stack, u64 parent_tid, u64 child_tid, u64 tls) {
    struct thread *t = thread_current();
    struct process *p = t->process;
    struct interrupt_frame child = *f;
    child.rax = 0;   /* the child sees clone() return 0 */
    if (stack) child.rsp = stack;
    struct thread_setup s = {
        .fs_base = flags & CLONE_SETTLS ? tls : t->fs_base,
        .sigmask = t->sigmask,
        .clear_child_tid = flags & CLONE_CHILD_CLEARTID ? child_tid : 0,
    };
    if ((flags & CLONE_SETTLS) && tls >= USER_TOP) return -EPERM;

    if (flags & CLONE_THREAD) {   /* a new thread in this process (pthread_create) */
        if (!(flags & CLONE_VM) || !(flags & CLONE_SIGHAND)) return -EINVAL;
        if (p->exiting) return -EAGAIN;
        s.tid = new_id();
        if ((flags & CLONE_PARENT_SETTID) && !put_user_u32(parent_tid, (u32)s.tid)) return -EFAULT;
        if ((flags & CLONE_CHILD_SETTID) && !put_user_u32(child_tid, (u32)s.tid)) return -EFAULT;
        struct thread *nt;
        int err = new_user_thread(p, &child, &s, &nt);
        return err < 0 ? err : (i64)s.tid;
    }

    /* a new process: fork (copied memory) or vfork/posix_spawn (CLONE_VM: shared memory) */
    struct process *c = process_new(p->name, p);
    if (!c) return -ENOMEM;
    memcpy(c->sigactions, p->sigactions, sizeof c->sigactions);
    if (flags & CLONE_VM) {
        mm_get(p->mm);
        c->mm = p->mm;
    } else {
        c->mm = mm_fork(p->mm);
    }
    if (!c->mm) {
        close_files(c);
        kfree(c);
        return -ENOMEM;
    }
    c->page_table = c->mm->page_table;
    s.tid = c->pid;
    if ((flags & CLONE_PARENT_SETTID) && !put_user_u32(parent_tid, (u32)s.tid)) {
        mm_put(c->mm);
        close_files(c);
        kfree(c);
        return -EFAULT;
    }
    if (flags & CLONE_CHILD_SETTID) {
        u32 v = (u32)s.tid;
        mm_write(c->mm, child_tid, &v, 4);
    }
    list_add(c);
    if (flags & CLONE_VFORK) c->refs++;   /* kept until the wait below ends */
    if (new_user_thread(c, &child, &s, &c->main_thread) < 0) {
        list_remove(c);
        mm_put(c->mm);
        close_files(c);
        kfree(c);
        return -ENOMEM;
    }
    c->main_thread->altstack_sp = t->altstack_sp;
    c->main_thread->altstack_size = t->altstack_size;
    c->main_thread->altstack_flags = t->altstack_flags;
    i64 pid = (i64)c->pid;
    if (flags & CLONE_VFORK) {   /* the parent sleeps until the child exec'd or ended */
        u64 fl = irq_save();
        while (!c->vfork_done) waitqueue_wait(&vfork_wq);
        irq_restore(fl);
        process_put(c);
    }
    return pid;
}

/* ---- exit and wait --------------------------------------------------------------------- */

void process_kill(struct process *p, int sig) {
    u64 f = spin_lock(&list_lock);
    if (!p->exiting) {
        p->exiting = true;
        p->signal = sig;
    }
    for (struct thread *t = p->threads; t; t = t->proc_next) thread_interrupt(t);
    spin_unlock(&list_lock, f);
    waitqueue_wake_all(&p->stop_wq);   /* a stopped process must also end */
}

/* The last thread of p ended: release everything but the struct, become a zombie. */
static NORETURN void process_finish(struct process *p, int code) {
    if (!p->exiting) p->exit_code = code;
    close_files(p);

    /* children nobody will wait for any more are freed when they end */
    struct process *collect = NULL;
    u64 f = spin_lock(&list_lock);
    for (struct process *c = all_processes; c; c = c->all_next)
        if (c->parent == p) {
            c->parent = NULL;
            c->orphan = true;
            if (c->state == PROCESS_ZOMBIE) {   /* already ended: nobody collects it, so drop that reference */
                c->reap_next = collect;
                collect = c;
            }
        }
    spin_unlock(&list_lock, f);
    while (collect) {
        struct process *c = collect;
        collect = c->reap_next;
        process_put(c);
    }

    /* leave the address space before releasing it; from now on this thread is kernel-only */
    struct mm *mm = p->mm;
    f = irq_save();
    p->mm = NULL;
    p->page_table = vmm_kernel_page_table();
    write_cr3(p->page_table);
    irq_restore(f);
    mm_put(mm);

    irq_save();   /* until thread_exit: the waiter must not run before this thread is dead */
    u64 lf = spin_lock(&list_lock);
    p->state = PROCESS_ZOMBIE;
    p->vfork_done = true;
    struct process *parent = p->parent;
    if (parent && (parent->sigactions[SIGCHLD].handler == 1 /* SIG_IGN */ ||
                   (parent->sigactions[SIGCHLD].flags & 2 /* SA_NOCLDWAIT */))) {
        p->parent = NULL;   /* POSIX: the parent will not wait, so nobody collects it */
        p->orphan = true;
        parent = NULL;
    }
    bool drop = p->orphan;
    spin_unlock(&list_lock, lf);
    if (parent) signal_send_process(parent, SIGCHLD);
    waitqueue_wake_all(&child_exit_wq);
    waitqueue_wake_all(&vfork_wq);
    if (drop) process_put(p);   /* this thread's reference keeps it until the thread is freed */
    thread_exit(code);
}

void thread_exit_user(int code) {
    struct thread *t = thread_current();
    struct process *p = t->process;
    if (!p || !p->user) panic("thread_exit_user called by a kernel thread");
    if (t->clear_child_tid && p->mm && mm_user_access(p->mm, t->clear_child_tid, 4, true)) {
        *(volatile u32 *)t->clear_child_tid = 0;   /* pthread_join waits for this */
        futex_wake(p->mm, t->clear_child_tid, 1);
    }
    u64 f = spin_lock(&list_lock);
    for (struct thread **q = &p->threads; *q; q = &(*q)->proc_next)
        if (*q == t) {
            *q = t->proc_next;
            break;
        }
    bool last = --p->live_threads == 0;
    spin_unlock(&list_lock, f);
    if (last) process_finish(p, code);
    if (p->exec_thread) waitqueue_wake_all(&dethread_wq);   /* execve waits for the others to end */
    thread_exit(code);
}

void process_exit(int code) {
    struct process *p = thread_current()->process;
    u64 f = spin_lock(&list_lock);
    if (!p->exiting) {
        p->exiting = true;
        p->exit_code = code;
    }
    for (struct thread *t = p->threads; t; t = t->proc_next)
        if (t != thread_current()) thread_interrupt(t);
    spin_unlock(&list_lock, f);
    thread_exit_user(code);
}

static int wait_status(const struct process *c) {
    /* as in Linux: exit code in bits 8-15, or the signal number in bits 0-6 */
    return c->signal ? c->signal : (c->exit_code & 0xFF) << 8;
}

int process_wait(struct process *p) {
    u64 f = irq_save();
    while (p->state != PROCESS_ZOMBIE) waitqueue_wait(&child_exit_wq);
    irq_restore(f);
    int code = p->signal ? 128 + p->signal : p->exit_code;
    process_put(p);
    sched_free_dead();   /* its threads are dead: free their stacks now */
    return code;
}

static bool wait_matches(const struct process *c, const struct process *parent, i64 pid) {
    if (c->parent != parent) return false;
    if (pid > 0) return (i64)c->pid == pid;
    if (pid == 0) return c->pgid == parent->pgid;
    if (pid < -1) return (i64)c->pgid == -pid;
    return true;
}

i64 process_wait_child(struct process *parent, i64 pid, int *status, u64 options) {
    u64 f = irq_save();
    struct process *found;
    int report = 0;
    for (;;) {
        bool any = false;
        found = NULL;
        u64 lf = spin_lock(&list_lock);
        for (struct process *c = all_processes; c; c = c->all_next) {
            if (!wait_matches(c, parent, pid)) continue;
            any = true;
            if (c->state == PROCESS_ZOMBIE) {
                found = c;
                c->parent = NULL;   /* collected: no other wait finds it */
                break;
            }
            if ((c->report == 1 && (options & 2)) || (c->report == 2 && (options & 8))) {
                found = c;          /* stopped (WUNTRACED) or continued (WCONTINUED): told once */
                report = c->report;
                *status = report == 1 ? 0x7F | (c->stop_sig << 8) : 0xFFFF;
                c->report = 0;
                break;
            }
        }
        spin_unlock(&list_lock, lf);
        if (found) break;
        if (!any) {
            irq_restore(f);
            return -ECHILD;
        }
        if (options & 1) {   /* WNOHANG */
            irq_restore(f);
            return 0;
        }
        if (waitqueue_wait_intr(&child_exit_wq) < 0) {
            irq_restore(f);
            return -EINTR;
        }
    }
    irq_restore(f);
    i64 r = (i64)found->pid;
    if (report) return r;
    *status = wait_status(found);
    process_put(found);
    sched_free_dead();
    return r;
}

void process_notify_parent(struct process *p) {
    u64 lf = spin_lock(&list_lock);
    struct process *parent = p->parent;
    bool quiet = parent && (parent->sigactions[SIGCHLD].flags & 1);   /* SA_NOCLDSTOP */
    spin_unlock(&list_lock, lf);
    waitqueue_wake_all(&child_exit_wq);
    if (parent && !quiet) signal_send_process(parent, SIGCHLD);
}

/* ---- process groups and sessions ------------------------------------------------------- */

bool process_group_exists(u64 pgid, u64 sid) {
    bool found = false;
    u64 f = spin_lock(&list_lock);
    for (struct process *p = all_processes; p && !found; p = p->all_next)
        found = p->state == PROCESS_ALIVE && p->pgid == pgid && p->sid == sid;
    spin_unlock(&list_lock, f);
    return found;
}

i64 process_setpgid(u64 pid, u64 pgid) {
    struct process *me = thread_current()->process;
    struct process *t = pid == 0 || pid == me->pid ? me : process_by_pid(pid);
    if (!t || (t != me && t->parent != me)) return -ESRCH;
    if (t->sid != me->sid || t->sid == t->pid) return -EPERM;   /* another session, or a session leader */
    if ((i64)pgid < 0) return -EINVAL;
    if (pgid == 0) pgid = t->pid;
    if (pgid != t->pid && !process_group_exists(pgid, me->sid)) return -EPERM;
    t->pgid = pgid;
    return 0;
}

i64 process_getpgid(u64 pid) {
    struct process *t = pid == 0 ? thread_current()->process : process_by_pid(pid);
    return t ? (i64)t->pgid : -ESRCH;
}

i64 process_getsid(u64 pid) {
    struct process *t = pid == 0 ? thread_current()->process : process_by_pid(pid);
    return t ? (i64)t->sid : -ESRCH;
}

i64 process_setsid(void) {
    struct process *me = thread_current()->process;
    if (me->pgid == me->pid || process_group_exists(me->pid, me->sid)) return -EPERM;   /* already leads a group */
    me->sid = me->pgid = me->pid;
    me->ctty = false;   /* a new session has no terminal until it opens one (TIOCSCTTY) */
    return (i64)me->sid;
}

int process_signal_group(u64 pgid, int sig, bool check) {
    /* references first: sending may take locks that the list lock must not hold */
    struct process *targets[64];
    int n = 0, found = 0, sent = 0;
    const struct cred *me = cred_current();
    u64 f = spin_lock(&list_lock);
    for (struct process *p = all_processes; p; p = p->all_next) {
        if (p->state != PROCESS_ALIVE || p->pgid != pgid || !p->user) continue;
        found++;
        if (check && !cred_may_signal(me, &p->cred)) continue;
        if (n < 64) {
            p->refs++;
            targets[n++] = p;
        }
    }
    spin_unlock(&list_lock, f);
    for (int i = 0; i < n; i++) {
        if (sig) signal_send_process(targets[i], sig);
        sent++;
        process_put(targets[i]);
    }
    return !found ? -ESRCH : !sent ? -EPERM : 0;
}
