/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * System calls. Numbers, arguments, structures and error codes follow Linux
 * x86-64 (mixed ABI, project.md section 3.1); OpalineOS's own calls use
 * numbers from 1000 on. Anything not implemented returns -ENOSYS.
 *
 * Every user pointer is checked against the calling process's page tables
 * before the kernel touches it (-EFAULT otherwise).
 */
#include <opaline/arch/syscall.h>
#include <opaline/console.h>
#include <opaline/errno.h>
#include <opaline/mm.h>
#include <opaline/power.h>
#include <opaline/proc.h>
#include <opaline/sched.h>
#include <opaline/string.h>
#include <opaline/vfs.h>

#define SYS_read           0
#define SYS_write          1
#define SYS_open           2
#define SYS_close          3
#define SYS_stat           4
#define SYS_fstat          5
#define SYS_lstat          6
#define SYS_lseek          8
#define SYS_ioctl         16
#define SYS_readv         19
#define SYS_writev        20
#define SYS_pipe          22
#define SYS_sched_yield   24
#define SYS_dup           32
#define SYS_dup2          33
#define SYS_nanosleep     35
#define SYS_getpid        39
#define SYS_exit          60
#define SYS_wait4         61
#define SYS_fsync         74
#define SYS_ftruncate     77
#define SYS_getcwd        79
#define SYS_chdir         80
#define SYS_rename        82
#define SYS_mkdir         83
#define SYS_rmdir         84
#define SYS_unlink        87
#define SYS_getppid      110
#define SYS_reboot       169
#define SYS_gettid       186
#define SYS_getdents64   217
#define SYS_clock_gettime 228
#define SYS_exit_group   231
#define SYS_openat       257
#define SYS_pipe2        293
#define SYS_opaline_spawn 1000   /* spawn(path, argv): starts a program, returns its pid */

#define AT_FDCWD (-100)
#define TIOCGWINSZ 0x5413

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

static bool user_ok(u64 addr, u64 len, bool write) { return vmm_user_range_ok(self()->page_table, addr, len, write); }

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

static int install(struct file *f, int min_fd) {
    struct process *p = self();
    for (int i = min_fd; i < PROCESS_MAX_FDS; i++)
        if (!p->fds[i]) {
            p->fds[i] = f;
            return i;
        }
    return -EMFILE;
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

static i64 sys_open(u64 path_addr, u64 flags) {
    char path[PATH_MAX];
    int r = copy_string(path_addr, path, sizeof path);
    if (r < 0) return r;
    struct file *f;
    r = vfs_open(path, self()->cwd, (u32)flags, &f);
    if (r < 0) return r;
    r = install(f, 0);
    if (r < 0) file_put(f);
    return r;
}

static i64 sys_close(u64 fd) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    self()->fds[fd] = NULL;
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

static i64 sys_stat(u64 path_addr, u64 buf) {
    char path[PATH_MAX];
    int r = copy_string(path_addr, path, sizeof path);
    if (r < 0) return r;
    if (!user_ok(buf, sizeof(struct linux_stat), true)) return -EFAULT;
    struct vnode_stat s;
    r = vfs_stat(path, self()->cwd, &s);
    if (r < 0) return r;
    to_linux_stat(&s, (struct linux_stat *)buf);
    return 0;
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

static i64 sys_pipe(u64 fds_addr) {
    if (!user_ok(fds_addr, 8, true)) return -EFAULT;
    struct file *r, *w;
    int e = pipe_create(&r, &w);
    if (e < 0) return e;
    int a = install(r, 0);
    int b = a >= 0 ? install(w, 0) : -EMFILE;
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

static i64 sys_dup(u64 fd) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    file_ref(f);
    int r = install(f, 0);
    if (r < 0) file_put(f);
    return r;
}

static i64 sys_dup2(u64 fd, u64 newfd) {
    struct file *f = get_file(fd);
    if (!f) return -EBADF;
    if (newfd >= PROCESS_MAX_FDS) return -EBADF;
    if (fd == newfd) return (i64)newfd;
    file_ref(f);
    struct file *old = self()->fds[newfd];
    self()->fds[newfd] = f;
    file_put(old);
    return (i64)newfd;
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

static i64 path_call(u64 path_addr, int op) {
    char path[PATH_MAX];
    int r = copy_string(path_addr, path, sizeof path);
    if (r < 0) return r;
    struct vnode *cwd = self()->cwd;
    if (op == 0) return vfs_mkdir(path, cwd);
    return vfs_remove(path, cwd, op == 1);   /* 1 = rmdir, 2 = unlink */
}

static i64 sys_rename(u64 old_addr, u64 new_addr) {
    char a[PATH_MAX], b[PATH_MAX];
    int r = copy_string(old_addr, a, sizeof a);
    if (r < 0) return r;
    r = copy_string(new_addr, b, sizeof b);
    if (r < 0) return r;
    return vfs_rename(a, b, self()->cwd);
}

static i64 sys_spawn(u64 path_addr, u64 argv_addr) {
    char path[PATH_MAX];
    int r = copy_string(path_addr, path, sizeof path);
    if (r < 0) return r;
    /* copy argv into one kernel buffer */
    char *strings = kmalloc(ARGV_BYTES_MAX);
    const char **argv = kcalloc(ARGV_MAX + 1, sizeof *argv);
    if (!strings || !argv) {
        kfree(strings);
        kfree(argv);
        return -ENOMEM;
    }
    u64 used = 0, argc = 0;
    r = 0;
    while (argv_addr) {
        if (!user_ok(argv_addr + argc * 8, 8, false)) {
            r = -EFAULT;
            break;
        }
        u64 s = ((const u64 *)argv_addr)[argc];
        if (!s) break;
        if (argc == ARGV_MAX) {
            r = -E2BIG;
            break;
        }
        r = copy_string(s, strings + used, ARGV_BYTES_MAX - used);
        if (r < 0) {
            r = r == -ENAMETOOLONG ? -E2BIG : r;
            break;
        }
        argv[argc++] = strings + used;
        used += strlen(strings + used) + 1;
    }
    i64 result = r;
    if (r == 0) {
        int err;
        struct process *child = process_spawn_path(path, argv, self(), &err);
        result = child ? (i64)child->pid : err;
    }
    kfree(strings);
    kfree(argv);
    return result;
}

static i64 sys_wait4(i64 pid, u64 status_addr, u64 options) {
    if (status_addr && !user_ok(status_addr, 4, true)) return -EFAULT;
    int status = 0;
    i64 r = process_wait_child(self(), pid, &status, options & 1 /* WNOHANG */);
    if (r > 0 && status_addr) *(int *)status_addr = status;
    return r;
}

/* reboot(magic1, magic2, cmd): the Linux interface. Any process may call it for now (there are
   no users yet); stage E15 restricts it to administrators. */
static i64 sys_reboot(u64 magic1, u64 magic2, u64 cmd) {
    if ((u32)magic1 != 0xFEE1DEAD || (u32)magic2 != 672274793) return -EINVAL;
    if ((u32)cmd == 0x4321FEDC) system_poweroff();   /* LINUX_REBOOT_CMD_POWER_OFF */
    if ((u32)cmd == 0x01234567) system_reboot();     /* LINUX_REBOOT_CMD_RESTART */
    return -EINVAL;
}

/* ---- time and processes ------------------------------------------------------------- */

static i64 sys_nanosleep(u64 req, u64 rem) {
    if (!user_ok(req, sizeof(struct timespec), false)) return -EFAULT;
    if (rem && !user_ok(rem, sizeof(struct timespec), true)) return -EFAULT;
    struct timespec t = *(const struct timespec *)req;
    if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000) return -EINVAL;
    thread_sleep_ms((u64)t.tv_sec * 1000 + ((u64)t.tv_nsec + 999999) / 1000000);
    if (rem) *(struct timespec *)rem = (struct timespec){0, 0};
    return 0;
}

static i64 sys_clock_gettime(u64 clock, u64 tp) {
    if (clock > 1) return -EINVAL;   /* CLOCK_REALTIME (0) and CLOCK_MONOTONIC (1): time since boot */
    if (!user_ok(tp, sizeof(struct timespec), true)) return -EFAULT;
    u64 ms = time_ms();
    *(struct timespec *)tp = (struct timespec){(i64)(ms / 1000), (i64)(ms % 1000) * 1000000};
    return 0;
}

void syscall_dispatch(struct syscall_frame *f) {
    u64 a = f->rdi, b = f->rsi, c = f->rdx;
    i64 r;
    switch (f->rax) {
    case SYS_read:          r = sys_read(a, b, c); break;
    case SYS_write:         r = sys_write(a, b, c); break;
    case SYS_open:          r = sys_open(a, b); break;
    case SYS_openat:        r = (i64)a == AT_FDCWD ? sys_open(b, c) : -ENOSYS; break;
    case SYS_close:         r = sys_close(a); break;
    case SYS_stat:
    case SYS_lstat:         r = sys_stat(a, b); break;   /* no symbolic links yet */
    case SYS_fstat:         r = sys_fstat(a, b); break;
    case SYS_lseek:         r = sys_lseek(a, (i64)b, c); break;
    case SYS_ioctl:         r = sys_ioctl(a, b, c); break;
    case SYS_readv:         r = sys_rwv(a, b, c, false); break;
    case SYS_writev:        r = sys_rwv(a, b, c, true); break;
    case SYS_pipe:
    case SYS_pipe2:         r = sys_pipe(a); break;
    case SYS_dup:           r = sys_dup(a); break;
    case SYS_dup2:          r = sys_dup2(a, b); break;
    case SYS_fsync:         r = sys_fsync(a); break;
    case SYS_ftruncate:     r = sys_ftruncate(a, (i64)b); break;
    case SYS_getcwd:        r = sys_getcwd(a, b); break;
    case SYS_chdir:         r = sys_chdir(a); break;
    case SYS_rename:        r = sys_rename(a, b); break;
    case SYS_mkdir:         r = path_call(a, 0); break;
    case SYS_rmdir:         r = path_call(a, 1); break;
    case SYS_unlink:        r = path_call(a, 2); break;
    case SYS_getdents64:    r = sys_getdents64(a, b, c); break;
    case SYS_wait4:         r = sys_wait4((i64)a, b, c); break;
    case SYS_opaline_spawn: r = sys_spawn(a, b); break;
    case SYS_sched_yield:   thread_yield(); r = 0; break;
    case SYS_nanosleep:     r = sys_nanosleep(a, b); break;
    case SYS_getpid:        r = (i64)self()->pid; break;
    case SYS_reboot:        r = sys_reboot(a, b, c); break;
    case SYS_getppid:       r = self()->parent ? (i64)self()->parent->pid : 0; break;
    case SYS_gettid:        r = (i64)thread_current()->id; break;
    case SYS_clock_gettime: r = sys_clock_gettime(a, b); break;
    case SYS_exit:
    case SYS_exit_group:    process_exit((int)a);
    default:                r = -ENOSYS; break;
    }
    f->rax = (u64)r;
}
