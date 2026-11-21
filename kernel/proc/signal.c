/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * POSIX signals with the Linux x86-64 interface.
 *
 * A signal is pending on a thread (tkill, CPU exceptions) or on the process
 * (kill). It is acted on when the thread next returns to user mode
 * (user_return): ignored, the default action (end the process, or stop it for
 * SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU until SIGCONT), or a handler call. A system
 * call that a signal interrupted without running a handler starts again. For a handler the kernel pushes a
 * struct rt_sigframe on the user stack, the same layout as Linux: the saved
 * registers (ucontext), the siginfo and the FPU state. The handler returns to
 * sa_restorer, which calls rt_sigreturn to restore them.
 */
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
#include <opaline/uvm.h>

#define SIG_DFL 0
#define SIG_IGN 1
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER  0x04000000
#define SS_ONSTACK 1
#define SS_DISABLE 2
#define MINSIGSTKSZ 2048

#define BIT(sig) (1ULL << ((sig) - 1))
#define UNBLOCKABLE (BIT(SIGKILL) | BIT(SIGSTOP))

struct sigcontext_k {   /* struct sigcontext (mcontext_t gregs on musl) */
    u64 r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    u16 cs, gs, fs, ss;
    u64 err, trapno, oldmask, cr2, fpstate;
    u64 reserved[8];
};

struct ucontext_k {
    u64 uc_flags, uc_link;
    u64 ss_sp;
    i32 ss_flags, pad;
    u64 ss_size;
    struct sigcontext_k mc;
    u64 uc_sigmask;
};

struct siginfo_k {
    i32 si_signo, si_errno, si_code, pad;
    u64 fields[14];   /* si_pid/si_uid or si_addr... (128 bytes in all) */
};

struct rt_sigframe {
    u64 pretcode;     /* the handler's return address: sa_restorer */
    struct ucontext_k uc;
    struct siginfo_k info;
};

static struct thread *self_thread(void) { return thread_current(); }

static bool default_ignored(int sig) { return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT; }

static bool is_stop(int sig) { return sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU; }

#define STOP_BITS (BIT(SIGSTOP) | BIT(SIGTSTP) | BIT(SIGTTIN) | BIT(SIGTTOU))

static bool ignored(struct process *p, int sig) {
    u64 h = p->sigactions[sig].handler;
    return sig != SIGKILL && (h == SIG_IGN || (h == SIG_DFL && default_ignored(sig)));
}

/* ---- sending ------------------------------------------------------------------------------ */

/* Every thread of p waits at its next return to user mode (SIGSTOP and friends). */
static void stop_process(struct process *p, int sig) {
    u64 f = irq_save();
    bool now = !p->stopped && !p->exiting;
    if (now) {
        p->stopped = true;
        p->stop_sig = sig;
        p->report = 1;
        for (struct thread *t = p->threads; t; t = t->proc_next) {
            t->restart_syscall = true;   /* its call starts again, even if SIGCONT comes before it stops */
            thread_interrupt(t);
        }
    }
    irq_restore(f);
    if (now) process_notify_parent(p);
}

static void continue_process(struct process *p) {
    u64 f = irq_save();
    bool was = p->stopped;
    p->stopped = false;
    if (was) p->report = 2;
    irq_restore(f);
    if (!was) return;
    waitqueue_wake_all(&p->stop_wq);
    process_notify_parent(p);
}

static void clear_pending(struct process *p, u64 bits) {
    u64 f = irq_save();
    p->sigpending &= ~bits;
    for (struct thread *t = p->threads; t; t = t->proc_next) t->sigpending &= ~bits;
    irq_restore(f);
}

/* to: a thread, or NULL for the whole process (any thread that does not block it takes it). */
static int send(struct process *p, struct thread *to, int sig) {
    if (sig < 1 || sig >= NSIG) return -EINVAL;
    if (p->state != PROCESS_ALIVE) return 0;
    if (sig == SIGKILL) {
        process_kill(p, SIGKILL);
        return 0;
    }
    if (sig == SIGCONT) {   /* continues the process whatever its action is */
        clear_pending(p, STOP_BITS);
        continue_process(p);
    }
    if (is_stop(sig)) clear_pending(p, BIT(SIGCONT));
    if (sig == SIGSTOP) {   /* cannot be caught or blocked */
        stop_process(p, SIGSTOP);
        return 0;
    }
    if (is_stop(sig) && p->sigactions[sig].handler == SIG_DFL && !p->parent) return 0;   /* nobody would continue it */
    if (ignored(p, sig)) return 0;
    u64 f = irq_save();
    struct thread *target = to;
    if (to) {
        to->sigpending |= BIT(sig);
        if (to->sigmask & BIT(sig)) target = NULL;
    } else {
        p->sigpending |= BIT(sig);
        for (struct thread *t = p->threads; t && !target; t = t->proc_next)
            if (!(t->sigmask & BIT(sig))) target = t;
    }
    irq_restore(f);
    if (target) thread_interrupt(target);
    return 0;
}

int signal_send_thread(struct thread *t, int sig) { return send(t->process, t, sig); }

int signal_send_process(struct process *p, int sig) { return send(p, NULL, sig); }

bool signal_blocked_or_ignored(struct thread *t, int sig) {
    return (t->sigmask & BIT(sig)) || t->process->sigactions[sig].handler == SIG_IGN;
}

/* ---- delivery ----------------------------------------------------------------------------- */

static int next_signal(struct thread *t, bool *from_process) {
    u64 ready = t->sigpending & ~t->sigmask;
    *from_process = false;
    if (!ready) {
        ready = t->process->sigpending & ~t->sigmask;
        *from_process = true;
    }
    return ready ? __builtin_ctzll(ready) + 1 : 0;
}

static bool on_altstack(struct thread *t, u64 sp) {
    return t->altstack_size && sp > t->altstack_sp && sp <= t->altstack_sp + t->altstack_size;
}

/* Builds the signal frame and points the registers at the handler. False if the stack is unusable. */
static bool setup_frame(struct interrupt_frame *f, int sig, const struct sigaction_k *act) {
    struct thread *t = self_thread();
    struct mm *mm = t->process->mm;
    u64 sp = f->rsp;
    if ((act->flags & SA_ONSTACK) && t->altstack_size && !(t->altstack_flags & SS_DISABLE) && !on_altstack(t, sp))
        sp = t->altstack_sp + t->altstack_size;
    else
        sp -= 128;   /* the red zone below the interrupted code's stack pointer */
    u64 fpstate = ALIGN_DOWN(sp - 512, 64);
    u64 frame_addr = ALIGN_DOWN(fpstate - sizeof(struct rt_sigframe), 16) - 8;   /* as if just called */
    if (frame_addr < USER_LOAD_MIN || !mm_user_access(mm, frame_addr, fpstate + 512 - frame_addr, true)) return false;

    u8 fx[512] ALIGNED(16);
    fxsave(fx);
    memcpy((void *)fpstate, fx, 512);

    struct rt_sigframe *fr = (struct rt_sigframe *)frame_addr;
    memset(fr, 0, sizeof *fr);
    fr->pretcode = act->restorer;
    fr->uc.ss_sp = t->altstack_sp;
    fr->uc.ss_flags = t->altstack_size ? (on_altstack(t, f->rsp) ? SS_ONSTACK : 0) : SS_DISABLE;
    fr->uc.ss_size = t->altstack_size;
    struct sigcontext_k *mc = &fr->uc.mc;
    mc->r8 = f->r8, mc->r9 = f->r9, mc->r10 = f->r10, mc->r11 = f->r11;
    mc->r12 = f->r12, mc->r13 = f->r13, mc->r14 = f->r14, mc->r15 = f->r15;
    mc->rdi = f->rdi, mc->rsi = f->rsi, mc->rbp = f->rbp, mc->rbx = f->rbx;
    mc->rdx = f->rdx, mc->rax = f->rax, mc->rcx = f->rcx, mc->rsp = f->rsp, mc->rip = f->rip;
    mc->eflags = f->rflags;
    mc->cs = 0x23;
    mc->ss = 0x1B;
    if (f->vector == SYSCALL_VECTOR) {
        /* an interrupted system call with SA_RESTART runs again once the handler returns */
        if ((i64)f->rax == -EINTR && (act->flags & SA_RESTART)) {
            mc->rax = f->error;   /* syscall_dispatch keeps the number there */
            mc->rip -= 2;         /* back to the SYSCALL instruction */
        }
    } else {
        mc->err = f->error;
        mc->trapno = f->vector;
        if (f->vector == 14) mc->cr2 = read_cr2();
    }
    mc->oldmask = t->restore_sigmask ? t->saved_sigmask : t->sigmask;
    mc->fpstate = fpstate;
    fr->uc.uc_sigmask = t->restore_sigmask ? t->saved_sigmask : t->sigmask;
    fr->info.si_signo = sig;
    if (f->vector == 14 || f->vector == 13) fr->info.fields[0] = f->vector == 14 ? read_cr2() : 0;   /* si_addr */

    f->rdi = (u64)sig;
    f->rsi = (u64)&fr->info;
    f->rdx = (u64)&fr->uc;
    f->rax = 0;
    f->rip = act->handler;
    f->rsp = frame_addr;
    f->rflags &= ~(0x100ULL | 0x400ULL);   /* TF, DF */
    thread_reset_fpu();                    /* the handler starts with a clean FPU */
    t->iret_return = true;
    return true;
}

void user_return(struct interrupt_frame *f) {
    struct thread *t = self_thread();
    struct process *p = t->process;
    if (!p || !p->user) return;
    /* a signal taken without a handler (or a stop) interrupted the call: it starts again */
    bool restart = t->restart_syscall;
    t->restart_syscall = false;
    for (;;) {
        if (p->exiting || t->exit_pending) {
            __asm__ volatile("sti");
            thread_exit_user(0);
        }
        if (p->stopped) {   /* wait here until SIGCONT (or SIGKILL) */
            u64 fl = irq_save();
            while (p->stopped && !p->exiting) waitqueue_wait(&p->stop_wq);
            irq_restore(fl);
            restart = true;
            continue;
        }
        bool from_process;
        u64 fl = irq_save();
        int sig = next_signal(t, &from_process);
        if (!sig) {
            if (t->restore_sigmask) {   /* sigsuspend ended without a handler */
                t->sigmask = t->saved_sigmask;
                t->restore_sigmask = false;
            }
            t->intr = false;   /* nothing left that should interrupt a wait */
            irq_restore(fl);
            if (restart && f->vector == SYSCALL_VECTOR && (i64)f->rax == -EINTR) {
                f->rax = f->error;   /* the system call number (syscall_dispatch keeps it there) */
                f->rip -= 2;         /* back to the SYSCALL instruction */
                t->iret_return = true;
            }
            return;
        }
        if (from_process) p->sigpending &= ~BIT(sig);
        else t->sigpending &= ~BIT(sig);
        irq_restore(fl);

        struct sigaction_k act = p->sigactions[sig];
        if (act.handler == SIG_IGN || (act.handler == SIG_DFL && default_ignored(sig))) {
            restart = true;
            continue;
        }
        if (act.handler == SIG_DFL) {
            if (is_stop(sig)) {
                stop_process(p, sig);   /* the loop waits at the top */
                continue;
            }
            process_kill(p, sig);
            continue;   /* p->exiting is now set */
        }
        __asm__ volatile("sti");   /* writing the frame may fault pages in */
        if (!setup_frame(f, sig, &act)) {
            process_kill(p, SIGSEGV);
            continue;
        }
        fl = irq_save();
        t->restore_sigmask = false;   /* the frame holds the mask to restore */
        t->sigmask |= act.mask | (act.flags & SA_NODEFER ? 0 : BIT(sig));
        t->sigmask &= ~UNBLOCKABLE;
        if (act.flags & SA_RESETHAND) p->sigactions[sig].handler = SIG_DFL;
        u64 left = (t->sigpending | p->sigpending) & ~t->sigmask;
        t->intr = left != 0;
        irq_restore(fl);
        return;   /* further signals go after this handler starts (on the next return) */
    }
}

/* A CPU exception in user mode. */
void process_fault(struct interrupt_frame *f) {
    struct thread *t = self_thread();
    struct process *p = t->process;
    int sig = f->vector == 6 ? SIGILL
            : f->vector == 0 || f->vector == 16 || f->vector == 19 ? SIGFPE
            : f->vector == 1 || f->vector == 3 ? SIGTRAP
            : f->vector == 17 ? SIGBUS
            : SIGSEGV;
    u64 h = p->sigactions[sig].handler;
    if (h != SIG_DFL && h != SIG_IGN && !(t->sigmask & BIT(sig))) {
        t->sigpending |= BIT(sig);   /* user_return delivers it to the handler right away */
        return;
    }
    console_set_color(COLOR_WARNING);
    kprintf("Process \"%s\" (pid %lu) crashed: %s at RIP=%p", p->name, p->pid, exception_name(f->vector),
            (void *)f->rip);
    if (f->vector == 14) kprintf(", address %p", (void *)read_cr2());
    kprintf("\n  The process was stopped (exit code %d). The system continues.\n", 128 + sig);
    console_set_color(COLOR_TEXT);
    process_kill(p, sig);
}

void signals_exec_reset(struct process *p) {
    for (int i = 1; i < NSIG; i++)
        if (p->sigactions[i].handler != SIG_IGN) memset(&p->sigactions[i], 0, sizeof p->sigactions[i]);
}

/* ---- system calls ------------------------------------------------------------------------- */

static bool user_ok(u64 addr, u64 len, bool write) {
    return mm_user_access(self_thread()->process->mm, addr, len, write);
}

i64 sys_rt_sigaction(u64 sig, u64 act, u64 oact, u64 size) {
    if (size != 8 || sig < 1 || sig >= NSIG) return -EINVAL;
    if (act && (sig == SIGKILL || sig == SIGSTOP)) return -EINVAL;
    struct process *p = self_thread()->process;
    if (act && !user_ok(act, sizeof(struct sigaction_k), false)) return -EFAULT;
    if (oact && !user_ok(oact, sizeof(struct sigaction_k), true)) return -EFAULT;
    struct sigaction_k old = p->sigactions[sig];
    if (act) {
        struct sigaction_k n = *(const struct sigaction_k *)act;
        n.mask &= ~UNBLOCKABLE;
        p->sigactions[sig] = n;
        if (ignored(p, sig)) {   /* pending ones are discarded */
            u64 f = irq_save();
            p->sigpending &= ~BIT(sig);
            for (struct thread *t = p->threads; t; t = t->proc_next) t->sigpending &= ~BIT(sig);
            irq_restore(f);
        }
    }
    if (oact) *(struct sigaction_k *)oact = old;
    return 0;
}

i64 sys_rt_sigprocmask(u64 how, u64 set, u64 oset, u64 size) {
    if (size != 8) return -EINVAL;
    struct thread *t = self_thread();
    if (set && !user_ok(set, 8, false)) return -EFAULT;
    if (oset && !user_ok(oset, 8, true)) return -EFAULT;
    u64 old = t->sigmask;
    if (set) {
        u64 s = *(const u64 *)set;
        if (how == 0) t->sigmask |= s;          /* SIG_BLOCK */
        else if (how == 1) t->sigmask &= ~s;    /* SIG_UNBLOCK */
        else if (how == 2) t->sigmask = s;      /* SIG_SETMASK */
        else return -EINVAL;
        t->sigmask &= ~UNBLOCKABLE;
        if ((t->sigpending | t->process->sigpending) & ~t->sigmask) t->intr = true;
    }
    if (oset) *(u64 *)oset = old;
    return 0;
}

i64 sys_rt_sigpending(u64 set, u64 size) {
    if (size != 8) return -EINVAL;
    if (!user_ok(set, 8, true)) return -EFAULT;
    struct thread *t = self_thread();
    *(u64 *)set = (t->sigpending | t->process->sigpending) & t->sigmask;
    return 0;
}

i64 sys_rt_sigsuspend(u64 set, u64 size) {
    if (size != 8) return -EINVAL;
    if (!user_ok(set, 8, false)) return -EFAULT;
    return signal_suspend(*(const u64 *)set);
}

i64 signal_suspend(u64 mask) {
    struct thread *t = self_thread();
    /* wait with a temporary mask; the old one comes back in user_return (after a handler: at sigreturn) */
    t->saved_sigmask = t->sigmask;
    t->restore_sigmask = true;
    t->sigmask = mask & ~UNBLOCKABLE;
    u64 f = irq_save();
    while (!((t->sigpending | t->process->sigpending) & ~t->sigmask) && !t->process->exiting)
        if (thread_block(0, true) == -EINTR) break;
    t->intr = true;
    irq_restore(f);
    return -EINTR;
}

i64 sys_rt_sigreturn(struct interrupt_frame *f) {
    struct thread *t = self_thread();
    u64 addr = f->rsp - 8;   /* the handler's RET already popped pretcode */
    if (!user_ok(addr, sizeof(struct rt_sigframe), false)) {
        process_kill(t->process, SIGSEGV);
        return 0;
    }
    const struct rt_sigframe *fr = (const struct rt_sigframe *)addr;
    const struct sigcontext_k *mc = &fr->uc.mc;
    if (mc->rip >= USER_TOP || mc->rsp >= USER_TOP) {
        process_kill(t->process, SIGSEGV);
        return 0;
    }
    if (mc->fpstate) {
        if (!user_ok(mc->fpstate, 512, false)) {
            process_kill(t->process, SIGSEGV);
            return 0;
        }
        u8 fx[512] ALIGNED(16);
        memcpy(fx, (const void *)mc->fpstate, 512);
        u32 mask = *(u32 *)(fx + 28);   /* MXCSR_MASK */
        *(u32 *)(fx + 24) &= mask ? mask : 0xFFBF;   /* reserved MXCSR bits would fault */
        fxrstor(fx);
    }
    f->r8 = mc->r8, f->r9 = mc->r9, f->r10 = mc->r10, f->r11 = mc->r11;
    f->r12 = mc->r12, f->r13 = mc->r13, f->r14 = mc->r14, f->r15 = mc->r15;
    f->rdi = mc->rdi, f->rsi = mc->rsi, f->rbp = mc->rbp, f->rbx = mc->rbx;
    f->rdx = mc->rdx, f->rax = mc->rax, f->rcx = mc->rcx, f->rsp = mc->rsp, f->rip = mc->rip;
    const u64 user_flags = 0x254DD5;   /* CF PF AF ZF SF TF DF OF AC ID RF */
    f->rflags = (mc->eflags & user_flags) | 0x202;
    f->cs = 0x23;
    f->ss = 0x1B;
    t->sigmask = fr->uc.uc_sigmask & ~UNBLOCKABLE;
    if ((t->sigpending | t->process->sigpending) & ~t->sigmask) t->intr = true;
    t->iret_return = true;
    return (i64)f->rax;   /* syscall_dispatch stores the result in rax: keep the restored value */
}

i64 sys_sigaltstack(u64 ss, u64 old) {
    struct thread *t = self_thread();
    struct stack_k {
        u64 sp;
        i32 flags, pad;
        u64 size;
    };
    if (old) {
        if (!user_ok(old, sizeof(struct stack_k), true)) return -EFAULT;
        struct stack_k *o = (struct stack_k *)old;
        o->sp = t->altstack_sp;
        o->size = t->altstack_size;
        o->flags = !t->altstack_size ? SS_DISABLE : 0;
        o->pad = 0;
    }
    if (ss) {
        if (!user_ok(ss, sizeof(struct stack_k), false)) return -EFAULT;
        struct stack_k n = *(const struct stack_k *)ss;
        if (n.flags & ~SS_DISABLE) return -EINVAL;
        if (n.flags & SS_DISABLE) {
            t->altstack_sp = t->altstack_size = 0;
            t->altstack_flags = SS_DISABLE;
        } else {
            if (n.size < MINSIGSTKSZ) return -ENOMEM;
            t->altstack_sp = n.sp;
            t->altstack_size = n.size;
            t->altstack_flags = 0;
        }
    }
    return 0;
}
