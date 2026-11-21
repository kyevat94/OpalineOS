/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Users, groups and permissions (POSIX).
 *
 * Every process has a real, effective and saved user id and group id plus
 * supplementary groups. Files have an owner, a group and permission bits; the
 * VFS checks them on every access with the effective ids (access() uses the real
 * ids). User 0 (root) passes every check, except that a file needs at least one
 * execute bit to be run. Users and groups are listed in /etc/passwd and
 * /etc/group; there is no login yet (stage E15), so the system starts as root.
 */
#ifndef OPALINE_CRED_H
#define OPALINE_CRED_H

#include <opaline/types.h>

#define NGROUPS_MAX 32

struct cred {
    u32 uid, euid, suid;     /* real, effective, saved */
    u32 gid, egid, sgid;
    u32 ngroups;
    u32 groups[NGROUPS_MAX];
};

/* Mode bits (as in Linux st_mode) */
#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFREG  0100000
#define S_IFIFO  0010000
#define S_ISUID  04000
#define S_ISGID  02000
#define S_ISVTX  01000       /* sticky: in a directory, only the owner may delete a file */

/* Access wanted, for cred_may() */
#define MAY_EXEC  1
#define MAY_WRITE 2
#define MAY_READ  4

/* The credentials of the calling process (root for kernel threads). */
const struct cred *cred_current(void);
u32 cred_umask(void);   /* the calling process's umask (022 for kernel threads) */
bool cred_in_group(const struct cred *c, u32 gid);
/* May `c` access a file with this mode/owner? `real`: check the real ids (access()). */
bool cred_may(const struct cred *c, u32 mode, u32 uid, u32 gid, int want, bool real);
/* May a process with `from` send a signal to one with `to`? Root, or the same user. */
bool cred_may_signal(const struct cred *from, const struct cred *to);

#endif
