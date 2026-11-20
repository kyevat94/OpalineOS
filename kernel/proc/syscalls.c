/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * System calls. Numbers, arguments, structures and error codes follow Linux
 * x86-64 (mixed ABI, project.md section 3.1); OpalineOS's own calls use
 * numbers from 1000 on. Anything not implemented returns -ENOSYS.
 *
 * Every user pointer is checked against the calling process's memory areas
 * before the kernel touches it (-EFAULT otherwise); the check also brings the
 * pages in, so the kernel can then read and write them directly.
 */
#include <opaline/arch/cpu.h>
#include <opaline/arch/idt.h>
#include <opaline/arch/syscall.h>
#include <opaline/console.h>
#include <opaline/errno.h>
#include <opaline/mm.h>
#include <opaline/power.h>
#include <opaline/proc.h>
#include <opaline/sched.h>
#include <opaline/string.h>
#include <opaline/uvm.h>
#include <opaline/vfs.h>

enum {
    SYS_read = 0, SYS_write = 1, SYS_open = 2, SYS_close = 3, SYS_stat = 4, SYS_fstat = 5, SYS_lstat = 6,
    SYS_lseek = 8, SYS_mmap = 9, SYS_mprotect = 10, SYS_munmap = 11, SYS_brk = 12, SYS_rt_sigaction = 13,
    SYS_rt_sigprocmask = 14, SYS_rt_sigreturn = 15, SYS_ioctl = 16, SYS_pread64 = 17, SYS_pwrite64 = 18,
    SYS_readv = 19, SYS_writev = 20, SYS_access = 21, SYS_pipe = 22, SYS_sched_yield = 24, SYS_mremap = 25,
    SYS_msync = 26, SYS_madvise = 28, SYS_dup = 32, SYS_dup2 = 33, SYS_pause = 34, SYS_nanosleep = 35,
    SYS_getpid = 39, SYS_clone = 56, SYS_fork = 57, SYS_vfork = 58, SYS_execve = 59, SYS_exit = 60,
    SYS_wait4 = 61, SYS_kill = 62, SYS_uname = 63, SYS_fcntl = 72, SYS_flock = 73, SYS_fsync = 74,
    SYS_fdatasync = 75, SYS_truncate = 76, SYS_ftruncate = 77, SYS_getcwd = 79, SYS_chdir = 80, SYS_fchdir = 81,
    SYS_rename = 82, SYS_mkdir = 83, SYS_rmdir = 84, SYS_unlink = 87, SYS_readlink = 89, SYS_chmod = 90,
    SYS_fchmod = 91, SYS_chown = 92, SYS_fchown = 93, SYS_umask = 95, SYS_gettimeofday = 96, SYS_getrlimit = 97,
    SYS_getuid = 102, SYS_getgid = 104, SYS_setuid = 105, SYS_setgid = 106, SYS_geteuid = 107, SYS_getegid = 108,
    SYS_setpgid = 109, SYS_getppid = 110, SYS_getpgrp = 111, SYS_setsid = 112, SYS_getgroups = 115,
    SYS_getpgid = 121, SYS_getsid = 124, SYS_rt_sigpending = 127, SYS_rt_sigsuspend = 130, SYS_sigaltstack = 131,
    SYS_arch_prctl = 158, SYS_setrlimit = 160, SYS_sync = 162, SYS_reboot = 169, SYS_gettid = 186,
    SYS_tkill = 200, SYS_time = 201, SYS_futex = 202, SYS_sched_getaffinity = 204, SYS_getdents64 = 217,
    SYS_set_tid_address = 218, SYS_clock_gettime = 228, SYS_clock_getres = 229, SYS_clock_nanosleep = 230,
    SYS_exit_group = 231, SYS_tgkill = 234, SYS_openat = 257, SYS_mkdirat = 258, SYS_newfstatat = 262,
    SYS_unlinkat = 263, SYS_renameat = 264, SYS_readlinkat = 267, SYS_fchmodat = 268, SYS_faccessat = 269,
    SYS_set_robust_list = 273, SYS_dup3 = 292, SYS_pipe2 = 293, SYS_prlimit64 = 302, SYS_renameat2 = 316,
    SYS_getrandom = 318, SYS_faccessat2 = 439,
    SYS_opaline_spawn = 1000,   /* spawn(path, argv): starts a program, returns its pid */
};

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_EMPTY_PATH 0x1000
#define TIOCGWINSZ 0x5413
#define FD_CLOEXEC 1
#define ENODEV 19

struct timespec {
    i64 tv_sec;
    i64 tv_nsec;
};

struct linux_stat {   /* struct stat on Linux x86-64 */
    u64 st_dev, st_ino, st_nlink;
    u32 st_mode, st_uid, st_gid, pad0;
    u64 st_rdev;
    i64 st_size, st_blksize, st_blocks;
    i64 st_atime, st_atime_nsec, st_mtime, st_mtime_nsec, st_ctime, st_ctime_nsec;
    i64 reserved[3];
};

struct iovec {
    u64 base, len;
};

static struct process *self(void) { return thread_current()->process; }

static bool user_ok(u64 addr, u64 len, bool write) { return mm_user_access(self()->mm, addr, len, write); }

/* Copies a NUL-terminated user string; -EFAULT if unmapped, -ENAMETOOLONG if longer than max-1. */
static int copy_string(u64 src, char *dst, u64 max) {
    for (u64 i = 0; i < max; i++) {
        if ((i == 0 || (src + i) % PAGE_SIZE == 0) && !user_ok(src + i, 1, false)) return -EFAULT;
        dst[i] = ((const char *)src)[i];
        if (!dst[i]) return 0;
    }
    return -ENAMETOOLONG;
}

/* ---- file descriptors ----------------------------------------------------------- */

static struct file *get_file(u64 fd) {
    return fd < PROCESS_MAX_FDS ? self()->fds[fd] : NULL;
}

static int install(struct file *f, int min_fd, bool cloexec) {
    struct process *p = self();
    for (int i = min_fd; i < PROCESS_MAX_FDS; i++)
        if (!p->fds[i]) {
            p->fds[i] = f;
            p->fd_flags[i] = cloexec ? FD_CLOEXEC : 0;
            return i;
        }
    return -EMFILE;
}

/* The directory a *at() call starts from: the current one, or an open directory. */
static int at_dir(i64 dirfd, struct vnode **out) {
    if (dirfd == AT_FDCWD) {
        *out = self()->cwd;
        return 0;
    }
    struct file *f = get_file((u64)dirfd);
    if (!f) return -EBADF;
    if (f->vn->type != VN_DIR) return -ENOTDIR;
    *out = f->vn;
    return 0;
}

static i64 sys_read(u64 fd, u64 buf, u64 len) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    if (!user_ok(buf, len, true)) return -EFAULT;
    return file_read(f, (void *)buf, len);
}

static i64 sys_write(u64 fd, u64 buf, u64 len) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    if (!user_ok(buf, len, false)) return -EFAULT;
    return file_write(f, (const void *)buf, len);
}

static i64 sys_pread(u64 fd, u64 buf, u64 len, i64 off, bool write) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    if (off < 0) return -EINVAL;
    if (f->vn->type != VN_FILE) return -ESPIPE;
    if (!user_ok(buf, len, !write)) return -EFAULT;
    u32 mode = f->flags & O_ACCMODE;
    if (write ? mode == O_RDONLY : mode == O_WRONLY) return -EBADF;
    if (write) return f->vn->ops->write ? f->vn->ops->write(f->vn, (u64)off, (const void *)buf, len) : -EINVAL;
    return f->vn->ops->read ? f->vn->ops->read(f->vn, (u64)off, (void *)buf, len) : -EINVAL;
}

static i64 sys_rwv(u64 fd, u64 iov_addr, u64 count, bool write) {
    if (count > 1024) return -EINVAL;
    if (!user_ok(iov_addr, count * sizeof(struct iovec), false)) return -EFAULT;
    const struct iovec *iov = (const struct iovec *)iov_addr;
    i64 total = 0;
    for (u64 i = 0; i < count; i++) {
        if (!iov[i].len) continue;
        i64 r = write ? sys_write(fd, iov[i].base, iov[i].len) : sys_read(fd, iov[i].base, iov[i].len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((u64)r < iov[i].len) break;   /* short read/write */
    }
    return total;
}

static i64 sys_openat(i64 dirfd, u64 path_addr, u64 flags) {
    char path[PATH_MAX];
    int r = copy_string(path_addr, path, sizeof path);
    if (r < 0) return r;
    struct vnode *dir;
    if (path[0] != '/' && (r = at_dir(dirfd, &dir)) < 0) return r;
    if (path[0] == '/') dir = NULL;
    struct file *f;
    r = vfs_open(path, dir, (u32)flags & ~(u32)O_CLOEXEC, &f);
    if (r < 0) return r;
    r = install(f, 0, flags & O_CLOEXEC);
    if (r < 0) file_put(f);
    return r;
}

static i64 sys_close(u64 fd) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    self()->fds[fd] = NULL;
    self()->fd_flags[fd] = 0;
    file_put(f);
    return 0;
}

static void to_linux_stat(const struct vnode_stat *s, struct linux_stat *out) {
    memset(out, 0, sizeof *out);
    out->st_dev = s->dev;
    out->st_ino = s->ino;
    out->st_nlink = s->links;
    out->st_mode = s->mode;
    out->st_rdev = s->rdev;
    out->st_size = (i64)s->size;
    out->st_blksize = 4096;
    out->st_blocks = (i64)s->blocks;
}

static i64 sys_fstat(u64 fd, u64 buf) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    if (!user_ok(buf, sizeof(struct linux_stat), true)) return -EFAULT;
    struct vnode_stat s;
    int r = f->vn->ops->stat ? f->vn->ops->stat(f->vn, &s) : -EINVAL;
    if (r < 0) return r;
    to_linux_stat(&s, (struct linux_stat *)buf);
    return 0;
}

static i64 sys_fstatat(i64 dirfd, u64 path_addr, u64 buf, u64 flags) {
    char path[PATH_MAX];
    int r = copy_string(path_addr, path, sizeof path);
    if (r < 0) return r;
    if (!path[0]) {
        if (!(flags & AT_EMPTY_PATH)) return -ENOENT;
        if (dirfd == AT_FDCWD) return -EBADF;
        return sys_fstat((u64)dirfd, buf);
    }
    if (!user_ok(buf, sizeof(struct linux_stat), true)) return -EFAULT;
    struct vnode *dir = NULL;
    if (path[0] != '/' && (r = at_dir(dirfd, &dir)) < 0) return r;
    struct vnode_stat s;
    r = vfs_stat(path, dir, &s);   /* no symbolic links yet: AT_SYMLINK_NOFOLLOW changes nothing */
    if (r < 0) return r;
    to_linux_stat(&s, (struct linux_stat *)buf);
    return 0;
}

static i64 sys_faccessat(i64 dirfd, u64 path_addr) {
    char path[PATH_MAX];
    int r = copy_string(path_addr, path, sizeof path);
    if (r < 0) return r;
    struct vnode *dir = NULL;
    if (path[0] != '/' && (r = at_dir(dirfd, &dir)) < 0) return r;
    struct vnode_stat s;
    return vfs_stat(path, dir, &s);   /* no permissions yet: existing means accessible */
}

static i64 sys_readlinkat(i64 dirfd, u64 path_addr) {
    i64 r = sys_faccessat(dirfd, path_addr);
    return r < 0 ? r : -EINVAL;   /* there are no symbolic links */
}

static i64 sys_lseek(u64 fd, i64 offset, u64 whence) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    if (f->vn->type != VN_FILE && f->vn->type != VN_DIR) return -ESPIPE;
    i64 base;
    if (whence == 0) base = 0;
    else if (whence == 1) base = (i64)f->offset;
    else if (whence == 2) {
        struct vnode_stat s;
        int r = f->vn->ops->stat(f->vn, &s);
        if (r < 0) return r;
        base = (i64)s.size;
    } else return -EINVAL;
    if (base + offset < 0) return -EINVAL;
    f->offset = (u64)(base + offset);
    return (i64)f->offset;
}

static i64 sys_ioctl(u64 fd, u64 request, u64 arg) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    if (!f->vn->ops->ioctl) return -ENOTTY;
    if (request == TIOCGWINSZ && !user_ok(arg, 8, true)) return -EFAULT;
    return f->vn->ops->ioctl(f->vn, request, arg);
}

static i64 sys_pipe2(u64 fds_addr, u64 flags) {
    if (flags & ~(u64)(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    if (!user_ok(fds_addr, 8, true)) return -EFAULT;
    struct file *r, *w;
    int e = pipe_create(&r, &w);
    if (e < 0) return e;
    bool cloexec = flags & O_CLOEXEC;
    int a = install(r, 0, cloexec);
    int b = a >= 0 ? install(w, 0, cloexec) : -EMFILE;
    if (b < 0) {
        if (a >= 0) self()->fds[a] = NULL;
        file_put(r);
        file_put(w);
        return -EMFILE;
    }
    ((int *)fds_addr)[0] = a;
    ((int *)fds_addr)[1] = b;
    return 0;
}

static i64 sys_dup(u64 fd, int min_fd, bool cloexec) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    if (min_fd < 0 || min_fd >= PROCESS_MAX_FDS) return -EINVAL;
    file_ref(f);
    int r = install(f, min_fd, cloexec);
    if (r < 0) file_put(f);
    return r;
}

static i64 sys_dup3(u64 fd, u64 newfd, u64 flags, bool dup2) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    if (newfd >= PROCESS_MAX_FDS) return -EBADF;
    if (fd == newfd) return dup2 ? (i64)newfd : -EINVAL;
    if (flags & ~(u64)O_CLOEXEC) return -EINVAL;
    file_ref(f);
    struct file *old = self()->fds[newfd];
    self()->fds[newfd] = f;
    self()->fd_flags[newfd] = flags & O_CLOEXEC ? FD_CLOEXEC : 0;
    file_put(old);
    return (i64)newfd;
}

static i64 sys_fcntl(u64 fd, u64 cmd, u64 arg) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    struct process *p = self();
    switch (cmd) {
    case 0: return sys_dup(fd, (int)arg, false);      /* F_DUPFD */
    case 1030: return sys_dup(fd, (int)arg, true);    /* F_DUPFD_CLOEXEC */
    case 1: return p->fd_flags[fd];                   /* F_GETFD */
    case 2: p->fd_flags[fd] = arg & FD_CLOEXEC; return 0;   /* F_SETFD */
    case 3: return f->flags & (O_ACCMODE | O_APPEND | O_NONBLOCK);   /* F_GETFL */
    case 4: f->flags = (f->flags & ~(u32)(O_APPEND | O_NONBLOCK)) | ((u32)arg & (O_APPEND | O_NONBLOCK)); return 0;
    case 5:                                           /* F_GETLK: nothing is locked */
        if (!user_ok(arg, 2, true)) return -EFAULT;
        *(u16 *)arg = 2;                              /* F_UNLCK */
        return 0;
    case 6:
    case 7: return 0;                                 /* F_SETLK(W): one user, locks always succeed */
    default: return -EINVAL;
    }
}

static i64 sys_ftruncate(u64 fd, i64 size) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    if (size < 0) return -EINVAL;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (f->vn->type != VN_FILE || !f->vn->ops->truncate) return -EINVAL;
    return f->vn->ops->truncate(f->vn, (u64)size);
}

static i64 sys_fsync(u64 fd) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    return f->vn->ops->sync ? f->vn->ops->sync(f->vn) : 0;
}

static i64 sys_getdents64(u64 fd, u64 buf, u64 len) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    if (f->vn->type != VN_DIR) return -ENOTDIR;
    if (!user_ok(buf, len, true)) return -EFAULT;
    struct dir_entry *de = kmalloc(sizeof *de);   /* too large for the kernel stack */
    if (!de) return -ENOMEM;
    u64 used = 0;
    for (;;) {
        u64 cookie = f->offset;
        int r = f->vn->ops->readdir(f->vn, &cookie, de);
        if (r < 0) {
            kfree(de);
            return used ? (i64)used : r;
        }
        if (r == 0) break;
        u64 reclen = ALIGN_UP(19 + (u64)de->name_len + 1, 8);
        if (used + reclen > len) {
            if (!used) {
                kfree(de);
                return -EINVAL;   /* buffer too small for one entry */
            }
            break;
        }
        u8 *rec = (u8 *)buf + used;
        *(u64 *)rec = de->ino;
        *(i64 *)(rec + 8) = (i64)cookie;
        *(u16 *)(rec + 16) = (u16)reclen;
        rec[18] = de->type == VN_DIR ? 4 : de->type == VN_CHARDEV ? 2 : de->type == VN_PIPE ? 1 : 8;   /* DT_* */
        memcpy(rec + 19, de->name, de->name_len);
        memset(rec + 19 + de->name_len, 0, reclen - 19 - de->name_len);
        used += reclen;
        f->offset = cookie;
    }
    kfree(de);
    return (i64)used;
}

/* Makes `path` absolute and removes ".", ".." and repeated slashes (no symbolic links exist). */
static int join_path(const char *cwd, const char *path, char *out, u64 size) {
    char tmp[512];
    u64 n = 0;
    if (path[0] != '/') {
        for (const char *s = cwd; *s && n < sizeof tmp - 1; s++) tmp[n++] = *s;
        if (n < sizeof tmp - 1) tmp[n++] = '/';
    }
    for (const char *s = path; *s && n < sizeof tmp - 1; s++) tmp[n++] = *s;
    tmp[n] = 0;
    u64 o = 0;
    out[o++] = '/';
    for (char *p = tmp; *p;) {
        while (*p == '/') p++;
        char *start = p;
        while (*p && *p != '/') p++;
        u64 len = (u64)(p - start);
        if (len == 0 || (len == 1 && start[0] == '.')) continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (o > 1) {
                o--;
                while (o > 1 && out[o - 1] != '/') o--;
                if (o > 1) o--;
            }
            continue;
        }
        if (o > 1) out[o++] = '/';
        if (o + len + 1 > size) return -ENAMETOOLONG;
        memcpy(out + o, start, len);
        o += len;
    }
    out[o] = 0;
    return 0;
}

static i64 sys_chdir(u64 path_addr) {
    char path[PATH_MAX], abs[256];
    int r = copy_string(path_addr, path, sizeof path);
    if (r < 0) return r;
    struct process *p = self();
    struct vnode *v;
    r = vfs_lookup(path, p->cwd, &v);
    if (r < 0) return r;
    if (v->type != VN_DIR) {
        vnode_put(v);
        return -ENOTDIR;
    }
    r = join_path(p->cwd_path, path, abs, sizeof abs);
    if (r < 0) {
        vnode_put(v);
        return r;
    }
    vnode_put(p->cwd);
    p->cwd = v;
    memcpy(p->cwd_path, abs, sizeof abs);
    return 0;
}

static i64 sys_getcwd(u64 buf, u64 size) {
    u64 n = strlen(self()->cwd_path) + 1;
    if (size < n) return -ERANGE;
    if (!user_ok(buf, n, true)) return -EFAULT;
    memcpy((void *)buf, self()->cwd_path, n);
    return (i64)n;
}

/* mkdir(at), rmdir, unlink(at): op 0 = mkdir, 1 = rmdir, 2 = unlink */
static i64 path_call(i64 dirfd, u64 path_addr, int op) {
    char path[PATH_MAX];
    int r = copy_string(path_addr, path, sizeof path);
    if (r < 0) return r;
    struct vnode *dir = NULL;
    if (path[0] != '/' && (r = at_dir(dirfd, &dir)) < 0) return r;
    if (op == 0) return vfs_mkdir(path, dir);
    return vfs_remove(path, dir, op == 1);
}

static i64 sys_renameat(i64 olddir, u64 old_addr, i64 newdir, u64 new_addr) {
    char a[PATH_MAX], b[PATH_MAX];
    int r = copy_string(old_addr, a, sizeof a);
    if (r < 0) return r;
    r = copy_string(new_addr, b, sizeof b);
    if (r < 0) return r;
    struct vnode *da = NULL, *db = NULL;
    if (a[0] != '/' && (r = at_dir(olddir, &da)) < 0) return r;
    if (b[0] != '/' && (r = at_dir(newdir, &db)) < 0) return r;
    if (a[0] != '/' && b[0] != '/' && da != db) return -EXDEV;   /* vfs_rename takes one base directory */
    return vfs_rename(a, b, a[0] != '/' ? da : db);
}

/* ---- programs --------------------------------------------------------------------------- */

/* Strings copied from user memory into one kernel buffer (argv and envp of execve/spawn). */
struct str_vec {
    char *buf;
    u64 used;
    const char **ptrs;
    u64 count;
};

static int copy_vec(u64 addr, struct str_vec *v, const char ***out) {
    u64 first = v->count;
    for (u64 i = 0; addr; i++) {
        if (!user_ok(addr + i * 8, 8, false)) return -EFAULT;
        u64 s = ((const u64 *)addr)[i];
        if (!s) break;
        if (v->count >= ARGV_MAX) return -E2BIG;
        int r = copy_string(s, v->buf + v->used, ARGS_BYTES_MAX - v->used);
        if (r < 0) return r == -ENAMETOOLONG ? -E2BIG : r;
        v->ptrs[v->count++] = v->buf + v->used;
        v->used += strlen(v->buf + v->used) + 1;
    }
    v->ptrs[v->count++] = NULL;
    *out = v->ptrs + first;
    return 0;
}

static i64 run_program(struct interrupt_frame *f, u64 path_addr, u64 argv_addr, u64 envp_addr, bool spawn) {
    char path[PATH_MAX];
    int r = copy_string(path_addr, path, sizeof path);
    if (r < 0) return r;
    struct str_vec v = {kmalloc(ARGS_BYTES_MAX), 0, kmalloc((ARGV_MAX + 2) * sizeof(char *)), 0};
    i64 result = -ENOMEM;
    const char **argv = NULL, **envp = NULL;
    if (v.buf && v.ptrs) {
        result = copy_vec(argv_addr, &v, &argv);
        if (result == 0 && !spawn) result = copy_vec(envp_addr, &v, &envp);
    }
    if (result == 0) {
        if (spawn) {
            int err;
            struct process *child = process_spawn_path(path, argv, self(), &err);
            result = child ? (i64)child->pid : err;
        } else {
            result = process_execve(f, path, argv, envp);
        }
    }
    kfree(v.buf);
    kfree(v.ptrs);
    return result;
}

static i64 sys_wait4(i64 pid, u64 status_addr, u64 options) {
    if (status_addr && !user_ok(status_addr, 4, true)) return -EFAULT;
    int status = 0;
    i64 r = process_wait_child(self(), pid, &status, options & 1 /* WNOHANG */);
    if (r > 0 && status_addr) *(int *)status_addr = status;
    return r;
}

static i64 sys_kill(i64 pid, u64 sig) {
    if (sig >= NSIG) return -EINVAL;
    struct process *p = pid == 0 || pid == -1 ? self() : process_by_pid((u64)(pid < 0 ? -pid : pid));
    if (!p) return -ESRCH;
    return sig ? signal_send_process(p, (int)sig) : 0;
}

static i64 sys_tgkill(i64 tgid, i64 tid, u64 sig) {
    if (sig >= NSIG || tid <= 0) return -EINVAL;
    struct thread *t = thread_by_tid((u64)tid);
    if (!t || (tgid > 0 && t->process->pid != (u64)tgid)) return -ESRCH;
    return sig ? signal_send_thread(t, (int)sig) : 0;
}

/* reboot(magic1, magic2, cmd): the Linux interface. Any process may call it for now (there are
   no users yet); stage E15 restricts it to administrators. */
static i64 sys_reboot(u64 magic1, u64 magic2, u64 cmd) {
    if ((u32)magic1 != 0xFEE1DEAD || (u32)magic2 != 672274793) return -EINVAL;
    if ((u32)cmd == 0x4321FEDC) system_poweroff();   /* LINUX_REBOOT_CMD_POWER_OFF */
    if ((u32)cmd == 0x01234567) system_reboot();     /* LINUX_REBOOT_CMD_RESTART */
    return -EINVAL;
}

/* ---- memory ----------------------------------------------------------------------------- */

static i64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, i64 fd, u64 off) {
    struct vnode *file = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        struct file *f = get_file((u64)fd);
        if (!f) return -EBADF;
        if (f->vn->type != VN_FILE) return -ENODEV;
        if ((f->flags & O_ACCMODE) == O_WRONLY) return -EACCES;
        file = f->vn;
    }
    return mm_map(self()->mm, addr, len, (u32)prot, (u32)flags, file, off);
}

static i64 sys_mremap(u64 old, u64 old_len, u64 new_len) {
    if (old % PAGE_SIZE || !new_len) return -EINVAL;
    old_len = ALIGN_UP(old_len, PAGE_SIZE);
    new_len = ALIGN_UP(new_len, PAGE_SIZE);
    if (new_len == old_len) return (i64)old;
    if (new_len < old_len) {   /* shrinking: drop the tail */
        int r = mm_unmap(self()->mm, old + new_len, old_len - new_len);
        return r < 0 ? r : (i64)old;
    }
    return -ENOMEM;   /* growing is not supported yet: callers fall back to a new mapping */
}

/* ---- time --------------------------------------------------------------------------------- */

static i64 sys_clock_gettime(u64 clock, u64 tp) {
    if (clock > 7) return -EINVAL;   /* every clock counts from boot, in milliseconds (no RTC yet) */
    if (!user_ok(tp, sizeof(struct timespec), true)) return -EFAULT;
    u64 ms = time_ms();
    *(struct timespec *)tp = (struct timespec){(i64)(ms / 1000), (i64)(ms % 1000) * 1000000};
    return 0;
}

static i64 sys_clock_nanosleep(u64 clock, u64 flags, u64 req, u64 rem) {
    if (clock > 7) return -EINVAL;
    if (!user_ok(req, sizeof(struct timespec), false)) return -EFAULT;
    if (rem && !user_ok(rem, sizeof(struct timespec), true)) return -EFAULT;
    struct timespec t = *(const struct timespec *)req;
    if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000) return -EINVAL;
    u64 ms = (u64)t.tv_sec * 1000 + ((u64)t.tv_nsec + 999999) / 1000000;
    u64 start = time_ms();
    if (flags & 1) ms = ms > start ? ms - start : 0;   /* TIMER_ABSTIME */
    int r = thread_sleep_intr(ms);
    if (r < 0 && rem && !(flags & 1)) {
        u64 left = ms - MIN(ms, time_ms() - start);
        *(struct timespec *)rem = (struct timespec){(i64)(left / 1000), (i64)(left % 1000) * 1000000};
    }
    return r;
}

/* ---- information ---------------------------------------------------------------------- */

static i64 sys_uname(u64 buf) {
    if (!user_ok(buf, 6 * 65, true)) return -EFAULT;
    static const char *const fields[6] = {"OpalineOS", "opaline", "0.6.0-e6", "#1 OpalineOS", "x86_64", "(none)"};
    char *out = (char *)buf;
    memset(out, 0, 6 * 65);
    for (int i = 0; i < 6; i++) memcpy(out + i * 65, fields[i], strlen(fields[i]));
    return 0;
}

static i64 sys_prlimit(u64 pid, u64 resource, u64 new_limit, u64 old) {
    (void)new_limit;   /* limits cannot be changed yet */
    if (pid && pid != self()->pid) return -ESRCH;
    if (resource > 15) return -EINVAL;
    if (old) {
        if (!user_ok(old, 16, true)) return -EFAULT;
        u64 v = resource == 3 ? USER_STACK_SIZE : resource == 7 ? PROCESS_MAX_FDS : ~0ULL;   /* STACK, NOFILE */
        ((u64 *)old)[0] = ((u64 *)old)[1] = v;
    }
    return 0;
}

static i64 sys_arch_prctl(u64 code, u64 addr) {
    struct thread *t = thread_current();
    if (code == 0x1002) {   /* ARCH_SET_FS: the thread pointer of the C library */
        if (addr >= USER_TOP) return -EPERM;
        t->fs_base = addr;
        wrmsr(MSR_FS_BASE, addr);
        return 0;
    }
    if (code == 0x1003) {   /* ARCH_GET_FS */
        if (!user_ok(addr, 8, true)) return -EFAULT;
        *(u64 *)addr = t->fs_base;
        return 0;
    }
    return -EINVAL;
}

static i64 sys_getrandom(u64 buf, u64 len) {
    if (len > 33554431) len = 33554431;
    if (!user_ok(buf, len, true)) return -EFAULT;
    random_bytes((void *)buf, len);
    return (i64)len;
}

int syscall_dispatch(struct interrupt_frame *f) {
    u64 nr = f->rax;
    f->error = nr;   /* kept for SA_RESTART (signal.c) */
    u64 a = f->rdi, b = f->rsi, c = f->rdx, d = f->r10, e = f->r8, g = f->r9;
    struct thread *t = thread_current();
    struct process *p = t->process;
    i64 r;
    switch (nr) {
    /* files */
    case SYS_read:          r = sys_read(a, b, c); break;
    case SYS_write:         r = sys_write(a, b, c); break;
    case SYS_pread64:       r = sys_pread(a, b, c, (i64)d, false); break;
    case SYS_pwrite64:      r = sys_pread(a, b, c, (i64)d, true); break;
    case SYS_open:          r = sys_openat(AT_FDCWD, a, b); break;
    case SYS_openat:        r = sys_openat((i64)a, b, c); break;
    case SYS_close:         r = sys_close(a); break;
    case SYS_stat:
    case SYS_lstat:         r = sys_fstatat(AT_FDCWD, a, b, 0); break;   /* no symbolic links yet */
    case SYS_fstat:         r = sys_fstat(a, b); break;
    case SYS_newfstatat:    r = sys_fstatat((i64)a, b, c, d); break;
    case SYS_access:        r = sys_faccessat(AT_FDCWD, a); break;
    case SYS_faccessat:
    case SYS_faccessat2:    r = sys_faccessat((i64)a, b); break;
    case SYS_readlink:      r = sys_readlinkat(AT_FDCWD, a); break;
    case SYS_readlinkat:    r = sys_readlinkat((i64)a, b); break;
    case SYS_lseek:         r = sys_lseek(a, (i64)b, c); break;
    case SYS_ioctl:         r = sys_ioctl(a, b, c); break;
    case SYS_readv:         r = sys_rwv(a, b, c, false); break;
    case SYS_writev:        r = sys_rwv(a, b, c, true); break;
    case SYS_pipe:          r = sys_pipe2(a, 0); break;
    case SYS_pipe2:         r = sys_pipe2(a, b); break;
    case SYS_dup:           r = sys_dup(a, 0, false); break;
    case SYS_dup2:          r = sys_dup3(a, b, 0, true); break;
    case SYS_dup3:          r = sys_dup3(a, b, c, false); break;
    case SYS_fcntl:         r = sys_fcntl(a, b, c); break;
    case SYS_flock:         r = get_file(a) ? 0 : -EBADF; break;
    case SYS_fsync:
    case SYS_fdatasync:     r = sys_fsync(a); break;
    case SYS_sync:          r = 0; break;
    case SYS_ftruncate:     r = sys_ftruncate(a, (i64)b); break;
    case SYS_getcwd:        r = sys_getcwd(a, b); break;
    case SYS_chdir:         r = sys_chdir(a); break;
    case SYS_rename:        r = sys_renameat(AT_FDCWD, a, AT_FDCWD, b); break;
    case SYS_renameat:
    case SYS_renameat2:     r = d && nr == SYS_renameat2 ? -EINVAL : sys_renameat((i64)a, b, (i64)c, d); break;
    case SYS_mkdir:         r = path_call(AT_FDCWD, a, 0); break;
    case SYS_mkdirat:       r = path_call((i64)a, b, 0); break;
    case SYS_rmdir:         r = path_call(AT_FDCWD, a, 1); break;
    case SYS_unlink:        r = path_call(AT_FDCWD, a, 2); break;
    case SYS_unlinkat:      r = path_call((i64)a, b, c & AT_REMOVEDIR ? 1 : 2); break;
    case SYS_chmod:
    case SYS_fchmod:
    case SYS_fchmodat:
    case SYS_chown:
    case SYS_fchown:        r = 0; break;   /* no permissions yet */
    case SYS_umask:         r = 022; break;
    case SYS_getdents64:    r = sys_getdents64(a, b, c); break;
    /* memory */
    case SYS_mmap:          r = sys_mmap(a, b, c, d, (i64)e, g); break;
    case SYS_munmap:        r = mm_unmap(p->mm, a, b); break;
    case SYS_mprotect:      r = mm_protect(p->mm, a, b, (u32)c); break;
    case SYS_brk:           r = (i64)mm_brk(p->mm, a); break;
    case SYS_mremap:        r = sys_mremap(a, b, c); break;
    case SYS_madvise:
    case SYS_msync:         r = 0; break;
    /* processes and threads */
    case SYS_clone:         r = process_clone(f, a, b, c, d, e); break;
    case SYS_fork:          r = process_clone(f, SIGCHLD, 0, 0, 0, 0); break;
    case SYS_vfork:         r = process_clone(f, 0x4000 | 0x100 | SIGCHLD, 0, 0, 0, 0); break;
    case SYS_execve:        r = run_program(f, a, b, c, false); break;
    case SYS_opaline_spawn: r = run_program(f, a, b, 0, true); break;
    case SYS_exit:          thread_exit_user((int)(a & 0xFF));
    case SYS_exit_group:    process_exit((int)(a & 0xFF));
    case SYS_wait4:         r = sys_wait4((i64)a, b, c); break;
    case SYS_getpid:        r = (i64)p->pid; break;
    case SYS_getppid:       r = p->parent ? (i64)p->parent->pid : 0; break;
    case SYS_gettid:        r = (i64)t->tid; break;
    case SYS_set_tid_address: t->clear_child_tid = a; r = (i64)t->tid; break;
    case SYS_set_robust_list: r = 0; break;
    case SYS_arch_prctl:    r = sys_arch_prctl(a, b); break;
    case SYS_futex:         r = sys_futex(a, b, c, d, e, g); break;
    case SYS_sched_yield:   thread_yield(); r = 0; break;
    case SYS_sched_getaffinity:
        r = c && b >= 8 && user_ok(c, 8, true) ? (*(u64 *)c = 1, 8) : -EINVAL;   /* one CPU */
        break;
    case SYS_getpgid:
    case SYS_getpgrp:
    case SYS_getsid:
    case SYS_setsid:        r = (i64)p->pid; break;   /* every process leads its own group for now */
    case SYS_setpgid:       r = 0; break;
    case SYS_getuid:
    case SYS_geteuid:
    case SYS_getgid:
    case SYS_getegid:
    case SYS_setuid:
    case SYS_setgid:
    case SYS_getgroups:     r = 0; break;   /* one user (root) until stage E15 */
    case SYS_getrlimit:     r = sys_prlimit(0, a, 0, b); break;
    case SYS_setrlimit:     r = 0; break;
    case SYS_prlimit64:     r = sys_prlimit(a, b, c, d); break;
    case SYS_uname:         r = sys_uname(a); break;
    case SYS_getrandom:     r = sys_getrandom(a, b); break;
    case SYS_reboot:        r = sys_reboot(a, b, c); break;
    /* signals */
    case SYS_rt_sigaction:  r = sys_rt_sigaction(a, b, c, d); break;
    case SYS_rt_sigprocmask: r = sys_rt_sigprocmask(a, b, c, d); break;
    case SYS_rt_sigreturn:  r = sys_rt_sigreturn(f); break;
    case SYS_rt_sigpending: r = sys_rt_sigpending(a, b); break;
    case SYS_rt_sigsuspend: r = sys_rt_sigsuspend(a, b); break;
    case SYS_sigaltstack:   r = sys_sigaltstack(a, b); break;
    case SYS_pause:         r = signal_suspend(t->sigmask); break;
    case SYS_kill:          r = sys_kill((i64)a, b); break;
    case SYS_tkill:         r = sys_tgkill(0, (i64)a, b); break;
    case SYS_tgkill:        r = sys_tgkill((i64)a, (i64)b, c); break;
    /* time */
    case SYS_nanosleep:     r = sys_clock_nanosleep(1, 0, a, b); break;
    case SYS_clock_nanosleep: r = sys_clock_nanosleep(a, b, c, d); break;
    case SYS_clock_gettime: r = sys_clock_gettime(a, b); break;
    case SYS_clock_getres:
        r = a > 7 ? -EINVAL : !b ? 0 : user_ok(b, 16, true) ? (*(struct timespec *)b = (struct timespec){0, 1000000}, 0) : -EFAULT;
        break;
    case SYS_gettimeofday: {
        u64 ms = time_ms();
        r = !a ? 0 : user_ok(a, 16, true) ? (((i64 *)a)[0] = (i64)(ms / 1000), ((i64 *)a)[1] = (i64)(ms % 1000) * 1000, 0) : -EFAULT;
        break;
    }
    case SYS_time:
        r = (i64)(time_ms() / 1000);
        if (a) {
            if (user_ok(a, 8, true)) *(i64 *)a = r;
            else r = -EFAULT;
        }
        break;
    default:                r = -ENOSYS; break;
    }
    f->rax = (u64)r;
    user_return(f);
    bool full = t->iret_return;
    t->iret_return = false;
    return full;
}
