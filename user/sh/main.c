/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * sh: the first OpalineOS shell (stage E5). A small command interpreter for
 * the terminal; the real Terminal app comes in stage E10.
 *
 * Built-in commands: help, ls [-l] [dir], cat FILE..., echo TEXT, mkdir DIR, rmdir DIR,
 * rm FILE, mv OLD NEW, touch FILE, cd [DIR], pwd, poweroff, reboot, exit [CODE].
 * Leaving the main shell (exit or Ctrl+D) turns the computer off: the kernel does that
 * when the shell it started ends.
 * Line editing (arrows, Home/End, Delete, history with Up/Down) is done by the terminal.
 * Anything else runs a program: a path, or a name looked up in /apps and /system/bin.
 * "> FILE" and ">> FILE" at the end of a line redirect the output. Arguments may be quoted.
 */
#include <opaline/sys.h>

#define LINE_MAX 1024
#define ARGS_MAX 32


static void err(const char *what, const char *arg, long code) {
    dprintf(2, "%s: %s: %s\n", what, arg, strerror_short(code));
}

static int cmd_ls(int argc, char **argv) {
    int longform = argc > 1 && !strcmp(argv[1], "-l");
    const char *dir = argc > 1 + longform ? argv[1 + longform] : ".";
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        err("ls", dir, fd);
        return 1;
    }
    static char buf[4096];
    long n;
    while ((n = getdents64(fd, buf, sizeof buf)) > 0)
        for (long off = 0; off < n;) {
            struct dirent64 *d = (struct dirent64 *)(buf + off);
            off += d->d_reclen;
            if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
            const char *suffix = d->d_type == DT_DIR ? "/" : "";
            if (longform) {
                char path[512];
                snprintf(path, sizeof path, "%s/%s", dir, d->d_name);
                struct stat st;
                long size = stat(path, &st) == 0 ? st.st_size : 0;
                printf("%10ld  %s%s\n", d->d_type == DT_DIR ? 0L : size, d->d_name, suffix);
            } else {
                printf("%s%s\n", d->d_name, suffix);
            }
        }
    close(fd);
    return 0;
}

static int cmd_cat(int argc, char **argv) {
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            err("cat", argv[i], fd);
            status = 1;
            continue;
        }
        static char buf[4096];
        long n;
        while ((n = read(fd, buf, sizeof buf)) > 0) write(1, buf, (size_t)n);
        if (n < 0) {
            err("cat", argv[i], n);
            status = 1;
        }
        close(fd);
    }
    return status;
}

static int simple(const char *name, long r, const char *arg) {
    if (r < 0) {
        err(name, arg, r);
        return 1;
    }
    return 0;
}

static void help(void) {
    printf("Built-in commands:\n"
           "  ls [-l] [dir]   list a directory       cat FILE...    show files\n"
           "  echo TEXT       print text             touch FILE     create an empty file\n"
           "  mkdir DIR       make a directory       rmdir DIR      remove an empty directory\n"
           "  rm FILE         remove a file          mv OLD NEW     rename or move\n"
           "  cd [DIR]        change directory       pwd            show the directory\n"
           "  poweroff        turn the computer off  reboot         restart the computer\n"
           "  fg              continue the stopped program\n"
           "  help            this text              exit [CODE]    leave the shell\n"
           "Leaving the main shell (exit or Ctrl+D) turns the computer off.\n"
           "Arrows, Home/End and Delete edit the line; Up/Down bring back earlier lines.\n"
           "Anything else runs a program from /apps or /system/bin (try: hello, usertest).\n"
           "Ctrl+C ends the running program, Ctrl+Z stops it (fg continues it).\n"
           "End a line with \"> FILE\" or \">> FILE\" to send the output to a file.\n");
}

/* The program stopped with Ctrl+Z, if any (one job at a time for now). */
static long stopped_pid;
static char stopped_name[64];

/* Waits for the foreground job, then takes the terminal back. */
static int wait_job(long pid, const char *name) {
    int status = 0;
    wait4(pid, &status, WUNTRACED);
    tcsetpgrp(0, getpgrp());
    if (WIFSTOPPED(status)) {
        stopped_pid = pid;
        snprintf(stopped_name, sizeof stopped_name, "%s", name);
        dprintf(2, "[%s stopped; type fg to continue it]\n", name);
        return 128 + WSTOPSIG(status);
    }
    if (pid == stopped_pid) stopped_pid = 0;
    if (!WIFEXITED(status)) {
        if (WTERMSIG(status) != SIGINT) dprintf(2, "[%s was stopped by signal %d]\n", name, WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return WEXITSTATUS(status);
}

static int fg(void) {
    if (!stopped_pid) {
        dprintf(2, "fg: no stopped program\n");
        return 1;
    }
    long pid = stopped_pid;
    printf("%s\n", stopped_name);
    tcsetpgrp(0, pid);   /* the job's group has its leader's pid */
    kill(-pid, SIGCONT);
    return wait_job(pid, stopped_name);
}

/* Runs a program and waits for it. */
static int run(int argc, char **argv) {
    (void)argc;
    char path[512];
    if (strchr(argv[0], '/')) {
        snprintf(path, sizeof path, "%s", argv[0]);
    } else {
        struct stat st;
        snprintf(path, sizeof path, "/apps/%s", argv[0]);
        if (stat(path, &st) < 0) snprintf(path, sizeof path, "/system/bin/%s", argv[0]);
    }
    long pid = spawn(path, argv, SPAWN_FOREGROUND);
    if (pid < 0) {
        if (pid == -ENOENT) dprintf(2, "%s: command not found\n", argv[0]);
        else err("sh", argv[0], pid);
        return 127;
    }
    return wait_job(pid, argv[0]);
}

static int execute(int argc, char **argv) {
    const char *c = argv[0];
    if (!strcmp(c, "help")) {
        help();
        return 0;
    }
    if (!strcmp(c, "fg")) return fg();
    if (!strcmp(c, "ls")) return cmd_ls(argc, argv);
    if (!strcmp(c, "cat")) return cmd_cat(argc, argv);
    if (!strcmp(c, "echo")) {
        for (int i = 1; i < argc; i++) printf("%s%s", argv[i], i + 1 < argc ? " " : "");
        printf("\n");
        return 0;
    }
    if (!strcmp(c, "poweroff")) return simple(c, poweroff(), "");
    if (!strcmp(c, "reboot")) return simple(c, reboot(), "");
    if (!strcmp(c, "pwd")) {
        char buf[256];
        if (getcwd(buf, sizeof buf) > 0) printf("%s\n", buf);
        return 0;
    }
    if (!strcmp(c, "cd")) return simple("cd", chdir(argc > 1 ? argv[1] : "/home"), argc > 1 ? argv[1] : "/home");
    if (argc < 2 && (!strcmp(c, "mkdir") || !strcmp(c, "rmdir") || !strcmp(c, "rm") || !strcmp(c, "touch"))) {
        dprintf(2, "%s: missing argument\n", c);
        return 1;
    }
    if (!strcmp(c, "mkdir")) return simple(c, mkdir(argv[1]), argv[1]);
    if (!strcmp(c, "rmdir")) return simple(c, rmdir(argv[1]), argv[1]);
    if (!strcmp(c, "rm")) return simple(c, unlink(argv[1]), argv[1]);
    if (!strcmp(c, "touch")) {
        int fd = open(argv[1], O_WRONLY | O_CREAT);
        if (fd >= 0) close(fd);
        return simple(c, fd, argv[1]);
    }
    if (!strcmp(c, "mv")) {
        if (argc < 3) {
            dprintf(2, "mv: usage: mv OLD NEW\n");
            return 1;
        }
        return simple(c, rename(argv[1], argv[2]), argv[1]);
    }
    return run(argc, argv);
}

/* Splits a line into words; "double quotes" keep spaces. Returns the number of words. */
static int split(char *line, char **argv) {
    int argc = 0;
    char *p = line;
    while (*p && argc < ARGS_MAX - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *out = p;
        argv[argc++] = out;
        int quoted = 0;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') quoted = !quoted;
            else *out++ = *p;
            p++;
        }
        if (*p) p++;
        *out = 0;
    }
    argv[argc] = NULL;
    return argc;
}

int main(void) {
    static char line[LINE_MAX];
    static char motd[1024];
    int fd = open("/etc/motd", O_RDONLY);
    if (fd >= 0) {
        long n = read(fd, motd, sizeof motd - 1);
        if (n > 0) write(1, motd, (size_t)n);
        close(fd);
    }
    printf("Type \"help\" for the list of commands.\n");
    /* the shell itself is not stopped or ended from the keyboard; its programs are (they start
       with the default actions again) */
    static const int job_signals[] = {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU};
    for (unsigned i = 0; i < sizeof job_signals / sizeof job_signals[0]; i++) ignore_signal(job_signals[i]);
    chdir("/home");

    for (;;) {
        char cwd[256];
        if (getcwd(cwd, sizeof cwd) < 0) strcpy(cwd, "?");
        printf("opaline:%s$ ", cwd);
        long n = read(0, line, sizeof line - 1);
        if (n <= 0) {
            printf("\n");
            return 0;   /* Ctrl+D */
        }
        line[n] = 0;
        if (n && line[n - 1] == '\n') line[n - 1] = 0;

        char *argv[ARGS_MAX];
        int argc = split(line, argv);
        if (!argc) continue;
        if (!strcmp(argv[0], "exit")) {
            int code = 0;
            for (const char *s = argc > 1 ? argv[1] : ""; *s >= '0' && *s <= '9'; s++) code = code * 10 + (*s - '0');
            return code;
        }

        /* "> FILE" / ">> FILE" at the end: redirect standard output */
        int saved = -1, out = -1;
        if (argc >= 3 && (!strcmp(argv[argc - 2], ">") || !strcmp(argv[argc - 2], ">>"))) {
            int append = argv[argc - 2][1] == '>';
            out = open(argv[argc - 1], O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
            if (out < 0) {
                err("sh", argv[argc - 1], out);
                continue;
            }
            argc -= 2;
            argv[argc] = NULL;
            saved = dup(1);
            dup2(out, 1);
        }
        if (argc) execute(argc, argv);
        if (saved >= 0) {
            dup2(saved, 1);
            close(saved);
            close(out);
        }
    }
}
