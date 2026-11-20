/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * POSIX test for OpalineOS (stage E6): an ordinary C program built against
 * musl. The build makes two copies: /apps/posixtest (dynamically linked,
 * started through /lib/ld-musl-x86_64.so.1) and /apps/posixtest-static.
 *
 * It checks files and directories, malloc, mmap/munmap/mprotect, signals,
 * fork/exec/wait, posix_spawn, pipes between processes, pthreads (mutexes,
 * condition variables, thread-local storage) and a few system queries.
 * Prints "ok"/"FAIL" per check and ends with exit code 0 only if all passed.
 */
#define _GNU_SOURCE
#include <complex.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
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

int main(int argc, char **argv) {
    self_path = argv[0];
    if (argc >= 2 && !strcmp(argv[1], "exec-child")) {
        const char *t = getenv("OPALINE_TEST");
        return argc == 3 && !strcmp(argv[2], "arg with spaces") && t && !strcmp(t, "yes") && !getenv("PATH") ? 7 : 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "spawn-child")) return 9;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("posixtest (%s): musl C library on OpalineOS, pid %d\n", argv[0], (int)getpid());
    test_files();
    test_memory();
    test_signals();
    test_processes();
    test_threads();
    test_system();
    printf("posixtest: %d checks, %d failures\n", checks, failures);
    if (!failures) printf("POSIXTEST_OK %s\n", argv[0]);
    return failures ? 1 : 0;
}
