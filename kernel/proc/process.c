/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
#include <opaline/arch/cpu.h>
#include <opaline/arch/idt.h>
#include <opaline/arch/syscall.h>
#include <opaline/console.h>
#include <opaline/errno.h>
#include <opaline/mm.h>
#include <opaline/panic.h>
#include <opaline/proc.h>
#include <opaline/spinlock.h>
#include <opaline/string.h>
#include <opaline/vfs.h>

static u64 next_pid = 1;
static struct process *all_processes;
static spinlock_t list_lock = SPINLOCK_INIT;

/* First code of a user process's main thread: leave the kernel. */
static void user_thread_start(void *arg) {
    struct process *p = arg;
    jump_to_user(p->user_entry, p->user_stack);
}

/* Copies bytes into the process's memory through the HHDM (works before the process runs). */
static bool copy_to_process(u64 page_table, u64 vaddr, const void *src, u64 len) {
    while (len) {
        u64 phys = vmm_translate_in(page_table, vaddr, NULL);
        if (phys == VMM_NOT_MAPPED) return false;
        u64 n = MIN(len, PAGE_SIZE - vaddr % PAGE_SIZE);
        memcpy(phys_to_virt(phys), src, n);
        vaddr += n;
        src = (const u8 *)src + n;
        len -= n;
    }
    return true;
}

static bool map_user_stack(u64 page_table) {
    for (u64 i = 1; i <= USER_STACK_PAGES; i++) {
        u64 phys = pmm_alloc_page();
        if (!phys) return false;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        if (!vmm_map_in(page_table, USER_STACK_TOP - i * PAGE_SIZE, phys, VMM_USER | VMM_WRITE)) return false;
    }
    return true;   /* the page below the stack stays unmapped: overflowing it faults */
}

/*
 * The initial stack, as the System V ABI expects at _start:
 *   [rsp] argc, argv[0..argc-1], NULL, envp NULL, auxv {AT_NULL, 0}; the strings above them.
 */
static int build_stack(struct process *p, const char *const *argv) {
    u64 argc = 0, bytes = 0;
    while (argv && argv[argc]) {
        bytes += strlen(argv[argc]) + 1;
        if (++argc > ARGV_MAX || bytes > ARGV_BYTES_MAX) return -E2BIG;
    }
    u64 sp = USER_STACK_TOP - bytes;
    u64 ptrs[ARGV_MAX + 1];
    u64 at = sp;
    for (u64 i = 0; i < argc; i++) {
        u64 n = strlen(argv[i]) + 1;
        if (!copy_to_process(p->page_table, at, argv[i], n)) return -EFAULT;
        ptrs[i] = at;
        at += n;
    }
    ptrs[argc] = 0;
    u64 words = 1 + (argc + 1) + 1 + 2;
    sp = ALIGN_DOWN(sp - words * 8, 16);
    u64 w[ARGV_MAX + 6];
    u64 k = 0;
    w[k++] = argc;
    for (u64 i = 0; i <= argc; i++) w[k++] = ptrs[i];
    w[k++] = 0;   /* envp terminator */
    w[k++] = 0;   /* AT_NULL */
    w[k++] = 0;
    if (!copy_to_process(p->page_table, sp, w, k * 8)) return -EFAULT;
    p->user_stack = sp;
    return 0;
}

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

static void process_free(struct process *p) {
    list_remove(p);
    kfree(p);
}

/* Orphans are freed together with their thread (see thread->on_free). */
static void free_orphan(struct thread *t) { process_free(t->process); }

static void setup_files(struct process *p, struct process *parent) {
    if (parent) {
        for (int i = 0; i < PROCESS_MAX_FDS; i++)
            if ((p->fds[i] = parent->fds[i])) file_ref(p->fds[i]);
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
    }
    vnode_put(p->cwd);
    p->cwd = NULL;
}

struct process *process_spawn(const char *name, const void *elf, size_t size, const char *const *argv,
                              struct process *parent, int *error) {
    struct process *p = kcalloc(1, sizeof *p);
    if (!p) {
        *error = -ENOMEM;
        return NULL;
    }
    for (size_t i = 0; i < sizeof p->name - 1 && name[i]; i++) p->name[i] = name[i];
    p->user = true;
    p->parent = parent;
    p->page_table = vmm_create_address_space();
    const char *why = "out of memory";
    *error = -ENOMEM;
    if (!p->page_table) goto fail;
    if (!elf_load(p->page_table, elf, size, &p->user_entry, &why)) {
        *error = -ENOEXEC;
        goto fail;
    }
    if (!map_user_stack(p->page_table)) goto fail;
    *error = build_stack(p, argv);
    if (*error < 0) {
        why = "arguments too long";
        goto fail;
    }
    setup_files(p, parent);

    u64 f = irq_save();
    p->pid = next_pid++;
    irq_restore(f);
    list_add(p);
    p->main_thread = thread_create_in(p, p->name, user_thread_start, p);
    if (!p->main_thread) {
        list_remove(p);
        close_files(p);
        why = "out of memory";
        *error = -ENOMEM;
        goto fail;
    }
    *error = 0;
    return p;
fail:
    kprintf("Cannot start \"%s\": %s\n", name, why);
    if (p->page_table) vmm_destroy_address_space(p->page_table);
    kfree(p);
    return NULL;
}

struct process *process_spawn_path(const char *path, const char *const *argv, struct process *parent, int *error) {
    void *elf;
    u64 size;
    *error = vfs_read_file(path, parent ? parent->cwd : NULL, &elf, &size);
    if (*error < 0) return NULL;
    const char *name = path;
    for (const char *s = path; *s; s++)
        if (*s == '/' && s[1]) name = s + 1;
    struct process *p = process_spawn(name, elf, size, argv, parent, error);
    kfree(elf);
    return p;
}

int process_wait(struct process *p) {
    int code = thread_join(p->main_thread);
    process_free(p);
    return code;
}

i64 process_wait_child(struct process *parent, i64 pid, int *status, bool nohang) {
    struct process *child = NULL;
    bool any = false;
    u64 f = spin_lock(&list_lock);
    for (struct process *c = all_processes; c; c = c->all_next) {
        if (c->parent != parent || (pid > 0 && (i64)c->pid != pid)) continue;
        any = true;
        if (!child || c->main_thread->state == THREAD_DEAD) child = c;   /* prefer one that finished */
        if (c->main_thread->state == THREAD_DEAD) break;
    }
    spin_unlock(&list_lock, f);
    if (!any) return -ECHILD;
    if (nohang && child->main_thread->state != THREAD_DEAD) return 0;
    int code = thread_join(child->main_thread);
    /* wait status as in Linux: exit code in bits 8-15, or the signal number in bits 0-6 */
    *status = child->signal ? child->signal : (code & 0xFF) << 8;
    i64 r = (i64)child->pid;
    process_free(child);
    return r;
}

void process_exit(int code) {
    struct thread *t = thread_current();
    struct process *p = t->process;
    if (!p || !p->user) panic("process_exit called by a kernel thread");
    close_files(p);

    /* children nobody will wait for any more are freed when they end */
    u64 f = spin_lock(&list_lock);
    for (struct process *c = all_processes; c; c = c->all_next)
        if (c->parent == p && c->main_thread) {
            c->parent = NULL;
            c->orphan = true;
            c->main_thread->on_free = free_orphan;
            thread_detach(c->main_thread);
        }
    spin_unlock(&list_lock, f);

    /* leave the address space before freeing it; from now on this thread is kernel-only */
    u64 old = p->page_table;
    f = irq_save();
    p->page_table = vmm_kernel_page_table();
    write_cr3(p->page_table);
    irq_restore(f);
    vmm_destroy_address_space(old);
    thread_exit(code);
}

void process_fault(const struct interrupt_frame *f) {
    struct process *p = thread_current()->process;
    p->signal = f->vector == 6 ? SIGILL : f->vector == 0 || f->vector == 16 || f->vector == 19 ? SIGFPE : SIGSEGV;
    int code = 128 + p->signal;
    console_set_color(COLOR_WARNING);
    kprintf("Process \"%s\" (pid %lu) crashed: %s at RIP=%p", p->name, p->pid, exception_name(f->vector),
            (void *)f->rip);
    if (f->vector == 14) kprintf(", address %p", (void *)read_cr2());
    kprintf("\n  The process was stopped (exit code %d). The system continues.\n", code);
    console_set_color(COLOR_TEXT);
    process_exit(code);
}
