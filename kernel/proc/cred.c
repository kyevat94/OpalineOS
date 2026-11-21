/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Credentials and the POSIX permission check (see opaline/cred.h). */
#include <opaline/cred.h>
#include <opaline/proc.h>

static const struct cred root_cred;   /* all zero: root */

const struct cred *cred_current(void) {
    struct thread *t = thread_current();
    struct process *p = t ? t->process : NULL;
    return p && p->user ? &p->cred : &root_cred;
}

u32 cred_umask(void) {
    struct thread *t = thread_current();
    struct process *p = t ? t->process : NULL;
    return p && p->user ? p->umask : 022;
}

bool cred_in_group(const struct cred *c, u32 gid) {
    if (c->egid == gid) return true;
    for (u32 i = 0; i < c->ngroups; i++)
        if (c->groups[i] == gid) return true;
    return false;
}

bool cred_may(const struct cred *c, u32 mode, u32 uid, u32 gid, int want, bool real) {
    u32 me = real ? c->uid : c->euid;
    if (me == 0) {
        /* root may do anything, but runs only what is executable for someone (or enters directories) */
        return !(want & MAY_EXEC) || (mode & S_IFMT) == S_IFDIR || (mode & 0111);
    }
    u32 bits;
    if (me == uid) bits = mode >> 6;
    else if ((real ? c->gid == gid : false) || cred_in_group(c, gid)) bits = mode >> 3;
    else bits = mode;
    return ((u32)want & ~bits & 7) == 0;
}

bool cred_may_signal(const struct cred *from, const struct cred *to) {
    return from->euid == 0 || from->uid == to->uid || from->uid == to->suid || from->euid == to->uid ||
           from->euid == to->suid;
}
