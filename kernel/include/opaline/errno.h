/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Error numbers: the same values as Linux (mixed ABI, project.md section 3.1).
   System calls return -errno on failure. */
#ifndef OPALINE_ERRNO_H
#define OPALINE_ERRNO_H

#define EPERM    1
#define ENOENT   2
#define EINTR    4
#define EIO      5
#define EBADF    9
#define ENOMEM  12
#define EFAULT  14
#define EINVAL  22
#define ENOSYS  38

#endif
