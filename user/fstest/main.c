/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * File system test, run by the kernel at every boot (stage E5).
 * Exit code 0 = every check passed. It also keeps a counter in
 * /home/persist.txt to show that data survives a reboot.
 */
#include <opaline/sys.h>

static int failures;

#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond)) {                            \
            printf("fstest: FAILED: " __VA_ARGS__); \
            printf("\n");                         \
            failures++;                           \
        }                                         \
    } while (0)

static long read_all(const char *path, char *buf, long size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return fd;
    long total = 0, n;
    while (total < size - 1 && (n = read(fd, buf + total, (size_t)(size - 1 - total))) > 0) total += n;
    buf[total] = 0;
    close(fd);
    return total;
}

static long write_file(const char *path, const void *data, long len, int extra_flags) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | extra_flags);
    if (fd < 0) return fd;
    long n = write(fd, data, (size_t)len);
    close(fd);
    return n;
}

static int contains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncmp(hay, needle, n)) return 1;
    return 0;
}

static int count_entries(const char *dir, const char *must_have) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return fd;
    static char buf[2048];
    int count = 0, found = must_have ? 0 : 1;
    long n;
    while ((n = getdents64(fd, buf, sizeof buf)) > 0)
        for (long off = 0; off < n;) {
            struct dirent64 *d = (struct dirent64 *)(buf + off);
            if (strcmp(d->d_name, ".") && strcmp(d->d_name, "..")) count++;
            if (must_have && !strcmp(d->d_name, must_have)) found = 1;
            off += d->d_reclen;
        }
    close(fd);
    return found ? count : -1;
}

static void test_basic_files(void) {
    static char buf[4096];
    CHECK(read_all("/etc/motd", buf, sizeof buf) > 0 && contains(buf, "OpalineOS"), "read /etc/motd");
    /* a file with an accented name, created on the host by tools/efs3.py */
    CHECK(read_all("/home/Documentos/Ação.txt", buf, sizeof buf) > 0 && contains(buf, "Olá"),
          "read /home/Documentos/Ação.txt");
    CHECK(count_entries("/home/Documentos", "Ação.txt") >= 1, "the accented name appears in getdents64");

    char zeros[16];
    memset(zeros, 0x55, sizeof zeros);
    int z = open("/dev/zero", O_RDONLY);
    CHECK(z >= 0 && read(z, zeros, sizeof zeros) == 16 && zeros[0] == 0 && zeros[15] == 0, "/dev/zero");
    close(z);
    int nul = open("/dev/null", O_WRONLY);
    CHECK(nul >= 0 && write(nul, "x", 1) == 1, "/dev/null");
    close(nul);

    unsigned short ws[4] = {0, 0, 0, 0};
    CHECK(ioctl(1, 0x5413, ws) == 0 && ws[0] > 0 && ws[1] > 0, "terminal size (TIOCGWINSZ)");
}

static void test_create_and_names(void) {
    static char buf[4096];
    unlink("/home/teste-ação/relatório.txt");   /* leftovers of an interrupted run */
    unlink("/home/teste-ação/renomeado.txt");
    rmdir("/home/teste-ação");

    CHECK(mkdir("/home/teste-ação") == 0, "mkdir with an accented name");
    CHECK(mkdir("/home/teste-ação") == -EEXIST, "mkdir of an existing directory gives EEXIST");
    const char text[] = "Relatório de testes do OpalineOS — ação, coração, über, ñandú.\n";
    CHECK(write_file("/home/teste-ação/relatório.txt", text, sizeof text - 1, 0) == (long)sizeof text - 1,
          "write a new file");
    CHECK(read_all("/home/teste-ação/relatório.txt", buf, sizeof buf) == (long)sizeof text - 1 &&
              !strcmp(buf, text), "read it back");
    /* the same name written with a combining accent (o + U+0301) is the same file (NFC) */
    CHECK(read_all("/home/teste-ac\xcc\xa7" "a\xcc\x83" "o/relato\xcc\x81" "rio.txt", buf, sizeof buf) ==
              (long)sizeof text - 1,
          "decomposed (NFD) names find the NFC file");
    struct stat st;
    CHECK(stat("/home/teste-ação/relatório.txt", &st) == 0 && st.st_size == (long)sizeof text - 1 &&
              (st.st_mode & S_IFMT) == S_IFREG, "stat a file");
    CHECK(stat("/home/teste-ação", &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR, "stat a directory");

    CHECK(rename("/home/teste-ação/relatório.txt", "/home/teste-ação/renomeado.txt") == 0, "rename");
    CHECK(open("/home/teste-ação/relatório.txt", O_RDONLY) == -ENOENT, "the old name is gone");
    CHECK(read_all("/home/teste-ação/renomeado.txt", buf, sizeof buf) > 0 && !strcmp(buf, text),
          "the new name has the contents");
    CHECK(count_entries("/home/teste-ação", "renomeado.txt") == 1, "directory listing after rename");

    CHECK(open("/no/such/file", O_RDONLY) == -ENOENT, "missing file gives ENOENT");
    CHECK(rmdir("/home/teste-ação") == -ENOTEMPTY, "rmdir of a non-empty directory gives ENOTEMPTY");
    CHECK(unlink("/home/teste-ação") == -EISDIR, "unlink of a directory gives EISDIR");
    CHECK(open("/etc/motd", O_RDONLY | O_DIRECTORY) == -ENOTDIR, "O_DIRECTORY on a file gives ENOTDIR");
    CHECK(mkdir("/etc/motd/x") == -ENOTDIR, "a file used as a directory gives ENOTDIR");

    CHECK(unlink("/home/teste-ação/renomeado.txt") == 0, "unlink");
    CHECK(rmdir("/home/teste-ação") == 0, "rmdir");
    CHECK(stat("/home/teste-ação", &st) == -ENOENT, "the directory is gone");
}

static void test_big_file(void) {
    static unsigned char data[300 * 1024], back[300 * 1024];   /* past the 12 direct blocks */
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (unsigned char)(i * 7 + i / 4096);
    const char *path = "/home/grande.bin";
    CHECK(write_file(path, data, sizeof data, 0) == (long)sizeof data, "write 300 KiB");
    int fd = open(path, O_RDONLY);
    long n = 0, r;
    while (fd >= 0 && (r = read(fd, back + n, sizeof back - (size_t)n)) > 0) n += r;
    CHECK(n == (long)sizeof data && !memcmp(data, back, sizeof data), "read 300 KiB back");
    CHECK(lseek(fd, 0, SEEK_END) == (long)sizeof data, "lseek SEEK_END");
    CHECK(read(fd, back, 10) == 0, "read at the end of the file returns 0");
    CHECK(lseek(fd, 100000, SEEK_SET) == 100000 && read(fd, back, 4) == 4 && !memcmp(back, data + 100000, 4),
          "lseek SEEK_SET and read");
    close(fd);

    fd = open(path, O_RDWR);
    CHECK(ftruncate(fd, 5000) == 0, "ftruncate to 5000 bytes");
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 5000, "size after truncate");
    close(fd);
    fd = open(path, O_WRONLY | O_APPEND);
    CHECK(write(fd, "FIM", 3) == 3, "append");
    close(fd);
    CHECK(stat(path, &st) == 0 && st.st_size == 5003, "size after append");
    n = 0;
    fd = open(path, O_RDONLY);
    while (fd >= 0 && (r = read(fd, back + n, 6000)) > 0) n += r;
    close(fd);
    CHECK(n == 5003 && !memcmp(back, data, 5000) && !memcmp(back + 5000, "FIM", 3), "contents after truncate+append");
    CHECK(unlink(path) == 0, "remove the big file");
}

static void test_pipes_and_fds(void) {
    int p[2];
    CHECK(pipe(p) == 0, "pipe");
    CHECK(write(p[1], "abc", 3) == 3 && write(p[1], "defg", 4) == 4, "write to a pipe");
    char buf[16];
    CHECK(read(p[0], buf, sizeof buf) == 7 && !memcmp(buf, "abcdefg", 7), "read from a pipe");
    CHECK(lseek(p[0], 0, SEEK_SET) == -ESPIPE, "lseek on a pipe gives ESPIPE");
    close(p[1]);
    CHECK(read(p[0], buf, sizeof buf) == 0, "read after the writer closed gives end of file");
    close(p[0]);
    CHECK(pipe(p) == 0, "second pipe");
    close(p[0]);
    /* such a write also sends SIGPIPE, which ends the process by default: ignore it here */
    long ignore[4] = {1 /* SIG_IGN */, 0, 0, 0};
    CHECK(syscall4(13 /* rt_sigaction */, 13 /* SIGPIPE */, (long)ignore, 0, 8) == 0, "ignore SIGPIPE");
    CHECK(write(p[1], "x", 1) == -EPIPE, "write without a reader gives EPIPE");
    close(p[1]);

    /* dup2: send stdout to a file for a moment */
    int fd = open("/home/saida.txt", O_WRONLY | O_CREAT | O_TRUNC);
    int saved = dup(1);
    CHECK(fd >= 0 && saved >= 0 && dup2(fd, 1) == 1, "dup and dup2");
    printf("redirected line\n");
    dup2(saved, 1);
    close(saved);
    close(fd);
    static char text[64];
    CHECK(read_all("/home/saida.txt", text, sizeof text) > 0 && !strcmp(text, "redirected line\n"),
          "stdout redirected to a file");
    unlink("/home/saida.txt");
    CHECK(close(40) == -EBADF, "close of an unused descriptor gives EBADF");
}

static void test_processes(void) {
    char *argv[] = {"/apps/hello", "um", "dois", NULL};
    long pid = spawn("/apps/hello", argv, 0);
    int status = -1;
    CHECK(pid > 0 && wait4(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "spawn /apps/hello and wait for it");
    char *crash[] = {"/apps/crashnull", NULL};
    pid = spawn("/apps/crashnull", crash, 0);
    CHECK(pid > 0 && wait4(pid, &status, 0) == pid && !WIFEXITED(status) && WTERMSIG(status) == 11,
          "a crashing child ends with signal 11 (SIGSEGV)");
    CHECK(spawn("/apps/no-such-program", argv, 0) == -ENOENT, "spawning a missing program gives ENOENT");
    CHECK(wait4(-1, &status, 0) == -ECHILD, "wait4 without children gives ECHILD");
}

static void test_persistence(void) {
    static char buf[64];
    const char *path = "/home/persist.txt";
    long n = read_all(path, buf, sizeof buf);
    long boot = 1;
    if (n > 0) {
        boot = 0;
        for (char *s = buf; *s >= '0' && *s <= '9'; s++) boot = boot * 10 + (*s - '0');
        printf("persist: found from a previous boot (boot %ld)\n", boot);
        boot++;
    } else {
        printf("persist: first boot of this disk\n");
    }
    char out[32];
    int len = snprintf(out, sizeof out, "%ld\n", boot);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0 && write(fd, out, (size_t)len) == len && fsync(fd) == 0, "write the boot counter");
    close(fd);
}

int main(int argc, char **argv) {
    (void)argc, (void)argv;
    test_basic_files();
    test_create_and_names();
    test_big_file();
    test_pipes_and_fds();
    test_processes();
    test_persistence();
    if (failures == 0) printf("fstest: all checks passed\n");
    return failures;
}
