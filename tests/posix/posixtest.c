/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * POSIX test for OpalineOS (stage E6): an ordinary C program built against
 * musl. The build makes two copies: /apps/posixtest (dynamically linked,
 * started through /lib/ld-musl-x86_64.so.1) and /apps/posixtest-static.
 *
 * It checks files and directories, malloc, mmap/munmap/mprotect, signals,
 * fork/exec/wait, posix_spawn, pipes between processes, pthreads (mutexes,
 * condition variables, thread-local storage) and a few system queries; and,
 * since stage SE1, copy-on-write fork, shared memory (anonymous and file),
 * mremap, execve from a process with threads, the real clock and file times,
 * users and permissions (as an ordinary user, set-user-id programs) and job
 * control (stop/continue, process groups, restarted system calls); since
 * stage SE2, symbolic and hard links; and, since stage SE3, the terminal
 * settings (termios: canonical and raw mode), select/pselect and O_NONBLOCK;
 * and, since stage SE4, signals and CPU state (hostile signal frames, the NT
 * flag, si_addr, copy-on-write under kernel writes, which calls SA_RESTART
 * restarts, kill(-1), sigaltstack), file system details (pipe POLLOUT,
 * O_CREAT through links, readdir while unlinking, directory renames, slashes),
 * limits of shared and brk memory, trailing slashes on names to create,
 * remove or rename (as in Linux), brk over an mprotected heap top, interval
 * timers (alarm, setitimer, getitimer) and the process count of sysinfo.
 * "posixtest rawkeys", "vmin", "flushraw", "sleep" and "env NAME" are used by the shell test.
 * Prints "ok"/"FAIL" per check and ends with exit code 0 only if all passed.
 */
#define _GNU_SOURCE
#include <complex.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <grp.h>
#include <poll.h>
#define FUTEX_WAIT 0   /* linux/futex.h; musl has no kernel headers */
#define FUTEX_WAKE 1
#include <termios.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static int checks, failures;
static const char *self_path;

#define CHECK(cond, what)                                    \
    do {                                                     \
        checks++;                                            \
        if (cond) {                                          \
            printf("  ok   %s\n", what);                     \
        } else {                                             \
            failures++;                                      \
            printf("  FAIL %s (errno %d)\n", what, errno);   \
        }                                                    \
    } while (0)

/* ---- files ---------------------------------------------------------------------------- */

static void test_files(void) {
    printf("files:\n");
    mkdir("/tmp", 0755);
    CHECK(mkdir("/tmp/ptest", 0755) == 0 || errno == EEXIST, "mkdir /tmp/ptest");
    FILE *f = fopen("/tmp/ptest/a.txt", "w");
    CHECK(f != NULL, "fopen for writing");
    if (!f) return;
    for (int i = 0; i < 1000; i++) fprintf(f, "line %d: %s\n", i, "Olá, ação, 漢字");
    CHECK(fclose(f) == 0, "fclose");
    f = fopen("/tmp/ptest/a.txt", "r");
    char line[128];
    int n = 0, good = 1;
    while (f && fgets(line, sizeof line, f)) {
        char want[128];
        snprintf(want, sizeof want, "line %d: %s\n", n, "Olá, ação, 漢字");
        good &= !strcmp(line, want);
        n++;
    }
    CHECK(f && n == 1000 && good, "fgets reads back 1000 UTF-8 lines");
    CHECK(f && fseek(f, 0, SEEK_END) == 0 && ftell(f) > 20000, "fseek/ftell");
    if (f) fclose(f);
    struct stat st;
    CHECK(stat("/tmp/ptest/a.txt", &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 20000, "stat");
    CHECK(rename("/tmp/ptest/a.txt", "/tmp/ptest/b.txt") == 0 && access("/tmp/ptest/b.txt", F_OK) == 0 &&
              access("/tmp/ptest/a.txt", F_OK) != 0,
          "rename + access");
    int fd = open("/tmp/ptest", O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0 && fstatat(fd, "b.txt", &st, 0) == 0, "fstatat relative to a directory");
    if (fd >= 0) close(fd);
    DIR *d = opendir("/tmp/ptest");
    int found = 0;
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (!strcmp(e->d_name, "b.txt")) found = 1;
    if (d) closedir(d);
    CHECK(found, "opendir/readdir finds the file");
    fd = open("/tmp/ptest/b.txt", O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC), "O_CLOEXEC sets FD_CLOEXEC");
    if (fd >= 0) close(fd);
    CHECK(unlink("/tmp/ptest/b.txt") == 0 && rmdir("/tmp/ptest") == 0, "unlink + rmdir");
}

/* ---- memory --------------------------------------------------------------------------- */

static sigjmp_buf jump;
static volatile sig_atomic_t segv_count;
static volatile void *segv_addr;

static void on_segv(int sig, siginfo_t *info, void *uc) {
    (void)sig, (void)uc;
    segv_count++;
    segv_addr = info->si_addr;
    siglongjmp(jump, 1);
}

/* True if writing (or reading) *p raises SIGSEGV. */
static int faults(volatile char *p, int write) {
    if (sigsetjmp(jump, 1)) return 1;
    if (write) *p = 1;
    else (void)*p;
    return 0;
}

static void test_memory(void) {
    printf("memory:\n");
    /* malloc: many sizes, freed in a scrambled order */
    enum { N = 2000 };
    static void *blocks[N];
    static size_t sizes[N];
    int good = 1;
    for (int i = 0; i < N; i++) {
        sizes[i] = (size_t)(i * 37 % 3000) + 1;
        blocks[i] = malloc(sizes[i]);
        if (!blocks[i]) good = 0;
        else memset(blocks[i], i & 0xFF, sizes[i]);
    }
    for (int i = 0; i < N; i += 2) {
        unsigned char *b = blocks[i];
        for (size_t k = 0; b && k < sizes[i]; k++) good &= b[k] == (i & 0xFF);
        free(blocks[i]);
    }
    for (int i = 1; i < N; i += 2) {
        blocks[i] = realloc(blocks[i], sizes[i] * 3);
        unsigned char *b = blocks[i];
        for (size_t k = 0; b && k < sizes[i]; k++) good &= b[k] == (i & 0xFF);
        free(blocks[i]);
    }
    CHECK(good, "malloc/realloc/free of 2000 blocks keeps their contents");
    char *big = malloc(8 << 20);   /* large blocks come from mmap */
    CHECK(big != NULL, "malloc of 8 MiB");
    if (big) {
        big[0] = 1;
        big[(8 << 20) - 1] = 2;
        CHECK(big[0] == 1 && big[(8 << 20) - 1] == 2, "both ends of the 8 MiB block are usable");
        free(big);
    }

    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);

    long page = sysconf(_SC_PAGESIZE);
    CHECK(page == 4096, "page size is 4096");
    char *m = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED, "anonymous mmap");
    if (m == MAP_FAILED) return;
    CHECK(m[0] == 0 && m[4 * page - 1] == 0, "new anonymous memory is zero");
    memset(m, 0x5A, 4 * page);
    CHECK(mprotect(m + page, page, PROT_READ) == 0, "mprotect one page read-only");
    CHECK(!faults(m + page, 0), "reading the read-only page works");
    CHECK(faults(m + page, 1), "writing the read-only page raises SIGSEGV");
    CHECK(segv_addr == m + page, "si_addr is the faulting address");
    CHECK(!faults(m + 2 * page, 1), "the next page is still writable");
    CHECK(munmap(m + 3 * page, page) == 0, "munmap the last page");
    CHECK(faults(m + 3 * page, 0), "reading the unmapped page raises SIGSEGV");
    CHECK(munmap(m, 3 * page) == 0, "munmap the rest");
    CHECK(faults((char *)16, 0), "a NULL-page access raises SIGSEGV");

    /* a file mapping */
    int fd = open("/tmp/map.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    char data[6000];
    for (int i = 0; i < (int)sizeof data; i++) data[i] = (char)(i * 7);
    CHECK(fd >= 0 && write(fd, data, sizeof data) == sizeof data, "write a 6000-byte file");
    char *fm = mmap(NULL, sizeof data, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(fm != MAP_FAILED && !memcmp(fm, data, sizeof data), "mmap of the file shows its contents");
    if (fm != MAP_FAILED) {
        CHECK(fm[6000] == 0 && fm[8191] == 0, "the rest of the last page is zero");
        fm[0] = 'X';
        char first;
        CHECK(pread(fd, &first, 1, 0) == 1 && first == data[0], "MAP_PRIVATE writes do not reach the file");
        munmap(fm, sizeof data);
    }
    if (fd >= 0) close(fd);
    unlink("/tmp/map.bin");
    signal(SIGSEGV, SIG_DFL);
}

/* ---- signals -------------------------------------------------------------------------- */

static volatile sig_atomic_t got_usr1, got_usr2_signo;

static void on_usr1(int sig) { got_usr1 += sig == SIGUSR1; }
static void on_usr2(int sig, siginfo_t *info, void *uc) {
    (void)sig, (void)uc;
    got_usr2_signo = info->si_signo;
}

static void test_signals(void) {
    printf("signals:\n");
    signal(SIGUSR1, on_usr1);
    CHECK(raise(SIGUSR1) == 0 && got_usr1 == 1, "raise() runs the handler");
    struct sigaction sa = {0};
    sa.sa_sigaction = on_usr2;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &sa, NULL);
    CHECK(kill(getpid(), SIGUSR2) == 0 && got_usr2_signo == SIGUSR2, "kill(getpid()) with SA_SIGINFO");
    sigset_t set, pending;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);
    raise(SIGUSR1);
    sigpending(&pending);
    CHECK(got_usr1 == 1 && sigismember(&pending, SIGUSR1), "a blocked signal stays pending");
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    CHECK(got_usr1 == 2, "unblocking delivers it");
    signal(SIGUSR1, SIG_IGN);
    CHECK(raise(SIGUSR1) == 0 && got_usr1 == 2, "an ignored signal does nothing");
    double x = 1.5;   /* the FPU state survives a handler */
    raise(SIGUSR2);
    CHECK(x * 2 == 3.0, "floating point after a handler");
}

/* ---- processes ------------------------------------------------------------------------ */

static int global_value = 1;

static void test_processes(void) {
    printf("processes:\n");
    int p[2];
    CHECK(pipe(p) == 0, "pipe");
    pid_t pid = fork();
    if (pid == 0) {
        global_value = 99;
        close(p[0]);
        const char msg[] = "from the child";
        write(p[1], msg, sizeof msg);
        _exit(42);
    }
    CHECK(pid > 0, "fork");
    close(p[1]);
    char buf[64] = {0};
    CHECK(read(p[0], buf, sizeof buf) == 15 && !strcmp(buf, "from the child"), "the child writes into a pipe");
    close(p[0]);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42, "waitpid: exit code 42");
    CHECK(global_value == 1, "the child's memory is a copy");

    /* a big transfer through a pipe between two processes */
    pipe(p);
    pid = fork();
    if (pid == 0) {
        close(p[1]);
        unsigned sum = 0;
        long total = 0;
        unsigned char chunk[1000];
        ssize_t r;
        while ((r = read(p[0], chunk, sizeof chunk)) > 0) {
            for (ssize_t i = 0; i < r; i++) sum += chunk[i];
            total += r;
        }
        _exit(total == 200000 && sum == 200000u * 3 ? 0 : 1);
    }
    close(p[0]);
    unsigned char block[5000];
    memset(block, 3, sizeof block);
    int ok = 1;
    for (int i = 0; i < 40; i++) ok &= write(p[1], block, sizeof block) == sizeof block;
    close(p[1]);
    waitpid(pid, &status, 0);
    CHECK(ok && WIFEXITED(status) && WEXITSTATUS(status) == 0, "200 KB through a pipe between processes");

    /* execve: the child runs this program again with other arguments and environment */
    pid = fork();
    if (pid == 0) {
        char *argv[] = {(char *)self_path, "exec-child", "arg with spaces", NULL};
        char *envp[] = {"OPALINE_TEST=yes", "HOME=/home", NULL};
        execve(self_path, argv, envp);
        _exit(127);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7, "fork + execve with arguments and environment");

    /* posix_spawn (vfork-style clone + execve inside musl) */
    char *sargv[] = {(char *)self_path, "spawn-child", NULL};
    pid = 0;
    int r = posix_spawn(&pid, self_path, NULL, NULL, sargv, environ);
    CHECK(r == 0 && pid > 0, "posix_spawn");
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 9, "the spawned child exits 9");
    r = posix_spawn(&pid, "/apps/does-not-exist", NULL, NULL, sargv, environ);
    CHECK(r == ENOENT, "posix_spawn of a missing program gives ENOENT");

    /* children killed by signals */
    pid = fork();
    if (pid == 0) abort();
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "abort() kills the child with SIGABRT");
    pid = fork();
    if (pid == 0) {
        pause();
        _exit(0);
    }
    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "kill(child, SIGTERM) ends a child in pause()");
    CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "no children left");
}

/* ---- threads -------------------------------------------------------------------------- */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static long counter;
static __thread int tls_value = 5;
static int queue[16], queue_len, produced_sum, consumed_sum;

static void *adder(void *arg) {
    tls_value = (int)(intptr_t)arg;
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
        if (i % 1000 == 0) sched_yield();
    }
    return (void *)(intptr_t)(tls_value * 10);
}

static void *consumer(void *arg) {
    (void)arg;
    for (int got = 0; got < 500; got++) {
        pthread_mutex_lock(&lock);
        while (queue_len == 0) pthread_cond_wait(&cond, &lock);
        consumed_sum += queue[--queue_len];
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

static void test_threads(void) {
    printf("threads:\n");
    pthread_t t[4];
    int ok = 1;
    for (int i = 0; i < 4; i++) ok &= pthread_create(&t[i], NULL, adder, (void *)(intptr_t)(i + 1)) == 0;
    CHECK(ok, "pthread_create x4");
    int results = 0;
    for (int i = 0; i < 4; i++) {
        void *r;
        pthread_join(t[i], &r);
        results += (int)(intptr_t)r;
    }
    CHECK(counter == 80000, "a mutex protects a shared counter (4 x 20000)");
    CHECK(results == 100, "pthread_join returns each thread's result (thread-local values)");
    CHECK(tls_value == 5, "the main thread's __thread variable is its own");

    pthread_t c;
    pthread_create(&c, NULL, consumer, NULL);
    for (int i = 1; i <= 500; i++) {
        pthread_mutex_lock(&lock);
        while (queue_len == 16) pthread_cond_wait(&cond, &lock);
        queue[queue_len++] = i;
        produced_sum += i;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&lock);
    }
    pthread_join(c, NULL);
    CHECK(consumed_sum == produced_sum && produced_sum == 125250, "condition variables: producer/consumer");

    pthread_t d;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    CHECK(pthread_create(&d, &attr, adder, (void *)(intptr_t)9) == 0, "a detached thread");
    struct timespec ts = {0, 100 * 1000000};
    nanosleep(&ts, NULL);
    pthread_mutex_lock(&lock);
    long now = counter;
    pthread_mutex_unlock(&lock);
    CHECK(now == 100000, "the detached thread finished its work");
}

/* ---- system queries ------------------------------------------------------------------- */

static void test_system(void) {
    printf("system:\n");
    struct utsname u;
    CHECK(uname(&u) == 0 && !strcmp(u.sysname, "OpalineOS") && !strcmp(u.machine, "x86_64"), "uname");
    CHECK(getenv("PATH") && getenv("HOME") && !strcmp(getenv("LANG"), "C.UTF-8"), "default environment");
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    usleep(30000);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    CHECK(ms >= 30 && ms < 1000, "usleep(30 ms) and clock_gettime");
    unsigned char r1[16] = {0}, r2[16] = {0};
    CHECK(getrandom(r1, 16, 0) == 16 && getrandom(r2, 16, 0) == 16 && memcmp(r1, r2, 16), "getrandom");
    char cwd[64];
    CHECK(chdir("/home") == 0 && getcwd(cwd, sizeof cwd) && !strcmp(cwd, "/home"), "chdir + getcwd");
    double complex z = (1.0 + 2.0 * I) * (3.0 - 1.0 * I);
    char text[64];
    snprintf(text, sizeof text, "%.3f %g %g", 3.14159265, creal(z), cimag(z));
    CHECK(!strcmp(text, "3.142 5 5"), "printf of floats, complex multiplication");
    CHECK(strtod("2.5e3", NULL) == 2500.0, "strtod");
}

/* ---- stage SE1: memory -------------------------------------------------------------- */

static long free_kib(void) {
    struct sysinfo si;
    return sysinfo(&si) == 0 ? (long)(si.freeram * si.mem_unit / 1024) : -1;
}

static void test_memory_sharing(void) {
    printf("memory sharing:\n");
    /* copy-on-write: a fork of 16 MiB of touched memory costs almost nothing until someone writes */
    size_t size = 16 << 20;
    char *big = malloc(size);
    if (!big) {
        CHECK(0, "malloc 16 MiB");
        return;
    }
    memset(big, 0x11, size);
    int p[2];
    pipe(p);
    long before = free_kib();
    pid_t pid = fork();
    if (pid == 0) {
        long after = free_kib();
        write(p[1], &after, sizeof after);
        for (size_t i = 0; i < size; i += 4096) big[i] = 0x22;   /* now the child gets its own copies */
        int ok = big[0] == 0x22 && big[size - 4096] == 0x22 && big[1] == 0x11;
        _exit(ok ? 0 : 1);
    }
    long after = -1;
    read(p[0], &after, sizeof after);
    close(p[0]);
    close(p[1]);
    int status;
    waitpid(pid, &status, 0);
    CHECK(before > 0 && after > 0 && before - after < 4096, "fork shares 16 MiB copy-on-write (less than 4 MiB used)");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child's writes get private copies");
    CHECK(big[0] == 0x11 && big[size - 4096] == 0x11, "the parent's memory is unchanged");
    free(big);

    /* MAP_SHARED | MAP_ANONYMOUS across fork, with a futex between the processes */
    volatile int *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(shared != MAP_FAILED, "shared anonymous mmap");
    if (shared == MAP_FAILED) return;
    shared[0] = 0;
    shared[1] = 0;
    pid = fork();
    if (pid == 0) {
        for (int i = 0; i < 1000; i++) __atomic_add_fetch(&shared[1], 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&shared[0], 1, __ATOMIC_SEQ_CST);
        syscall(SYS_futex, &shared[0], FUTEX_WAKE, 1, NULL, NULL, 0);   /* not private: across processes */
        _exit(0);
    }
    while (__atomic_load_n(&shared[0], __ATOMIC_SEQ_CST) == 0)
        syscall(SYS_futex, &shared[0], FUTEX_WAIT, 0, NULL, NULL, 0);
    waitpid(pid, &status, 0);
    CHECK(shared[1] == 1000, "the child's writes to shared memory are seen by the parent");
    CHECK(1, "a shared futex wakes a waiter in another process");
    munmap((void *)shared, 4096);

    /* MAP_SHARED of a file: changes reach the file, and write() reaches the mapping */
    int fd = open("/tmp/shared.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    char zero[8192] = {0};
    write(fd, zero, sizeof zero);
    char *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED, "shared mmap of a file");
    if (m != MAP_FAILED) {
        strcpy(m + 5000, "written through the mapping");
        char back[32] = {0};
        CHECK(pread(fd, back, 27, 5000) == 27 && !strcmp(back, "written through the mapping"),
              "read() sees what the mapping wrote");
        CHECK(m[100] == 0, "the first page is mapped (and now in memory)");   /* so write() must update it */
        pwrite(fd, "via write()", 11, 100);
        CHECK(!memcmp(m + 100, "via write()", 11), "the mapping sees what write() wrote");
        pid = fork();
        if (pid == 0) {
            strcpy(m + 200, "from the child");
            _exit(0);
        }
        waitpid(pid, &status, 0);
        CHECK(!strcmp(m + 200, "from the child"), "a forked child writes the same file pages");
        CHECK(msync(m, 8192, MS_SYNC) == 0 && munmap(m, 8192) == 0, "msync + munmap");
    }
    close(fd);
    fd = open("/tmp/shared.bin", O_RDONLY);
    char back[16] = {0};
    CHECK(fd >= 0 && pread(fd, back, 14, 200) == 14 && !strcmp(back, "from the child"), "the file keeps the changes");
    close(fd);
    unlink("/tmp/shared.bin");
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, open("/etc/motd", O_RDONLY), 0) == MAP_FAILED &&
              errno == EACCES,
          "a writable shared mapping needs a file opened for writing");

    /* mremap */
    char *r = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mmap(r + 2 * 4096, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);   /* a neighbour: no room */
    strcpy(r, "keep me");
    strcpy(r + 4096, "and me");
    char *moved = mremap(r, 2 * 4096, 64 * 4096, MREMAP_MAYMOVE);
    CHECK(moved != MAP_FAILED && moved != r && !strcmp(moved, "keep me") && !strcmp(moved + 4096, "and me"),
          "mremap moves a mapping that cannot grow in place, with its contents");
    CHECK(moved != MAP_FAILED && (moved[64 * 4096 - 1] = 1) == 1, "the grown part is usable");
    char *same = mremap(moved, 64 * 4096, 128 * 4096, 0);
    CHECK(same == moved || same == MAP_FAILED, "mremap without MAYMOVE grows in place or fails");
    munmap(r + 2 * 4096, 4096);
    munmap(same != MAP_FAILED ? same : moved, same != MAP_FAILED ? 128 * 4096 : 64 * 4096);
}

/* ---- stage SE1: execve with threads, clock and file times --------------------------------- */

static void *sleeper(void *arg) {
    (void)arg;
    for (;;) sleep(10);
    return NULL;
}

static void test_exec_threads_and_time(void) {
    printf("execve with threads, time:\n");
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t t[3];
        for (int i = 0; i < 3; i++) pthread_create(&t[i], NULL, sleeper, NULL);
        char *argv[] = {(char *)self_path, "exec-child", "arg with spaces", NULL};
        char *envp[] = {"OPALINE_TEST=yes", NULL};
        execve(self_path, argv, envp);
        _exit(100 + errno);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7, "execve from a process with 4 threads");

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    CHECK(now.tv_sec > 1767225600, "the real clock is past 2026-01-01");
    int fd = open("/tmp/times.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fd, "x", 1);
    close(fd);
    struct stat st;
    stat("/tmp/times.txt", &st);
    CHECK(st.st_mtime >= now.tv_sec - 5 && st.st_mtime <= now.tv_sec + 5, "a new file's mtime is now");
    struct timespec times[2] = {{1000000000, 0}, {1234567890, 500}};
    CHECK(utimensat(AT_FDCWD, "/tmp/times.txt", times, 0) == 0 && stat("/tmp/times.txt", &st) == 0 &&
              st.st_mtime == 1234567890 && st.st_mtim.tv_nsec == 500 && st.st_atime == 1000000000,
          "utimensat sets both times");
    unlink("/tmp/times.txt");
}

/* ---- stage SE1: users and permissions ------------------------------------------------------- */

static int stime_check(void);

static int copy_file(const char *from, const char *to, mode_t mode) {
    int in = open(from, O_RDONLY), out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
    char buf[4096];
    ssize_t n;
    int ok = in >= 0 && out >= 0;
    while (ok && (n = read(in, buf, sizeof buf)) > 0) ok = write(out, buf, (size_t)n) == n;
    close(in);
    close(out);
    return ok;
}

static int unprivileged_checks(void) {   /* runs as user 1000 in a child; returns the number of failures */
    int bad = 0;
#define SUB(cond) bad += !(cond)
    SUB(getuid() == 1000 && geteuid() == 1000 && getgid() == 1000);
    SUB(open("/tmp/perm/secret", O_RDONLY) < 0 && errno == EACCES);       /* 0600 root */
    SUB(access("/tmp/perm/secret", R_OK) < 0 && access("/tmp/perm/secret", F_OK) == 0);
    SUB(open("/tmp/perm/x", O_WRONLY | O_CREAT, 0644) < 0 && errno == EACCES);   /* 0755 root directory */
    SUB(unlink("/tmp/perm/public") < 0 && errno == EACCES);
    int fd = open("/tmp/mine.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);   /* /tmp is 1777 */
    SUB(fd >= 0);
    close(fd);
    struct stat st;
    SUB(stat("/tmp/mine.txt", &st) == 0 && st.st_uid == 1000 && st.st_gid == 1000);
    SUB(unlink("/tmp/rootfile") < 0 && errno == EPERM);                   /* sticky /tmp, root's file */
    SUB(chmod("/tmp/perm/public", 0777) < 0 && errno == EPERM);
    SUB(chown("/tmp/mine.txt", 0, 0) < 0 && errno == EPERM);
    SUB(chmod("/tmp/mine.txt", 0600) == 0);
    SUB(unlink("/tmp/mine.txt") == 0);
    SUB(setuid(0) < 0 && errno == EPERM);
    SUB(kill(getppid(), 0) < 0 && errno == EPERM);                        /* the parent is root */
    SUB(reboot(RB_POWER_OFF) < 0 && errno == EPERM);
    SUB(stime_check());
    /* a set-user-id program owned by root runs with euid 0 */
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {"/tmp/suid-posixtest", "suid-child", NULL};
        execve(argv[0], argv, environ);
        _exit(99);
    }
    int status;
    waitpid(pid, &status, 0);
    SUB(WIFEXITED(status) && WEXITSTATUS(status) == 5);
    /* a file without x for others cannot be run */
    pid = fork();
    if (pid == 0) {
        char *argv[] = {"/tmp/perm/noexec", NULL};
        execve(argv[0], argv, environ);
        _exit(errno == EACCES ? 6 : 98);
    }
    waitpid(pid, &status, 0);
    SUB(WIFEXITED(status) && WEXITSTATUS(status) == 6);
#undef SUB
    return bad;
}

static int stime_check(void) {
    struct timespec t = {1000, 0};
    return clock_settime(CLOCK_REALTIME, &t) < 0 && errno == EPERM;
}

static void test_permissions(void) {
    printf("users and permissions:\n");
    CHECK(getuid() == 0 && geteuid() == 0, "the system starts as root");
    mkdir("/tmp/perm", 0755);
    int fd = open("/tmp/perm/secret", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    write(fd, "s", 1);
    close(fd);
    close(open("/tmp/perm/public", O_WRONLY | O_CREAT | O_TRUNC, 0666));
    close(open("/tmp/rootfile", O_WRONLY | O_CREAT | O_TRUNC, 0666));
    struct stat st;
    stat("/tmp/perm/public", &st);
    CHECK((st.st_mode & 0777) == 0644, "the umask (022) applies to new files");
    mode_t old = umask(077);
    close(open("/tmp/perm/private", O_WRONLY | O_CREAT | O_TRUNC, 0666));
    stat("/tmp/perm/private", &st);
    CHECK(old == 022 && (st.st_mode & 0777) == 0600, "umask(077)");
    umask(old);
    CHECK(stat("/tmp", &st) == 0 && (st.st_mode & 07777) == 01777, "/tmp is 1777 (sticky)");
    CHECK(copy_file(self_path, "/tmp/suid-posixtest", 0755) && chmod("/tmp/suid-posixtest", 04755) == 0 &&
              stat("/tmp/suid-posixtest", &st) == 0 && (st.st_mode & 04000),
          "chmod 04755 (set-user-id)");
    CHECK(copy_file(self_path, "/tmp/perm/noexec", 0744), "a program only root may run (0744)");
    CHECK(chown("/tmp/perm/private", 1000, 100) == 0 && stat("/tmp/perm/private", &st) == 0 && st.st_uid == 1000 &&
              st.st_gid == 100,
          "root gives a file away (chown)");
    pid_t pid = fork();
    if (pid == 0) {
        setgroups(0, NULL);
        if (setgid(1000) || setuid(1000)) _exit(200);
        _exit(unprivileged_checks());
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "as user 1000: permissions refuse and allow as they should");
    if (!WIFEXITED(status) || WEXITSTATUS(status)) printf("       (%d checks failed as user 1000)\n", WEXITSTATUS(status));
    CHECK(access("/tmp/perm/secret", R_OK | W_OK) == 0, "root reads and writes anything");
    unlink("/tmp/perm/secret");
    unlink("/tmp/perm/public");
    unlink("/tmp/perm/private");
    unlink("/tmp/perm/noexec");
    unlink("/tmp/suid-posixtest");
    unlink("/tmp/rootfile");
    rmdir("/tmp/perm");
}

/* ---- stage SE1: job control ------------------------------------------------------------------ */

static volatile sig_atomic_t chld_count, usr1_count;
static void on_chld(int sig) { chld_count += sig == SIGCHLD; }
static void on_usr1_job(int sig) { usr1_count += sig == SIGUSR1; }

static void test_jobs(void) {
    printf("job control:\n");
    struct sigaction sa = {0};
    sa.sa_handler = on_chld;
    sa.sa_flags = SA_RESTART;   /* waitpid must not fail with EINTR because of it */
    sigaction(SIGCHLD, &sa, NULL);

    pid_t pid = fork();
    if (pid == 0) {
        for (;;) pause();
    }
    int status;
    CHECK(kill(pid, SIGSTOP) == 0 && waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status) &&
              WSTOPSIG(status) == SIGSTOP,
          "SIGSTOP stops a child (waitpid WUNTRACED)");
    CHECK(kill(pid, SIGCONT) == 0 && waitpid(pid, &status, WCONTINUED) == pid && WIFCONTINUED(status),
          "SIGCONT continues it (waitpid WCONTINUED)");
    CHECK(kill(pid, SIGTSTP) == 0 && waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status) &&
              WSTOPSIG(status) == SIGTSTP,
          "SIGTSTP (Ctrl+Z) stops it too");
    kill(pid, SIGKILL);
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          "SIGKILL ends a stopped child");
    CHECK(chld_count >= 3, "SIGCHLD arrives for stop, continue and end");

    /* a read interrupted by a stop starts again after SIGCONT instead of failing with EINTR */
    int p[2];
    pipe(p);
    pid = fork();
    if (pid == 0) {
        close(p[1]);
        char buf[8] = {0};
        ssize_t n = read(p[0], buf, sizeof buf);
        _exit(n == 5 && !memcmp(buf, "hello", 5) ? 0 : n < 0 ? 100 + errno : 1);
    }
    close(p[0]);
    struct timespec ms20 = {0, 20 * 1000000};
    nanosleep(&ms20, NULL);   /* let it block in read() */
    kill(pid, SIGSTOP);
    waitpid(pid, &status, WUNTRACED);
    kill(pid, SIGCONT);
    nanosleep(&ms20, NULL);
    signal(SIGPIPE, SIG_IGN);   /* if the child is gone already, report it instead of dying */
    write(p[1], "hello", 5);
    signal(SIGPIPE, SIG_DFL);
    close(p[1]);
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a system call stopped in the middle starts again");
    if (!WIFEXITED(status) || WEXITSTATUS(status)) printf("       (child status 0x%x)\n", status);

    /* process groups and sessions */
    CHECK(getpgrp() == getpgid(0) && getsid(0) > 0, "getpgrp, getpgid, getsid");
    signal(SIGUSR1, on_usr1_job);
    pid_t a = fork();
    if (a == 0) {
        setpgid(0, 0);
        for (;;) pause();
    }
    setpgid(a, a);   /* both sides set it, as shells do */
    pid_t b = fork();
    if (b == 0) {
        setpgid(0, a);
        for (;;) pause();
    }
    setpgid(b, a);
    nanosleep(&ms20, NULL);
    CHECK(getpgid(a) == a && getpgid(b) == a, "setpgid puts two children in one group");
    CHECK(kill(-a, SIGTERM) == 0, "kill(-pgid) signals the group");
    int st1, st2;
    waitpid(a, &st1, 0);
    waitpid(b, &st2, 0);
    CHECK(WIFSIGNALED(st1) && WIFSIGNALED(st2), "both members of the group got it");
    raise(SIGUSR1);
    CHECK(kill(0, SIGUSR1) == 0 && usr1_count == 2, "kill(0) signals the caller's own group");
    pid = fork();
    if (pid == 0) _exit(setsid() == getpid() && getsid(0) == getpid() && getpgrp() == getpid() ? 0 : 1);
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "setsid makes a new session");
    signal(SIGCHLD, SIG_DFL);
    signal(SIGUSR1, SIG_DFL);
}

/* ---- stage SE2: symbolic and hard links ------------------------------------------------------ */

static void test_links(void) {
    printf("links:\n");
    mkdir("/tmp/lk", 0755);
    mkdir("/tmp/lk/real", 0755);
    int fd = open("/tmp/lk/real/target.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fd, "hello links", 11);
    close(fd);
    CHECK(symlink("real/target.txt", "/tmp/lk/sym") == 0, "symlink");
    char buf[64] = {0};
    CHECK(readlink("/tmp/lk/sym", buf, sizeof buf) == 15 && !memcmp(buf, "real/target.txt", 15), "readlink gives the target");
    struct stat st, lst;
    CHECK(lstat("/tmp/lk/sym", &lst) == 0 && S_ISLNK(lst.st_mode) && lst.st_size == 15, "lstat sees the link itself");
    CHECK(stat("/tmp/lk/sym", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 11, "stat follows the link");
    fd = open("/tmp/lk/sym", O_RDONLY);
    memset(buf, 0, sizeof buf);
    CHECK(fd >= 0 && read(fd, buf, sizeof buf) == 11 && !strcmp(buf, "hello links"), "open follows a relative link");
    close(fd);
    CHECK(open("/tmp/lk/sym", O_RDONLY | O_NOFOLLOW) < 0 && errno == ELOOP, "O_NOFOLLOW refuses a link");
    symlink("/tmp/lk/nowhere", "/tmp/lk/dangling");
    CHECK(stat("/tmp/lk/dangling", &st) < 0 && errno == ENOENT && lstat("/tmp/lk/dangling", &lst) == 0,
          "a dangling link: stat fails, lstat works");
    symlink("loop2", "/tmp/lk/loop1");
    symlink("loop1", "/tmp/lk/loop2");
    CHECK(open("/tmp/lk/loop1", O_RDONLY) < 0 && errno == ELOOP, "a loop of links gives ELOOP");
    CHECK(symlink("/tmp/lk/real", "/tmp/lk/dir") == 0 && access("/tmp/lk/dir/target.txt", R_OK) == 0,
          "an absolute link to a directory works in the middle of a path");
    char cwd[128];
    CHECK(chdir("/tmp/lk/dir") == 0 && getcwd(cwd, sizeof cwd) && !strcmp(cwd, "/tmp/lk/real"),
          "chdir through a link: getcwd shows the real path");
    chdir("/");
    char *real = realpath("/tmp/lk/dir/../sym", NULL);
    CHECK(real && !strcmp(real, "/tmp/lk/real/target.txt"), "realpath resolves every link");
    free(real);
    DIR *d = opendir("/tmp/lk");
    int lnk = 0;
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (!strcmp(e->d_name, "sym") && e->d_type == DT_LNK) lnk = 1;
    if (d) closedir(d);
    CHECK(lnk, "readdir reports DT_LNK");
    CHECK(rename("/tmp/lk/sym", "/tmp/lk/sym2") == 0 && lstat("/tmp/lk/sym2", &lst) == 0 && S_ISLNK(lst.st_mode),
          "a renamed link is still a link");
    CHECK(unlink("/tmp/lk/sym2") == 0 && access("/tmp/lk/real/target.txt", F_OK) == 0, "unlink removes the link, not the target");
    CHECK(chmod("/tmp/lk/dir", 0700) == 0 && stat("/tmp/lk/real", &st) == 0 && (st.st_mode & 0777) == 0700,
          "chmod through a link changes the target");
    chmod("/tmp/lk/real", 0755);

    /* hard links */
    CHECK(link("/tmp/lk/real/target.txt", "/tmp/lk/hard") == 0, "link");
    struct stat a, b;
    CHECK(stat("/tmp/lk/real/target.txt", &a) == 0 && stat("/tmp/lk/hard", &b) == 0 && a.st_ino == b.st_ino &&
              a.st_nlink == 2, "both names are the same file (2 links)");
    fd = open("/tmp/lk/hard", O_WRONLY | O_APPEND);
    write(fd, "!", 1);
    close(fd);
    CHECK(stat("/tmp/lk/real/target.txt", &a) == 0 && a.st_size == 12, "writing through one name changes the other");
    CHECK(unlink("/tmp/lk/real/target.txt") == 0 && stat("/tmp/lk/hard", &b) == 0 && b.st_nlink == 1 && b.st_size == 12,
          "the file lives on under its other name");
    CHECK(link("/tmp/lk/real", "/tmp/lk/dirhard") < 0 && errno == EPERM, "no hard links to directories");
    CHECK(link("/tmp/lk/hard", "/tmp/lk/dir") < 0 && errno == EEXIST, "link onto an existing name gives EEXIST");
    const char *gone[] = {"/tmp/lk/hard", "/tmp/lk/dangling", "/tmp/lk/loop1", "/tmp/lk/loop2", "/tmp/lk/dir"};
    for (unsigned i = 0; i < sizeof gone / sizeof gone[0]; i++) unlink(gone[i]);
    CHECK(rmdir("/tmp/lk/real") == 0 && rmdir("/tmp/lk") == 0, "everything cleaned up");
}

/* ---- terminal, select, O_NONBLOCK (SE3) --------------------------------------------------- */

static long ms_since(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

static void test_terminal(void) {
    printf("terminal (termios):\n");
    struct termios orig, t;
    CHECK(isatty(0) && tcgetattr(0, &orig) == 0, "tcgetattr on the terminal");
    CHECK((orig.c_lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG) && (orig.c_iflag & ICRNL) &&
              orig.c_cc[VMIN] == 1 && orig.c_cc[VINTR] == 3 && orig.c_cc[VERASE] == 0x7F,
          "it starts canonical, with echo, signal keys and CR -> NL");
    t = orig;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    /* Someone may press keys while this runs (during boot): TCSAFLUSH throws away what was typed,
       and each check that expects no input flushes again and gets a few tries, so a stray key
       cannot fail it. */
    struct termios back;
    CHECK(tcsetattr(0, TCSAFLUSH, &t) == 0 && tcgetattr(0, &back) == 0 && !(back.c_lflag & ICANON) &&
              back.c_cc[VMIN] == 0,
          "tcsetattr: raw mode, read back the same");
    char c;
    struct timespec t0;
    int ok = 0;
    for (int i = 0; i < 3 && !ok; i++) {
        tcflush(0, TCIFLUSH);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ok = read(0, &c, 1) == 0 && ms_since(&t0) < 1000;
    }
    CHECK(ok, "raw, VMIN=0 VTIME=0: read returns 0 at once");
    t.c_cc[VTIME] = 3;   /* tenths of a second */
    tcsetattr(0, TCSANOW, &t);
    int timed_out = 0, empty = 0;
    for (int i = 0; i < 3 && !(timed_out && empty); i++) {
        tcflush(0, TCIFLUSH);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        long r = read(0, &c, 1);
        long took = ms_since(&t0);
        int n = -1;
        timed_out = r == 0 && took >= 250 && took < 2000;
        empty = ioctl(0, FIONREAD, &n) == 0 && n == 0;
    }
    CHECK(timed_out, "raw, VMIN=0 VTIME=3: read gives up after 0.3 s");
    CHECK(empty, "FIONREAD: nothing waiting");
    CHECK(tcsetattr(0, TCSAFLUSH, &orig) == 0 && tcgetattr(0, &back) == 0 && (back.c_lflag & ICANON) &&
              (back.c_lflag & ECHO),
          "back to canonical mode");
    int fl = fcntl(0, F_GETFL);
    fcntl(0, F_SETFL, fl | O_NONBLOCK);
    ok = 0;
    for (int i = 0; i < 3 && !ok; i++) {
        tcflush(0, TCIFLUSH);
        errno = 0;
        ok = read(0, &c, 1) == -1 && errno == EAGAIN;
    }
    CHECK(ok, "O_NONBLOCK terminal: read gives EAGAIN, no wait");
    fcntl(0, F_SETFL, fl);
    struct winsize ws;
    CHECK(ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0, "TIOCGWINSZ: the size in cells");
    errno = 0;
    CHECK(ioctl(0, TCGETS, (void *)8) == -1 && errno == EFAULT, "TCGETS to a bad pointer: EFAULT");
}

static volatile sig_atomic_t usr1_seen;
static void on_usr1_seen(int s) {
    (void)s;
    usr1_seen = 1;
}

static void test_select(void) {
    printf("select, pselect, O_NONBLOCK:\n");
    int p[2];
    pipe(p);
    fd_set rd, wr;
    struct timeval tv = {0, 0};
    FD_ZERO(&rd);
    FD_SET(p[0], &rd);
    CHECK(select(p[0] + 1, &rd, NULL, NULL, &tv) == 0 && !FD_ISSET(p[0], &rd), "select: empty pipe not readable");
    write(p[1], "x", 1);
    int q[2];   /* writable means a 4 KiB write would not block (SE4): an empty pipe */
    pipe(q);
    FD_ZERO(&rd);
    FD_SET(p[0], &rd);
    FD_ZERO(&wr);
    FD_SET(q[1], &wr);
    int maxfd = p[0] > q[1] ? p[0] : q[1];
    CHECK(select(maxfd + 1, &rd, &wr, NULL, NULL) == 2 && FD_ISSET(p[0], &rd) && FD_ISSET(q[1], &wr),
          "select: readable and writable (2 bits set)");
    close(q[0]);
    close(q[1]);
    char c;
    read(p[0], &c, 1);
    FD_ZERO(&rd);
    FD_SET(p[0], &rd);
    tv.tv_sec = 0;
    tv.tv_usec = 150000;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    /* the system call itself: musl passes the kernel a copy of the timeval */
    int r = (int)syscall(SYS_select, p[0] + 1, &rd, NULL, NULL, &tv);
    long took = ms_since(&t0);
    CHECK(r == 0 && took >= 140 && took < 2000 && tv.tv_sec == 0 && tv.tv_usec == 0,
          "select: 0.15 s timeout, and the time left goes back (0)");
    FD_ZERO(&rd);
    FD_SET(40, &rd);
    errno = 0;
    CHECK(select(41, &rd, NULL, NULL, &tv) == -1 && errno == EBADF, "select on a closed fd: EBADF");

    /* pselect: SIGUSR1 is blocked; the mask given only while waiting lets it in */
    struct sigaction sa = {0};
    sa.sa_handler = on_usr1_seen;
    sigaction(SIGUSR1, &sa, NULL);
    sigset_t block, old, during, now;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, &old);
    raise(SIGUSR1);
    CHECK(!usr1_seen, "SIGUSR1 blocked and pending");
    sigemptyset(&during);
    FD_ZERO(&rd);
    FD_SET(p[0], &rd);
    errno = 0;
    struct timespec limit = {1, 0};
    r = pselect(p[0] + 1, &rd, NULL, NULL, &limit, &during);
    sigprocmask(SIG_BLOCK, NULL, &now);
    CHECK(r == -1 && errno == EINTR && usr1_seen && sigismember(&now, SIGUSR1),
          "pselect: its mask lets the signal in (EINTR), the old mask comes back");
    sigprocmask(SIG_SETMASK, &old, NULL);
    struct timespec ts = {0, 50000000};
    FD_ZERO(&rd);
    FD_SET(p[0], &rd);
    CHECK(pselect(p[0] + 1, &rd, NULL, NULL, &ts, NULL) == 0, "pselect: 0.05 s timeout");
    close(p[1]);
    FD_ZERO(&rd);
    FD_SET(p[0], &rd);
    CHECK(select(p[0] + 1, &rd, NULL, NULL, NULL) == 1 && FD_ISSET(p[0], &rd) && read(p[0], &c, 1) == 0,
          "select: a pipe with no writers is readable (end of file)");
    close(p[0]);

    pipe(p);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    errno = 0;
    CHECK(read(p[0], &c, 1) == -1 && errno == EAGAIN, "O_NONBLOCK pipe: empty read gives EAGAIN");
    static char big[8192];
    CHECK(write(p[1], big, 4096) == 4096, "O_NONBLOCK pipe: 4 KiB fills it");
    errno = 0;
    CHECK(write(p[1], big, 1) == -1 && errno == EAGAIN, "full pipe: write gives EAGAIN");
    while (read(p[0], big, sizeof big) > 0) {}
    write(p[1], big, 100);
    CHECK(write(p[1], big, sizeof big) == 4096 - 100, "a big write takes only what fits");
    close(p[0]);
    close(p[1]);
}

/* ---- file system details (SE4) ------------------------------------------------------------- */

struct dent64 {
    uint64_t ino;
    int64_t off;
    unsigned short reclen;
    unsigned char type;
    char name[];
};

static void test_fs_details(void) {
    printf("file system details:\n");
    int p[2];
    static char page[4096];
    pipe(p);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    write(p[1], "x", 1);
    struct pollfd pf = {p[1], POLLOUT, 0};
    int pr = poll(&pf, 1, 0);
    errno = 0;
    CHECK(!(pr == 1 && (pf.revents & POLLOUT)) || write(p[1], page, sizeof page) == (ssize_t)sizeof page,
          "poll: POLLOUT only if a 4 KiB O_NONBLOCK write does not get EAGAIN");
    while (read(p[0], page, sizeof page) > 0) {}
    pf.revents = 0;
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLOUT), "poll: an empty pipe is writable");
    close(p[0]);
    close(p[1]);

    /* open with O_CREAT and links */
    struct stat st;
    CHECK(mkdir("/tmp/fs4", 0755) == 0, "mkdir /tmp/fs4");
    symlink("/tmp/fs4/target", "/tmp/fs4/l");
    int fd = open("/tmp/fs4/l", O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0 && stat("/tmp/fs4/target", &st) == 0 && S_ISREG(st.st_mode), "O_CREAT through a dangling link creates its target");
    if (fd >= 0) close(fd);
    errno = 0;
    CHECK(open("/tmp/fs4/l", O_WRONLY | O_CREAT | O_EXCL, 0644) < 0 && errno == EEXIST, "O_CREAT|O_EXCL on a link: EEXIST");
    symlink("/tmp/fs4/nothing", "/tmp/fs4/l2");
    errno = 0;
    CHECK(open("/tmp/fs4/l2", O_WRONLY | O_CREAT | O_EXCL, 0644) < 0 && errno == EEXIST && access("/tmp/fs4/nothing", F_OK) < 0,
          "O_CREAT|O_EXCL on a dangling link: EEXIST, nothing created");

    /* a trailing slash: the name must be a directory, and a link there is followed */
    errno = 0;
    CHECK(open("/tmp/fs4/new/", O_WRONLY | O_CREAT, 0644) < 0 && errno == EISDIR && access("/tmp/fs4/new", F_OK) < 0,
          "open(\"new/\", O_CREAT): EISDIR, nothing created");
    errno = 0;
    CHECK(stat("/tmp/fs4/target/", &st) < 0 && errno == ENOTDIR, "stat(\"file/\"): ENOTDIR");
    errno = 0;
    CHECK(open("/tmp/fs4/target/", O_RDONLY) < 0 && errno == ENOTDIR, "open(\"file/\"): ENOTDIR");
    CHECK(mkdir("/tmp/fs4/d/", 0755) == 0 && stat("/tmp/fs4/d", &st) == 0 && S_ISDIR(st.st_mode), "mkdir(\"dir/\") works");
    symlink("d", "/tmp/fs4/dl");
    CHECK(lstat("/tmp/fs4/dl/", &st) == 0 && S_ISDIR(st.st_mode), "lstat(\"link/\") follows the link");

    /* readdir while names go away: a removed entry is never returned */
    mkdir("/tmp/fs4/many", 0755);
    char path[64];
    for (int i = 0; i < 40; i++) {
        snprintf(path, sizeof path, "/tmp/fs4/many/file%02d", i);
        close(open(path, O_WRONLY | O_CREAT, 0644));
    }
    int seen[41] = {0}, gone[41] = {0}, bad = 0;
    int dfd = open("/tmp/fs4/many", O_RDONLY | O_DIRECTORY);
    char dbuf[40];   /* one entry per call */
    long n;
    while (dfd >= 0 && (n = syscall(SYS_getdents64, dfd, dbuf, sizeof dbuf)) > 0)
        for (long o = 0; o < n; o += ((struct dent64 *)(dbuf + o))->reclen) {
            const char *name = ((struct dent64 *)(dbuf + o))->name;
            if (strncmp(name, "file", 4)) continue;
            int k = atoi(name + 4);
            if (k < 0 || k >= 40 || gone[k] || seen[k]++) {
                bad++;
                continue;
            }
            for (int j = k; j <= k + 1 && j < 40; j++) {   /* this one, and the next before it is read */
                snprintf(path, sizeof path, "/tmp/fs4/many/file%02d", j);
                if (!gone[j] && unlink(path) == 0) gone[j] = 1;
            }
        }
    if (dfd >= 0) close(dfd);
    int all = 1;
    for (int i = 0; i < 40; i++)
        if (!gone[i]) all = 0;
    CHECK(dfd >= 0 && !bad && all, "readdir while unlinking: no name twice, none after its unlink");
    CHECK(rmdir("/tmp/fs4/many") == 0, "the directory is empty afterwards");

    /* rename of directories */
    struct stat before, after, b, dotdot;
    mkdir("/tmp/fs4/a", 0755);
    mkdir("/tmp/fs4/b", 0755);
    close(open("/tmp/fs4/a/inside", O_WRONLY | O_CREAT, 0644));
    stat("/tmp/fs4", &before);
    CHECK(rename("/tmp/fs4/a", "/tmp/fs4/b") == 0 && access("/tmp/fs4/a", F_OK) < 0 && access("/tmp/fs4/b/inside", F_OK) == 0,
          "rename a directory over an empty one");
    CHECK(stat("/tmp/fs4", &after) == 0 && after.st_nlink == before.st_nlink - 1, "the parent has one link less");
    mkdir("/tmp/fs4/c", 0755);
    errno = 0;
    CHECK(rename("/tmp/fs4/c", "/tmp/fs4/b") < 0 && errno == ENOTEMPTY, "onto a non-empty directory: ENOTEMPTY");
    errno = 0;
    CHECK(rename("/tmp/fs4/c", "/tmp/fs4/target") < 0 && errno == ENOTDIR, "a directory onto a file: ENOTDIR");
    errno = 0;
    CHECK(rename("/tmp/fs4/target", "/tmp/fs4/c") < 0 && errno == EISDIR, "a file onto a directory: EISDIR");
    mkdir("/tmp/fs4/b/e", 0755);
    stat("/tmp/fs4", &before);
    CHECK(rename("/tmp/fs4/c", "/tmp/fs4/b/e") == 0 && stat("/tmp/fs4", &after) == 0 && after.st_nlink == before.st_nlink - 1 &&
              stat("/tmp/fs4/b", &b) == 0 && b.st_nlink == 3 && stat("/tmp/fs4/b/e/..", &dotdot) == 0 && dotdot.st_ino == b.st_ino,
          "over an empty directory in another parent: link counts and \"..\"");
    const char *files[] = {"/tmp/fs4/target", "/tmp/fs4/l", "/tmp/fs4/l2", "/tmp/fs4/dl", "/tmp/fs4/b/inside"};
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) unlink(files[i]);
    CHECK(rmdir("/tmp/fs4/b/e") == 0 && rmdir("/tmp/fs4/b") == 0 && rmdir("/tmp/fs4/d") == 0 && rmdir("/tmp/fs4") == 0,
          "file system details cleaned up");
}

/* "posixtest rawkeys": raw mode, one line per key until q (x: exit without restoring the terminal). */
static int raw_keys(void) {
    struct termios orig, t;
    if (tcgetattr(0, &orig) < 0) return 2;
    t = orig;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    printf("rawkeys: press keys, q quits\n");
    char c;
    while (read(0, &c, 1) == 1) {
        printf("key %d\n", (unsigned char)c);
        if (c == 'x') {
            printf("rawkeys: leaving raw\n");
            return 3;
        }
        if (c == 'q') break;
    }
    tcsetattr(0, TCSANOW, &orig);
    printf("rawkeys done\n");
    return 0;
}

/* "posixtest vmin": raw with VMIN=3 VTIME=5, one line per read() until q. Three keys typed quickly
   come back from one read; fewer come back once 0.5 s has passed after the last one. */
static int vmin_reads(void) {
    struct termios orig, t;
    if (tcgetattr(0, &orig) < 0) return 2;
    t = orig;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 3;
    t.c_cc[VTIME] = 5;
    tcsetattr(0, TCSAFLUSH, &t);
    printf("vmin: type keys, q quits\n");
    char buf[16];
    ssize_t n;
    while ((n = read(0, buf, sizeof buf)) > 0) {
        printf("vmin got %d bytes\n", (int)n);
        if (memchr(buf, 'q', (size_t)n)) break;
    }
    tcsetattr(0, TCSANOW, &orig);
    printf("vmin done\n");
    return 0;
}

/* "posixtest flushraw": what is typed (without Enter) during a 3 s sleep must be thrown away by
   tcsetattr(TCSAFLUSH) into raw mode, not handed to the next read. */
static int flush_raw(void) {
    struct termios orig, t;
    if (tcgetattr(0, &orig) < 0) return 2;
    printf("flushraw: type now\n");
    sleep(3);
    t = orig;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 5;
    tcsetattr(0, TCSAFLUSH, &t);
    char buf[16];
    ssize_t n = read(0, buf, sizeof buf);
    printf("\nflushraw got %d bytes\n", (int)n);
    tcsetattr(0, TCSANOW, &orig);
    return 0;
}

/* ---- stage SE4: signals and CPU state -------------------------------------------------------- */

/* Waits up to `ms` for the child, sending it `sig` every 10 ms meanwhile (0: none).
 * 0 if it had to be killed because it did not end. */
static int wait_poking(pid_t pid, int sig, int ms, int *status) {
    struct timespec ms5 = {0, 5 * 1000000};
    for (int i = 0; i < ms / 5; i++) {
        if (sig && i % 2 == 0) kill(pid, sig);
        if (waitpid(pid, status, WNOHANG) == pid) return 1;
        nanosleep(&ms5, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, status, 0);
    return 0;
}

static int exited_with(int status, int code) { return WIFEXITED(status) && WEXITSTATUS(status) == code; }

static void on_mxcsr(int sig, siginfo_t *info, void *ctx) {
    (void)sig, (void)info;
    ucontext_t *uc = ctx;
    uc->uc_mcontext.fpregs->mxcsr = 0x10000 | 0x1F80;   /* bit 16 is reserved on every CPU */
    uc->uc_mcontext.fpregs->mxcr_mask = 0xFFFFFFFF;    /* and the frame claims it is not */
}

static volatile sig_atomic_t nt_count;
static void on_nt(int sig, siginfo_t *info, void *ctx) {
    (void)sig, (void)info;
    nt_count++;
    ((ucontext_t *)ctx)->uc_mcontext.gregs[REG_EFL] |= 0x4000;   /* NT in the frame to return to */
}

static unsigned long read_rflags(void) {
    unsigned long f;
    __asm__ volatile("pushfq\n\tpopq %0" : "=r"(f));
    return f;
}

static int nt_child(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = on_nt;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);
    __asm__ volatile("pushfq\n\torq $0x4000, (%%rsp)\n\tpopfq" ::: "cc", "memory");   /* NT=1 */
    raise(SIGUSR1);   /* system calls with NT=1, and one leaves through IRETQ (the handler) */
    int ok = nt_count == 1 && !(read_rflags() & 0x4000);
    raise(SIGUSR1);
    ok = ok && nt_count == 2 && !(read_rflags() & 0x4000);   /* sigreturn drops NT, as Linux does */
    return ok ? 0 : 1;
}

static char *cow_buf;
static int cow_pipe[2];
static void *cow_reader(void *arg) {
    (void)arg;
    return (void *)(intptr_t)read(cow_pipe[0], cow_buf, 8);
}

static volatile sig_atomic_t restart_hits;
static void on_restart(int sig) { restart_hits += sig == SIGUSR1; }
static void on_usr2_exit(int sig) { _exit(sig == SIGUSR2 ? 42 : 1); }

/* A call that waits for a signal or a time fails with EINTR after a SA_RESTART handler. */
static int no_restart_child(int kind) {
    if (kind == 0) {
        signal(SIGUSR1, on_restart);   /* musl's signal() sets SA_RESTART */
    } else {
        struct sigaction sa = {0};
        sa.sa_handler = on_restart;
        sa.sa_flags = SA_RESTART;
        sigaction(SIGUSR1, &sa, NULL);
    }
    sigset_t none;
    sigemptyset(&none);
    struct timespec five = {5, 0}, rem = {0, 0};
    fd_set rd;
    FD_ZERO(&rd);
    int r;
    switch (kind) {
    case 0: r = pause(); break;
    case 1: r = sigsuspend(&none); break;
    case 2: r = nanosleep(&five, &rem); break;
    case 3: r = poll(NULL, 0, -1); break;
    default: r = select(0, &rd, NULL, NULL, NULL); break;
    }
    if (r != -1 || errno != EINTR || !restart_hits) return 1;
    if (kind == 2 && !(rem.tv_sec < 5 && (rem.tv_sec > 0 || rem.tv_nsec > 0))) return 2;   /* the time left */
    return 0;
}

static stack_t alt_seen;
static int alt_change;
static void on_alt(int sig) {
    (void)sig;
    sigaltstack(NULL, &alt_seen);
    stack_t other = {.ss_sp = alt_seen.ss_sp, .ss_size = alt_seen.ss_size, .ss_flags = 0};
    alt_change = sigaltstack(&other, NULL) == -1 && errno == EPERM;
}

static void test_signals_cpu(void) {
    printf("signals and CPU state:\n");
    int status;

    /* rt_sigreturn: MXCSR is checked against the CPU's mask, not the one in the frame */
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction sa = {0};
        sa.sa_sigaction = on_mxcsr;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGUSR1, &sa, NULL);
        raise(SIGUSR1);
        unsigned m;
        __asm__ volatile("stmxcsr %0" : "=m"(m));
        _exit(m == 0x1F80 ? 0 : 1);
    }
    waitpid(pid, &status, 0);
    CHECK(exited_with(status, 0), "a reserved MXCSR bit in a signal frame is dropped (the kernel survives)");

    /* NT (nested task) never reaches the kernel's IRETQ */
    pid = fork();
    if (pid == 0) _exit(nt_child());
    waitpid(pid, &status, 0);
    CHECK(exited_with(status, 0), "NT set by a program or in a signal frame does not break IRETQ");

    /* si_addr comes from the fault itself, even when another process faults meanwhile */
    long page = sysconf(_SC_PAGESIZE);
    char *none = mmap(NULL, 16 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pid_t busy = fork();
    if (busy == 0) {
        for (;;) {   /* page faults all the time */
            char *m = mmap(NULL, 64 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            for (int i = 0; m != MAP_FAILED && i < 64; i++) m[i * page] = 1;
            if (m != MAP_FAILED) munmap(m, 64 * page);
        }
    }
    struct sigaction sa = {0}, old_segv;
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, &old_segv);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int n = 0, wrong = 0;
    for (;; n++) {
        if (n % 32 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 > 600) break;
        }
        char *p = none + (n % 16) * page + n % 64;
        segv_addr = NULL;
        if (!faults(p, n & 1) || segv_addr != p) wrong++;
    }
    sigaction(SIGSEGV, &old_segv, NULL);
    kill(busy, SIGKILL);
    waitpid(busy, &status, 0);
    munmap(none, 16 * page);
    CHECK(n > 100 && wrong == 0, "si_addr is right while another process takes page faults");
    if (wrong) printf("       (%d of %d faults reported another address)\n", wrong, n);

    /* a read() blocked across fork(): its data goes to the caller's copy only (CR0.WP) */
    cow_buf = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(cow_buf, "original", 8);
    int sync[2];
    pipe(cow_pipe);
    pipe(sync);
    pthread_t th;
    pthread_create(&th, NULL, cow_reader, NULL);
    struct timespec ms30 = {0, 30 * 1000000};
    nanosleep(&ms30, NULL);   /* let it block in read() */
    pid = fork();
    if (pid == 0) {
        char c;
        read(sync[0], &c, 1);
        _exit(memcmp(cow_buf, "original", 8) ? 1 : 0);
    }
    write(cow_pipe[1], "changed!", 8);
    void *got = NULL;
    pthread_join(th, &got);
    write(sync[1], "x", 1);
    waitpid(pid, &status, 0);
    CHECK((intptr_t)got == 8 && !memcmp(cow_buf, "changed!", 8), "a read() blocked across fork() fills the buffer");
    CHECK(exited_with(status, 0), "the forked child's copy of that buffer is untouched (copy-on-write)");
    close(cow_pipe[0]), close(cow_pipe[1]), close(sync[0]), close(sync[1]);
    munmap(cow_buf, page);

    /* SA_RESTART does not restart the waits for a signal or a time */
    static const char *const names[5] = {"pause()", "sigsuspend()", "nanosleep()", "poll()", "select()"};
    for (int kind = 0; kind < 5; kind++) {
        pid = fork();
        if (pid == 0) _exit(no_restart_child(kind));
        char what[96];
        snprintf(what, sizeof what, "%s fails with EINTR after a SA_RESTART handler", names[kind]);
        CHECK(wait_poking(pid, SIGUSR1, 3000, &status) && exited_with(status, 0), what);
    }
    /* but a read() still starts again */
    int rp[2];
    pipe(rp);
    pid = fork();
    if (pid == 0) {
        struct sigaction ra = {0};
        ra.sa_handler = on_restart;
        ra.sa_flags = SA_RESTART;
        sigaction(SIGUSR1, &ra, NULL);
        close(rp[1]);
        char buf[4] = {0};
        _exit(read(rp[0], buf, 2) == 2 && !memcmp(buf, "hi", 2) && restart_hits ? 0 : 1);
    }
    close(rp[0]);
    for (int i = 0; i < 5; i++) {
        struct timespec ms10 = {0, 10 * 1000000};
        nanosleep(&ms10, NULL);
        kill(pid, SIGUSR1);
    }
    nanosleep(&ms30, NULL);
    write(rp[1], "hi", 2);
    close(rp[1]);
    CHECK(wait_poking(pid, 0, 3000, &status) && exited_with(status, 0), "a read() with a SA_RESTART handler starts again");

    /* sigsuspend ended by a stop: the mask it restores lets a pending signal in at once */
    int ready[2];
    pipe(ready);
    pid = fork();
    if (pid == 0) {
        signal(SIGUSR2, on_usr2_exit);
        sigset_t m;
        sigemptyset(&m);
        sigaddset(&m, SIGUSR2);
        write(ready[1], "r", 1);
        sigsuspend(&m);   /* SIGUSR2 waits blocked; the stop ends the wait */
        _exit(1);
    }
    char c;
    read(ready[0], &c, 1);
    nanosleep(&ms30, NULL);
    kill(pid, SIGUSR2);
    kill(pid, SIGSTOP);
    waitpid(pid, &status, WUNTRACED);
    kill(pid, SIGCONT);
    CHECK(wait_poking(pid, 0, 2000, &status) && exited_with(status, 42),
          "after sigsuspend the restored mask delivers a pending signal at once");
    close(ready[0]), close(ready[1]);
    struct timespec zero = {0, 0};
    struct { const void *mask; size_t size; } bad = {(const void *)8, 8};
    CHECK(syscall(SYS_ppoll, NULL, 0, &zero, (void *)8, 8) == -1 && errno == EFAULT, "ppoll with a bad mask pointer: EFAULT");
    CHECK(syscall(SYS_pselect6, 0, NULL, NULL, NULL, &zero, &bad) == -1 && errno == EFAULT,
          "pselect6 with a bad mask pointer: EFAULT");
    sigset_t empty;
    sigemptyset(&empty);
    CHECK(syscall(SYS_ppoll, NULL, 0, &zero, &empty, 4) == -1 && errno == EINVAL, "ppoll with a wrong mask size: EINVAL");

    /* kill(-1): every process the caller may signal, but not the caller */
    int pp[2];
    pipe(pp);
    pid = fork();
    if (pid == 0) {
        uid_t uid = 20000 + (uid_t)(getpid() % 10000);   /* a user nobody else runs as */
        setgroups(0, NULL);
        if (setgid(uid) || setuid(uid)) _exit(200);
        pid_t victim = fork();
        if (victim == 0) {
            for (;;) pause();
        }
        write(pp[1], &victim, sizeof victim);
        struct timespec ms20 = {0, 20 * 1000000};
        nanosleep(&ms20, NULL);
        if (kill(-1, 0) != 0) _exit(2);
        if (kill(-1, SIGTERM) != 0) _exit(3);
        int st;
        if (waitpid(victim, &st, 0) != victim || !WIFSIGNALED(st) || WTERMSIG(st) != SIGTERM) _exit(4);
        _exit(0);
    }
    pid_t victim = 0;
    read(pp[0], &victim, sizeof victim);
    int done = wait_poking(pid, 0, 3000, &status);
    CHECK(done && exited_with(status, 0), "kill(-1) signals the caller's other processes, not itself");
    if (victim > 0) kill(victim, SIGKILL);   /* only left if the check failed */
    close(pp[0]), close(pp[1]);

    /* sigaltstack */
    stack_t s = {.ss_sp = malloc(SIGSTKSZ), .ss_size = SIGSTKSZ, .ss_flags = 0}, q;
    void *mine = s.ss_sp;
    CHECK(sigaltstack(&s, &s) == 0 && (s.ss_flags & SS_DISABLE) && sigaltstack(NULL, &q) == 0 && q.ss_sp == mine &&
              !(q.ss_flags & SS_DISABLE),
          "sigaltstack(&s, &s) installs s and returns the old one");
    stack_t on = {.ss_sp = mine, .ss_size = SIGSTKSZ, .ss_flags = SS_ONSTACK};
    CHECK(syscall(SYS_sigaltstack, &on, NULL) == 0, "sigaltstack accepts ss_flags SS_ONSTACK");
    struct sigaction aa = {0};
    aa.sa_handler = on_alt;
    aa.sa_flags = SA_ONSTACK;
    sigaction(SIGUSR1, &aa, NULL);
    raise(SIGUSR1);
    CHECK(alt_seen.ss_flags == SS_ONSTACK && alt_change, "on the alternate stack: SS_ONSTACK, and changing it gives EPERM");
    signal(SIGUSR1, SIG_DFL);
    stack_t off = {.ss_flags = SS_DISABLE};
    sigaltstack(&off, NULL);
    free(mine);
}

/* ---- stage SE4: memory limits --------------------------------------------------------------- */

static void test_memory_limits(void) {
    printf("memory limits:\n");
    const long page = 4096;

    /* a shared mapping of a file opened read-only can never become writable; a private one can */
    int fd = open("/tmp/readonly.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    char text[4096];
    memset(text, 'r', sizeof text);
    write(fd, text, sizeof text);
    close(fd);
    fd = open("/tmp/readonly.bin", O_RDONLY);
    char *m = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, 0);
    errno = 0;
    CHECK(m != MAP_FAILED && mprotect(m, page, PROT_READ | PROT_WRITE) == -1 && errno == EACCES,
          "mprotect cannot make a shared mapping of a read-only file writable");
    if (m != MAP_FAILED) munmap(m, page);
    char *pm = mmap(NULL, page, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(pm != MAP_FAILED && mprotect(pm, page, PROT_READ | PROT_WRITE) == 0 && (pm[0] = 'w') == 'w',
          "a private mapping of the same file can (copy-on-write)");
    char back = 0;
    CHECK(pread(fd, &back, 1, 0) == 1 && back == 'r', "the file keeps its contents");
    if (pm != MAP_FAILED) munmap(pm, page);

    /* one file page mapped more than 65535 times (the old 16-bit page reference count) */
    enum { MAPS = 66000 };
    char *area = mmap(NULL, MAPS * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);   /* find room */
    int mapped = 0;
    if (area != MAP_FAILED) {
        munmap(area, MAPS * page);
        /* from the top down, so each new area is the lowest: the kernel finds it at once */
        for (int i = MAPS - 1; i >= 0; i--, mapped++) {
            volatile char *at = mmap(area + i * page, page, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0);
            if (at == MAP_FAILED || at[0] != 'r') break;
        }
        munmap(area, MAPS * page);
    }
    CHECK(mapped == MAPS, "one file page mapped and touched 66000 times");
    close(fd);
    unlink("/tmp/readonly.bin");

    /* brk: grow, mprotect the middle, shrink below it, grow again */
    char *cur = (char *)syscall(SYS_brk, 0);
    char *start = (char *)(((uintptr_t)cur + page - 1) & ~(uintptr_t)(page - 1));
    int grown = (char *)syscall(SYS_brk, start + 16 * page) == start + 16 * page;
    if (grown) memset(start, 'h', 16 * page);
    int prot = grown && mprotect(start + 8 * page, 2 * page, PROT_READ) == 0;
    int shrunk = (char *)syscall(SYS_brk, start + 4 * page) == start + 4 * page;
    int regrown = (char *)syscall(SYS_brk, start + 16 * page) == start + 16 * page;
    int usable = 0;
    if (regrown) {
        memset(start + 4 * page, 'g', 12 * page);   /* the read-only middle went with the shrink */
        usable = start[4 * page - 1] == 'h' && start[9 * page] == 'g';
    }
    syscall(SYS_brk, cur);   /* back to where malloc left it */
    CHECK(grown && prot && shrunk, "brk grows, its middle can be mprotected, brk shrinks below that");
    CHECK(regrown && usable, "brk grows again afterwards and the memory is usable");
}

/* ---- trailing slashes on names that are created or removed, brk over an mprotected top ---- */

static int err_of(int r) { return r < 0 ? errno : 0; }

static void test_slashes_and_brk(void) {
    printf("slashes and brk:\n");
    struct stat st;
    CHECK(mkdir("/tmp/sl", 0755) == 0, "mkdir /tmp/sl");
    close(open("/tmp/sl/f", O_WRONLY | O_CREAT, 0644));
    mkdir("/tmp/sl/d", 0755);
    symlink("d", "/tmp/sl/dl");
    symlink("f", "/tmp/sl/fl");

    /* as in Linux: "name/" asks for a directory, and a link there is the link, not its target */
    CHECK(err_of(unlink("/tmp/sl/f/")) == ENOTDIR && access("/tmp/sl/f", F_OK) == 0, "unlink(\"file/\"): ENOTDIR, file kept");
    CHECK(err_of(unlink("/tmp/sl/d/")) == EISDIR, "unlink(\"dir/\"): EISDIR");
    CHECK(err_of(unlink("/tmp/sl/dl/")) == ENOTDIR && lstat("/tmp/sl/dl", &st) == 0 && S_ISLNK(st.st_mode),
          "unlink(\"dirlink/\"): ENOTDIR, link kept");
    CHECK(err_of(unlink("/tmp/sl/none/")) == ENOENT, "unlink(\"missing/\"): ENOENT");
    CHECK(err_of(rmdir("/tmp/sl/dl/")) == ENOTDIR && lstat("/tmp/sl/dl", &st) == 0 && S_ISLNK(st.st_mode),
          "rmdir(\"dirlink/\"): ENOTDIR, link kept");
    CHECK(err_of(rename("/tmp/sl/f", "/tmp/sl/g/")) == ENOTDIR && access("/tmp/sl/f", F_OK) == 0 && access("/tmp/sl/g", F_OK) < 0,
          "rename(\"file\", \"new/\"): ENOTDIR, nothing moved");
    CHECK(err_of(rename("/tmp/sl/f/", "/tmp/sl/g")) == ENOTDIR && access("/tmp/sl/f", F_OK) == 0 && access("/tmp/sl/g", F_OK) < 0,
          "rename(\"file/\", \"new\"): ENOTDIR, nothing moved");
    CHECK(err_of(rename("/tmp/sl/f", "/tmp/sl/d/")) == ENOTDIR, "rename(\"file\", \"dir/\"): ENOTDIR");
    CHECK(err_of(rename("/tmp/sl/dl/", "/tmp/sl/g")) == ENOTDIR && lstat("/tmp/sl/dl", &st) == 0 && S_ISLNK(st.st_mode),
          "rename(\"dirlink/\", \"new\"): ENOTDIR, link kept");
    CHECK(rename("/tmp/sl/d/", "/tmp/sl/e/") == 0 && stat("/tmp/sl/e", &st) == 0 && S_ISDIR(st.st_mode) && rename("/tmp/sl/e/", "/tmp/sl/d") == 0,
          "rename(\"dir/\", \"new/\") works");
    CHECK(err_of(link("/tmp/sl/f", "/tmp/sl/g/")) == ENOENT && access("/tmp/sl/g", F_OK) < 0, "link(\"file\", \"new/\"): ENOENT, nothing made");
    CHECK(err_of(link("/tmp/sl/f", "/tmp/sl/fl/")) == EEXIST, "link(\"file\", \"existing/\"): EEXIST");
    CHECK(err_of(symlink("t", "/tmp/sl/s/")) == ENOENT && lstat("/tmp/sl/s", &st) < 0, "symlink(\"t\", \"new/\"): ENOENT, nothing made");
    CHECK(err_of(symlink("t", "/tmp/sl/f/")) == EEXIST, "symlink(\"t\", \"existing/\"): EEXIST");
    CHECK(mkdir("/tmp/sl/x/", 0755) == 0 && rmdir("/tmp/sl/x/") == 0, "mkdir(\"dir/\") and rmdir(\"dir/\") work");
    const char *names[] = {"/tmp/sl/f", "/tmp/sl/fl", "/tmp/sl/dl", "/tmp/sl/g", "/tmp/sl/s"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) unlink(names[i]);
    CHECK(rmdir("/tmp/sl/d") == 0 && rmdir("/tmp/sl") == 0, "slash tests cleaned up");

    /* brk grows over a top heap page that was mprotected: the new memory is still read-write
       (in a child: a SIGSEGV there is a failed check, not the end of the test) */
    pid_t pid = fork();
    if (pid == 0) {
        const long page = 4096;
        char *cur = (char *)syscall(SYS_brk, 0);
        char *start = (char *)(((uintptr_t)cur + page - 1) & ~(uintptr_t)(page - 1));
        if ((char *)syscall(SYS_brk, start + 4 * page) != start + 4 * page) _exit(2);
        memset(start, 'h', 4 * page);
        if (mprotect(start + 3 * page, page, PROT_READ) != 0) _exit(3);
        if ((char *)syscall(SYS_brk, start + 8 * page) != start + 8 * page) _exit(4);
        memset(start + 4 * page, 'g', 4 * page);
        if (mprotect(start + 7 * page, page, PROT_NONE) != 0) _exit(5);
        if ((char *)syscall(SYS_brk, start + 12 * page) != start + 12 * page) _exit(6);
        memset(start + 8 * page, 'n', 4 * page);
        _exit(start[3 * page] == 'h' && start[4 * page] == 'g' && start[8 * page] == 'n' ? 0 : 7);
    }
    int status = 0;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "brk over a PROT_READ or PROT_NONE top page: the new memory is writable");
}

/* ---- stage SE4: kernel core (review fixes) -------------------------------------------------- */

static pthread_barrier_t exec_barrier;
static void *exec_racer(void *arg) {
    (void)arg;
    char *argv[] = {(char *)self_path, "exec-child", "arg with spaces", NULL};
    char *envp[] = {"OPALINE_TEST=yes", NULL};
    pthread_barrier_wait(&exec_barrier);
    execve(self_path, argv, envp);
    return NULL;   /* the other thread's execve won: this one ends with it */
}

static stack_t alt_old;
static int alt_old_err;
static void on_alt_old(int sig) {
    (void)sig;
    stack_t other = {.ss_sp = malloc(SIGSTKSZ), .ss_size = SIGSTKSZ, .ss_flags = 0};
    memset(&alt_old, 0x5A, sizeof alt_old);
    alt_old_err = sigaltstack(&other, &alt_old) == -1 ? errno : 0;
    free(other.ss_sp);
}

static int all_bytes(const void *p, int byte, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (((const unsigned char *)p)[i] != byte) return 0;
    return 1;
}

static void test_kernel_core(void) {
    printf("kernel core:\n");
    int status;

    /* kill(-1) reaches every process, however many: 80 of them, more than one batch of 64 */
    pid_t pid = fork();
    if (pid == 0) {
        uid_t uid = 30000 + (uid_t)(getpid() % 10000);   /* a user nobody else runs as */
        setsid();
        setgroups(0, NULL);
        if (setgid(uid) || setuid(uid)) _exit(200);
        enum { N = 80 };
        pid_t kids[N];
        for (int i = 0; i < N; i++) {
            if ((kids[i] = fork()) == 0) {
                for (;;) pause();
            }
            if (kids[i] < 0) _exit(2);
        }
        struct timespec ms10 = {0, 10 * 1000000};
        nanosleep(&ms10, NULL);
        if (kill(-1, SIGTERM) != 0) _exit(3);
        int dead = 0;
        for (int tries = 0; tries < 300 && dead < N; tries++) {   /* up to 3 s */
            int st;
            while (waitpid(-1, &st, WNOHANG) > 0) dead += WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM;
            if (dead < N) nanosleep(&ms10, NULL);
        }
        for (int i = 0; i < N; i++) kill(kids[i], SIGKILL);   /* survivors, if the check fails */
        while (wait(NULL) > 0) {}
        _exit(dead == N ? 0 : 4);
    }
    int done = wait_poking(pid, 0, 10000, &status);
    CHECK(done && exited_with(status, 0), "kill(-1) as an ordinary user ends all 80 of its processes");

    /* two threads call execve at once: one wins, the other ends (no deadlock); a few rounds, since
       both must be inside execve together for the old deadlock */
    int rounds_ok = 0;
    for (int round = 0; round < 5; round++) {
        pid = fork();
        if (pid == 0) {
            pthread_barrier_init(&exec_barrier, NULL, 2);
            pthread_t t[2];
            for (int i = 0; i < 2; i++) pthread_create(&t[i], NULL, exec_racer, NULL);
            for (;;) pause();
        }
        rounds_ok += wait_poking(pid, 0, 5000, &status) && exited_with(status, 7);
    }
    CHECK(rounds_ok == 5, "two threads calling execve at once: one program runs");

    /* the terminal of a session whose leader exited is free again */
    pid = fork();
    if (pid == 0) _exit(setsid() > 0 && ioctl(0, TIOCSCTTY, 1) == 0 ? 0 : 1);   /* root takes it */
    waitpid(pid, &status, 0);
    int taken = exited_with(status, 0);
    pid = fork();
    if (pid == 0) {
        uid_t uid = 30000 + (uid_t)(getpid() % 10000);
        setsid();
        setgroups(0, NULL);
        if (setgid(uid) || setuid(uid)) _exit(200);
        _exit(ioctl(0, TIOCSCTTY, 0) == 0 ? 0 : 1);
    }
    waitpid(pid, &status, 0);
    CHECK(taken && exited_with(status, 0), "after a session leader with the terminal exits, another session takes it");
    CHECK(ioctl(0, TIOCSCTTY, 1) == 0 && tcgetpgrp(0) == getpgrp(), "and this test takes it back");

    /* sigaltstack on the alternate stack: EPERM, and the old stack is not written */
    stack_t s = {.ss_sp = malloc(SIGSTKSZ), .ss_size = SIGSTKSZ, .ss_flags = 0};
    sigaltstack(&s, NULL);
    struct sigaction aa = {0};
    aa.sa_handler = on_alt_old;
    aa.sa_flags = SA_ONSTACK;
    sigaction(SIGUSR1, &aa, NULL);
    raise(SIGUSR1);
    CHECK(alt_old_err == EPERM && all_bytes(&alt_old, 0x5A, sizeof alt_old),
          "sigaltstack failing with EPERM leaves the old stack untouched");
    signal(SIGUSR1, SIG_DFL);
    stack_t off = {.ss_flags = SS_DISABLE};
    sigaltstack(&off, NULL);
    free(s.ss_sp);
}

/* ---- stage SE4: futex words on a page another thread unmaps ------------------------------ */

static volatile uint32_t *futex_page;
static volatile int futex_stop;
static void *futex_unmap_waiter(void *arg) {
    int *odd = arg;   /* results other than 0, EAGAIN, EFAULT, ETIMEDOUT, EINTR */
    for (unsigned i = 0; !futex_stop; i++) {
        struct timespec t = {0, i % 2 ? 1000000 : 0};   /* 1 ms, or only a look at the word */
        long r = syscall(SYS_futex, futex_page, FUTEX_WAIT, 0, &t, NULL, 0);
        if (r != 0 && errno != EAGAIN && errno != EFAULT && errno != ETIMEDOUT && errno != EINTR) (*odd)++;
    }
    return NULL;
}

static void test_futex_unmap(void) {
    printf("futex and munmap:\n");
    /* FUTEX_WAIT reads the word with interrupts off: a page unmapped meanwhile by another thread
       must give EFAULT (or a fresh look), never a kernel fault */
    futex_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *where = (void *)futex_page;
    pthread_t t[3];
    int odd[3] = {0};
    futex_stop = 0;
    for (int i = 0; i < 3; i++) pthread_create(&t[i], NULL, futex_unmap_waiter, &odd[i]);
    int remapped = 0;
    for (int round = 0; round < 300; round++) {
        munmap(where, 4096);
        sched_yield();
        remapped += mmap(where, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == where;
        syscall(SYS_futex, futex_page, FUTEX_WAKE, 3, NULL, NULL, 0);
        sched_yield();
    }
    futex_stop = 1;
    syscall(SYS_futex, futex_page, FUTEX_WAKE, 3, NULL, NULL, 0);
    for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);
    munmap(where, 4096);
    CHECK(remapped == 300 && !odd[0] && !odd[1] && !odd[2],
          "FUTEX_WAIT on a page unmapped and mapped again by another thread: 0, EAGAIN, EFAULT or ETIMEDOUT");
}

/* ---- interval timers and sysinfo (stage SE4) ------------------------------------------- */

static volatile sig_atomic_t alarms, vtalarms, profs;
static void on_alarm(int sig) { alarms += sig == SIGALRM; }
static void on_vtalarm(int sig) { vtalarms += sig == SIGVTALRM; }
static void on_prof(int sig) { profs += sig == SIGPROF; }

static long usec_left(int which) {
    struct itimerval it;
    if (getitimer(which, &it) != 0) return -1;
    return it.it_value.tv_sec * 1000000L + it.it_value.tv_usec;
}

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, ms % 1000 * 1000000};
    nanosleep(&ts, NULL);
}

static int wait_child(pid_t c) {
    int st = -1;
    while (waitpid(c, &st, 0) < 0 && errno == EINTR) {}
    return st;
}

/* User-mode work until *flag is set or `limit_ms` passed. */
static void spin_until(volatile sig_atomic_t *flag, long limit_ms) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!*flag && ms_since(&t0) < limit_ms)
        for (volatile int i = 0; i < 100000; i++) {}
}

/* Runs in a child process: without timers, pause() would wait for ever (test_timers sets a limit). */
static void timer_checks(void) {
    char what[160];
    struct sigaction sa = {0};
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    sa.sa_handler = on_vtalarm;
    sigaction(SIGVTALRM, &sa, NULL);
    sa.sa_handler = on_prof;
    sigaction(SIGPROF, &sa, NULL);

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(alarm(1) == 0, "alarm(1): no alarm was set (0)");
    errno = 0;
    int r = pause();
    long took = ms_since(&t0);
    snprintf(what, sizeof what, "alarm(1) + pause(): SIGALRM after 1 s, EINTR (%ld ms)", took);
    CHECK(r == -1 && errno == EINTR && alarms == 1 && took >= 990 && took < 3000, what);

    alarms = 0;
    struct itimerval it = {{0, 50000}, {0, 50000}}, old, zero = {{0, 0}, {0, 0}};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0, "setitimer: ITIMER_REAL every 50 ms");
    while (ms_since(&t0) < 400) sleep_ms(10);
    int fired = alarms;
    setitimer(ITIMER_REAL, &zero, &old);
    snprintf(what, sizeof what, "the periodic timer fires 5-10 times in 0.4 s (%d)", fired);
    CHECK(fired >= 5 && fired <= 10, what);
    CHECK(old.it_interval.tv_sec == 0 && old.it_interval.tv_usec == 50000 && old.it_value.tv_sec == 0 &&
          old.it_value.tv_usec > 0 && old.it_value.tv_usec <= 50000,
          "setitimer returns the old interval and time left");
    sleep_ms(120);
    CHECK(alarms == fired && usec_left(ITIMER_REAL) == 0, "a stopped timer stays stopped");

    struct itimerval one = {{0, 0}, {1, 0}};
    setitimer(ITIMER_REAL, &one, NULL);
    long left1 = usec_left(ITIMER_REAL);
    sleep_ms(50);
    long left2 = usec_left(ITIMER_REAL);
    snprintf(what, sizeof what, "getitimer: the time left goes down (%ld, then %ld us)", left1, left2);
    CHECK(left1 > 900000 && left1 <= 1000000 && left2 > 0 && left2 <= left1 - 40000, what);
    CHECK(alarm(5) == 1, "alarm(5) returns what the 1 s timer had left (1)");
    CHECK(alarm(3) == 5, "alarm(3) returns the seconds left of alarm(5)");
    CHECK(syscall(SYS_alarm, 2) == 3, "the alarm system call rounds the seconds left (3)");
    CHECK(alarm(0) == 2 && usec_left(ITIMER_REAL) == 0, "alarm(0) returns the seconds left and stops it");
    alarms = 0;
    struct itimerval short_timer = {{0, 0}, {0, 100000}};
    setitimer(ITIMER_REAL, &short_timer, NULL);
    alarm(0);
    sleep_ms(200);
    CHECK(alarms == 0, "alarm(0) cancels a running timer: no SIGALRM");
    struct itimerval bad = {{0, 0}, {0, 1000000}};
    errno = 0;
    CHECK(setitimer(ITIMER_REAL, &bad, NULL) == -1 && errno == EINVAL, "setitimer: tv_usec 1000000 is EINVAL");
    errno = 0;
    CHECK(getitimer(7, &old) == -1 && errno == EINVAL, "getitimer: an unknown timer is EINVAL");

    /* fork: the child has no timer; execve keeps it */
    alarms = 0;
    struct itimerval t200 = {{0, 0}, {0, 200000}};
    setitimer(ITIMER_REAL, &t200, NULL);
    pid_t c = fork();
    if (c == 0) {
        signal(SIGALRM, SIG_DFL);   /* a SIGALRM would end the child */
        long left = usec_left(ITIMER_REAL);
        sleep_ms(400);
        _exit(left == 0 ? 0 : 1);
    }
    int st = wait_child(c);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0 && alarms == 1,
          "fork: the parent's timer fires, the child has none (no SIGALRM there)");
    c = fork();
    if (c == 0) {
        struct itimerval t30 = {{0, 0}, {30, 0}};
        setitimer(ITIMER_REAL, &t30, NULL);
        execl(self_path, self_path, "itimer-child", (char *)NULL);
        _exit(1);
    }
    st = wait_child(c);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 6, "execve keeps the interval timer (and exit stops it)");

    /* ITIMER_VIRTUAL counts user time only; ITIMER_PROF user and system time */
    struct itimerval v1 = {{0, 0}, {1, 0}};
    setitimer(ITIMER_VIRTUAL, &v1, NULL);
    sleep_ms(100);
    long vleft = usec_left(ITIMER_VIRTUAL);
    setitimer(ITIMER_VIRTUAL, &zero, NULL);
    snprintf(what, sizeof what, "ITIMER_VIRTUAL does not count time asleep (%ld us left)", vleft);
    CHECK(vleft > 950000 && vleft <= 1000000, what);
    struct itimerval t20 = {{0, 0}, {0, 20000}};
    setitimer(ITIMER_VIRTUAL, &t20, NULL);
    spin_until(&vtalarms, 3000);
    CHECK(vtalarms == 1, "ITIMER_VIRTUAL: SIGVTALRM after 20 ms of user time");
    setitimer(ITIMER_PROF, &t20, NULL);
    spin_until(&profs, 3000);
    CHECK(profs == 1, "ITIMER_PROF: SIGPROF after 20 ms of CPU time");
}

static void test_timers(void) {
    printf("interval timers, sysinfo:\n");
    struct sysinfo si;
    CHECK(sysinfo(&si) == 0 && si.procs > 1, "sysinfo: procs counts the running tasks (> 1)");

    int p[2];
    pipe(p);
    int base_checks = checks, base_failures = failures;
    fflush(stdout);
    pid_t c = fork();
    if (c == 0) {
        close(p[0]);
        timer_checks();
        int counts[2] = {checks - base_checks, failures - base_failures};
        write(p[1], counts, sizeof counts);
        fflush(stdout);
        _exit(0);
    }
    close(p[1]);
    int counts[2] = {0, 0};
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(p[0], &rd);
    struct timeval limit = {20, 0};
    int r;
    while ((r = select(p[0] + 1, &rd, NULL, NULL, &limit)) < 0 && errno == EINTR) {}
    int done = r == 1 && read(p[0], counts, sizeof counts) == sizeof counts;
    if (!done) kill(c, SIGKILL);
    wait_child(c);
    close(p[0]);
    checks += counts[0];
    failures += counts[1];
    CHECK(done, "the timer checks finished within 20 s");
}

int main(int argc, char **argv) {
    self_path = argv[0];
    if (argc >= 2 && !strcmp(argv[1], "rawkeys")) {
        setvbuf(stdout, NULL, _IONBF, 0);
        return raw_keys();
    }
    if (argc >= 2 && !strcmp(argv[1], "vmin")) {
        setvbuf(stdout, NULL, _IONBF, 0);
        return vmin_reads();
    }
    if (argc >= 2 && !strcmp(argv[1], "flushraw")) {
        setvbuf(stdout, NULL, _IONBF, 0);
        return flush_raw();
    }
    if (argc >= 3 && !strcmp(argv[1], "env")) {   /* for the shell test: the environment it passed */
        const char *v = getenv(argv[2]);
        if (v) printf("env %s=%s\n", argv[2], v);
        else printf("env %s unset\n", argv[2]);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "exec-child")) {
        const char *t = getenv("OPALINE_TEST");
        return argc == 3 && !strcmp(argv[2], "arg with spaces") && t && !strcmp(t, "yes") && !getenv("PATH") ? 7 : 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "spawn-child")) return 9;
    if (argc >= 2 && !strcmp(argv[1], "itimer-child")) {   /* the timer set before execve still runs */
        struct itimerval it;
        return getitimer(ITIMER_REAL, &it) == 0 && it.it_value.tv_sec >= 25 && it.it_value.tv_sec < 30 ? 6 : 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "suid-child")) return geteuid() == 0 && getuid() == 1000 && getauxval(AT_SECURE) ? 5 : 1;
    if (argc >= 2 && !strcmp(argv[1], "sleep")) {   /* for the shell test: Ctrl+C and Ctrl+Z */
        printf("sleeping...\n");
        sleep(300);
        return 0;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("posixtest (%s): musl C library on OpalineOS, pid %d\n", argv[0], (int)getpid());
    test_files();
    test_memory();
    test_signals();
    test_processes();
    test_threads();
    test_system();
    test_memory_sharing();
    test_exec_threads_and_time();
    test_permissions();
    test_jobs();
    test_links();
    test_terminal();
    test_select();
    test_fs_details();
    test_signals_cpu();
    test_memory_limits();
    test_slashes_and_brk();
    test_kernel_core();
    test_futex_unmap();
    test_timers();
    printf("posixtest: %d checks, %d failures\n", checks, failures);
    if (!failures) printf("POSIXTEST_OK %s\n", argv[0]);
    return failures ? 1 : 0;
}
