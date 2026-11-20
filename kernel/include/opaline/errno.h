/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Error numbers: the same values as Linux (mixed ABI, project.md section 3.1).
   System calls return -errno on failure. */
#ifndef OPALINE_ERRNO_H
#define OPALINE_ERRNO_H

#define EPERM         1
#define ENOENT        2
#define ESRCH         3
#define EINTR         4
#define EIO           5
#define ENXIO         6
#define E2BIG         7
#define ENOEXEC       8
#define EBADF         9
#define ECHILD       10
#define EAGAIN       11
#define ENOMEM       12
#define EACCES       13
#define EFAULT       14
#define EBUSY        16
#define EEXIST       17
#define EXDEV        18
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define EMFILE       24
#define ENOTTY       25
#define ENOSPC       28
#define ESPIPE       29
#define EPIPE        32
#define ERANGE       34
#define EDEADLK      35
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39
#define ELOOP        40
#define EOVERFLOW    75
#define ENOTSOCK     88
#define EOPNOTSUPP   95
#define ETIMEDOUT   110

#endif
