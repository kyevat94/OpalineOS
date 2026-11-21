/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Minimal user-space library for the first OpalineOS programs (stages E4-E5).
 * It is replaced by a real C library (musl) in stage E6.
 *
 * System calls use the Linux x86-64 convention: RAX = number, arguments in
 * RDI, RSI, RDX, R10, R8, R9; the result (or -errno) comes back in RAX.
 */
#ifndef OPALINE_USER_SYS_H
#define OPALINE_USER_SYS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define SYS_read           0
#define SYS_write          1
#define SYS_open           2
#define SYS_close          3
#define SYS_stat           4
#define SYS_fstat          5
#define SYS_lseek          8
#define SYS_ioctl         16
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
#define SYS_reboot       169
#define SYS_gettid       186
#define SYS_getdents64   217
#define SYS_clock_gettime 228
#define SYS_exit_group   231
#define SYS_opaline_spawn 1000
#define SYS_rt_sigaction  13
#define SYS_kill          62
#define SYS_getpgrp      111

#define ENOENT        2
#define EBADF         9
#define ECHILD       10
#define EEXIST       17
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define ENOTTY       25
#define ESPIPE       29
#define EPIPE        32
#define EFAULT       14
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_DIRECTORY 0x10000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define S_IFMT  0xF000
#define S_IFDIR 0x4000
#define S_IFREG 0x8000
#define S_IFCHR 0x2000

#define DT_DIR 4
#define DT_REG 8

static inline long syscall0(long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory");
    return r;
}

static inline long syscall1(long n, long a) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory");
    return r;
}

static inline long syscall2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}

static inline long syscall3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static inline long syscall4(long n, long a, long b, long c, long d) {
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return r;
}

struct timespec {
    long tv_sec;
    long tv_nsec;
};

struct stat {   /* Linux x86-64 layout */
    unsigned long st_dev, st_ino, st_nlink;
    unsigned int st_mode, st_uid, st_gid, pad0;
    unsigned long st_rdev;
    long st_size, st_blksize, st_blocks;
    long st_atime, st_atime_nsec, st_mtime, st_mtime_nsec, st_ctime, st_ctime_nsec;
    long reserved[3];
};

struct dirent64 {
    unsigned long d_ino;
    long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static inline long read(int fd, void *buf, size_t n) { return syscall3(SYS_read, fd, (long)buf, (long)n); }
static inline long write(int fd, const void *buf, size_t n) { return syscall3(SYS_write, fd, (long)buf, (long)n); }
static inline int open(const char *path, int flags) { return (int)syscall3(SYS_open, (long)path, flags, 0644); }
static inline int close(int fd) { return (int)syscall1(SYS_close, fd); }
static inline int stat(const char *path, struct stat *st) { return (int)syscall2(SYS_stat, (long)path, (long)st); }
static inline int fstat(int fd, struct stat *st) { return (int)syscall2(SYS_fstat, fd, (long)st); }
static inline long lseek(int fd, long off, int whence) { return syscall3(SYS_lseek, fd, off, whence); }
static inline int ioctl(int fd, unsigned long req, void *arg) { return (int)syscall3(SYS_ioctl, fd, (long)req, (long)arg); }
static inline int pipe(int fds[2]) { return (int)syscall1(SYS_pipe, (long)fds); }
static inline int dup(int fd) { return (int)syscall1(SYS_dup, fd); }
static inline int dup2(int fd, int newfd) { return (int)syscall2(SYS_dup2, fd, newfd); }
static inline int fsync(int fd) { return (int)syscall1(SYS_fsync, fd); }
static inline int ftruncate(int fd, long size) { return (int)syscall2(SYS_ftruncate, fd, size); }
static inline long getcwd(char *buf, size_t size) { return syscall2(SYS_getcwd, (long)buf, (long)size); }
static inline int chdir(const char *path) { return (int)syscall1(SYS_chdir, (long)path); }
static inline int rename(const char *a, const char *b) { return (int)syscall2(SYS_rename, (long)a, (long)b); }
static inline int mkdir(const char *path) { return (int)syscall2(SYS_mkdir, (long)path, 0755); }
static inline int rmdir(const char *path) { return (int)syscall1(SYS_rmdir, (long)path); }
static inline int unlink(const char *path) { return (int)syscall1(SYS_unlink, (long)path); }
static inline long getdents64(int fd, void *buf, size_t len) { return syscall3(SYS_getdents64, fd, (long)buf, (long)len); }
static inline long getpid(void) { return syscall0(SYS_getpid); }
static inline long gettid(void) { return syscall0(SYS_gettid); }
static inline long sched_yield(void) { return syscall0(SYS_sched_yield); }
static inline long nanosleep(const struct timespec *req, struct timespec *rem) {
    return syscall2(SYS_nanosleep, (long)req, (long)rem);
}
static inline long clock_gettime(int clock, struct timespec *tp) { return syscall2(SYS_clock_gettime, clock, (long)tp); }
/* Starts a program; returns its pid or -errno. SPAWN_FOREGROUND: as a job of its own that gets the
   terminal (the shell gives it back with tcsetpgrp once the job ends or stops). */
#define SPAWN_FOREGROUND 1
static inline long spawn(const char *path, char *const argv[], int flags) {
    return syscall3(SYS_opaline_spawn, (long)path, (long)argv, flags);
}
static inline long kill(long pid, int sig) { return syscall2(SYS_kill, pid, sig); }
static inline long getpgrp(void) { return syscall0(SYS_getpgrp); }
static inline int tcsetpgrp(int fd, long pgid) {
    int id = (int)pgid;
    return ioctl(fd, 0x5410 /* TIOCSPGRP */, &id);
}
/* Ignores a signal (rt_sigaction with SIG_IGN). */
static inline long ignore_signal(int sig) {
    long action[4] = {1 /* SIG_IGN */, 0, 0, 0};
    return syscall4(SYS_rt_sigaction, sig, (long)action, 0, 8);
}
#define SIGINT  2
#define SIGQUIT 3
#define SIGCONT 18
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
/* Linux reboot(): turns the computer off or restarts it (does not return on success). */
static inline long poweroff(void) { return syscall3(SYS_reboot, 0xFEE1DEAD, 672274793, 0x4321FEDC); }
static inline long reboot(void) { return syscall3(SYS_reboot, 0xFEE1DEAD, 672274793, 0x01234567); }
static inline long wait4(long pid, int *status, int options) {
    return syscall4(SYS_wait4, pid, (long)status, options, 0);
}
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define WTERMSIG(s)    ((s) & 0x7F)
#define WIFSTOPPED(s)  (((s) & 0xFF) == 0x7F)
#define WSTOPSIG(s)    (((s) >> 8) & 0xFF)
#define WUNTRACED 2

_Noreturn void _exit(int code);
void sleep_ms(long ms);
long time_ms(void);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strchr(const char *s, int c);
int memcmp(const void *a, const void *b, size_t n);
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
const char *strerror_short(long err);   /* "ENOENT", ... for a negative errno */

#endif
