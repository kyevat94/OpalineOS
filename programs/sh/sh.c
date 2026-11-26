/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * sh: the OpalineOS shell. Since stage SE3 an ordinary C program on musl (the E5 shell was
 * built on the minimal user/lib library); the real Terminal app comes in stage E10.
 *
 * Built-in commands: help, ls [-l] [DIR], cat FILE..., echo TEXT, touch FILE, mkdir DIR,
 * rmdir DIR, rm FILE, mv OLD NEW, ln [-s] TARGET NAME, cd [DIR], pwd, export NAME=VALUE...,
 * unset NAME..., env, fg, poweroff, reboot, exit [CODE].
 * Anything else runs a program: a path, or a name looked up in $PATH. The shell forks, the
 * child gets its own process group and the terminal, and execve() passes it the environment.
 * The terminal settings (termios) are saved when a program stops and restored afterwards, so a
 * program that left the terminal in raw mode cannot break the prompt.
 *
 * Words: spaces separate them; "double quotes" keep spaces, 'single quotes' keep everything as
 * typed, \ takes the next character as it is. $NAME, ${NAME} and $? (the last exit code) expand,
 * except inside single quotes. "NAME=VALUE" alone sets a variable; "NAME=VALUE program" sets it
 * for that program only. "> FILE" and ">> FILE" at the end of a line redirect the output.
 * "sh -c LINE" runs one line and exits with its code.
 *
 * Line editing (arrows, Home/End, Delete, history with Up/Down) is done by the terminal.
 * Leaving the main shell (exit or Ctrl+D) turns the computer off: the kernel does that when
 * the shell it started ends.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define SH_LINE 1024
#define ARGS_MAX 64

extern char **environ;

static int interactive;          /* the shell on the terminal (not "sh -c") */
static int last_status;          /* $? */
static struct termios shell_tio; /* the terminal as the shell wants it */
static int have_tio;

/* Short error names, the same in every program of the shell. */
static const char *err_name(int e) {
    switch (e) {
    case ENOENT: return "ENOENT (no such file or directory)";
    case EBADF: return "EBADF (bad file descriptor)";
    case EEXIST: return "EEXIST (already exists)";
    case ENOTDIR: return "ENOTDIR (not a directory)";
    case EISDIR: return "EISDIR (is a directory)";
    case EINVAL: return "EINVAL (invalid argument)";
    case ENOTEMPTY: return "ENOTEMPTY (directory not empty)";
    case ENAMETOOLONG: return "ENAMETOOLONG (name too long)";
    case EFAULT: return "EFAULT (bad address)";
    case ENOSYS: return "ENOSYS (not implemented)";
    case EACCES: return "EACCES (permission denied)";
    case EPERM: return "EPERM (operation not permitted)";
    case ELOOP: return "ELOOP (too many symbolic links)";
    case EXDEV: return "EXDEV (not on the same file system)";
    case ENOEXEC: return "ENOEXEC (not a program)";
    case ENOSPC: return "ENOSPC (no space left)";
    case ENOMEM: return "ENOMEM (out of memory)";
    default: return strerror(e);
    }
}

static void err(const char *what, const char *arg, int e) {
    fflush(stdout);
    fprintf(stderr, "%s: %s: %s\n", what, arg, err_name(e));
}

static int check(const char *what, int r, const char *arg) {
    if (r < 0) {
        err(what, arg, errno);
        return 1;
    }
    return 0;
}

/* ---- built-in commands -------------------------------------------------------------------- */

static int cmd_ls(int argc, char **argv) {
    int longform = argc > 1 && !strcmp(argv[1], "-l");
    const char *dir = argc > 1 + longform ? argv[1 + longform] : ".";
    DIR *d = opendir(dir);
    if (!d) {
        err("ls", dir, errno);
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        const char *suffix = e->d_type == DT_DIR ? "/" : e->d_type == DT_LNK && !longform ? "@" : "";
        if (!longform) {
            printf("%s%s\n", e->d_name, suffix);
            continue;
        }
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        long size = lstat(path, &st) == 0 ? (long)st.st_size : 0;
        if (e->d_type == DT_LNK) {
            char target[512];
            ssize_t n = readlink(path, target, sizeof target - 1);
            target[n > 0 ? n : 0] = 0;
            printf("%10ld  %s -> %s\n", size, e->d_name, target);
        } else {
            printf("%10ld  %s%s\n", e->d_type == DT_DIR ? 0L : size, e->d_name, suffix);
        }
    }
    closedir(d);
    return 0;
}

static int cmd_cat(int argc, char **argv) {
    int status = 0;
    fflush(stdout);
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            err("cat", argv[i], errno);
            status = 1;
            continue;
        }
        static char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0) write(1, buf, (size_t)n);
        if (n < 0) {
            err("cat", argv[i], errno);
            status = 1;
        }
        close(fd);
    }
    return status;
}

static int valid_name(const char *s, size_t len) {
    if (!len || (s[0] >= '0' && s[0] <= '9')) return 0;
    for (size_t i = 0; i < len; i++)
        if (!(s[i] == '_' || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9')))
            return 0;
    return 1;
}

/* "NAME=VALUE": the length of NAME, or 0 if the word is not an assignment. */
static size_t assignment(const char *w) {
    const char *eq = strchr(w, '=');
    return eq && valid_name(w, (size_t)(eq - w)) ? (size_t)(eq - w) : 0;
}

static int set_var(const char *what, const char *w) {
    size_t len = assignment(w);
    if (!len) {
        if (valid_name(w, strlen(w))) return 0;   /* "export NAME": the environment is all there is */
        fflush(stdout);
        fprintf(stderr, "%s: %s: not a valid name\n", what, w);
        return 1;
    }
    char name[256];
    snprintf(name, sizeof name, "%.*s", (int)len, w);
    return check(what, setenv(name, w + len + 1, 1), name);
}

static int cmd_env(void) {
    for (char **e = environ; *e; e++) printf("%s\n", *e);
    return 0;
}

static void help(void) {
    printf("Built-in commands:\n"
           "  ls [-l] [dir]   list a directory       cat FILE...    show files\n"
           "  echo TEXT       print text             touch FILE     create an empty file\n"
           "  mkdir DIR       make a directory       rmdir DIR      remove an empty directory\n"
           "  rm FILE         remove a file          mv OLD NEW     rename or move\n"
           "  ln [-s] T NAME  link (-s: symbolic)    cd [DIR]       change directory\n"
           "  pwd             show the directory     env            show the variables\n"
           "  export N=VALUE  set a variable         unset NAME     remove a variable\n"
           "  poweroff        turn the computer off  reboot         restart the computer\n"
           "  fg              continue the stopped program\n"
           "  help            this text              exit [CODE]    leave the shell\n"
           "Leaving the main shell (exit or Ctrl+D) turns the computer off.\n"
           "Arrows, Home/End and Delete edit the line; Up/Down bring back earlier lines.\n"
           "Anything else runs a program from $PATH (try: hello, posixtest rawkeys).\n"
           "$NAME and $? expand; \"NAME=VALUE program\" sets a variable for one program.\n"
           "Ctrl+C ends the running program, Ctrl+Z stops it (fg continues it).\n"
           "End a line with \"> FILE\" or \">> FILE\" to send the output to a file.\n");
}

/* ---- programs ----------------------------------------------------------------------------- */

/* The program stopped with Ctrl+Z, if any (one job at a time for now), and its terminal settings. */
static pid_t stopped_pid;
static char stopped_name[64];
static struct termios stopped_tio;

/* Waits for the foreground job, then takes the terminal back as the shell left it. */
static int wait_job(pid_t pid, const char *name) {
    int status = 0;
    while (waitpid(pid, &status, WUNTRACED) < 0 && errno == EINTR) {}
    if (interactive) {
        tcsetpgrp(0, getpgrp());
        if (WIFSTOPPED(status)) tcgetattr(0, &stopped_tio);   /* fg gives it back */
        if (have_tio) tcsetattr(0, TCSADRAIN, &shell_tio);
    }
    if (WIFSTOPPED(status)) {
        stopped_pid = pid;
        snprintf(stopped_name, sizeof stopped_name, "%s", name);
        fprintf(stderr, "[%s stopped; type fg to continue it]\n", name);
        return 128 + WSTOPSIG(status);
    }
    if (pid == stopped_pid) stopped_pid = 0;
    if (!WIFEXITED(status)) {
        if (WTERMSIG(status) != SIGINT) fprintf(stderr, "[%s was stopped by signal %d]\n", name, WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return WEXITSTATUS(status);
}

static int fg(void) {
    if (!stopped_pid) {
        fprintf(stderr, "fg: no stopped program\n");
        return 1;
    }
    pid_t pid = stopped_pid;
    printf("%s\n", stopped_name);
    fflush(stdout);
    tcsetattr(0, TCSADRAIN, &stopped_tio);
    tcsetpgrp(0, pid);   /* the job's group has its leader's pid */
    kill(-pid, SIGCONT);
    return wait_job(pid, stopped_name);
}

/* A name with no "/" is looked up in each directory of $PATH. */
static int find_program(const char *name, char *path, size_t size) {
    if (strchr(name, '/')) {
        snprintf(path, size, "%s", name);
        return access(path, F_OK) == 0 ? 0 : -1;
    }
    const char *p = getenv("PATH");
    if (!p) p = "/apps:/system/bin";
    for (;;) {
        const char *end = strchr(p, ':');
        int len = end ? (int)(end - p) : (int)strlen(p);
        snprintf(path, size, "%.*s/%s", len, len ? p : ".", name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) return 0;
        if (!end) return -1;
        p = end + 1;
    }
}

static const int job_signals[] = {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU};

/* Runs a program (with "NAME=VALUE" words just for it) and waits for it. */
static int run(char **argv, char **vars, int nvars) {
    char path[1024];
    if (find_program(argv[0], path, sizeof path) < 0) {
        fflush(stdout);
        fprintf(stderr, "%s: command not found\n", argv[0]);
        return 127;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        err("sh", argv[0], errno);
        return 126;
    }
    if (pid == 0) {
        if (interactive) {
            setpgid(0, 0);            /* a job of its own: the keyboard signals go to it, not to us */
            tcsetpgrp(0, getpid());
        }
        for (unsigned i = 0; i < sizeof job_signals / sizeof job_signals[0]; i++) signal(job_signals[i], SIG_DFL);
        for (int i = 0; i < nvars; i++) set_var("sh", vars[i]);
        execve(path, argv, environ);
        int e = errno;
        err("sh", argv[0], e);
        _exit(e == ENOENT ? 127 : 126);
    }
    if (interactive) {
        setpgid(pid, pid);   /* both sides do it: whichever runs first */
        tcsetpgrp(0, pid);
    }
    return wait_job(pid, argv[0]);
}

static int need_args(int argc, char **argv, int n, const char *usage) {
    if (argc > n) return 0;
    fflush(stdout);
    fprintf(stderr, "%s: usage: %s\n", argv[0], usage);
    return 1;
}

static int execute(int argc, char **argv) {
    /* leading NAME=VALUE words */
    int nvars = 0;
    while (nvars < argc && assignment(argv[nvars])) nvars++;
    if (nvars == argc) {
        int r = 0;
        for (int i = 0; i < nvars; i++) r |= set_var("sh", argv[i]);
        return r;
    }
    char **vars = argv;
    argv += nvars;
    argc -= nvars;

    const char *c = argv[0];
    if (!strcmp(c, "help")) {
        help();
        return 0;
    }
    if (!strcmp(c, "fg")) return fg();
    if (!strcmp(c, "ls")) return cmd_ls(argc, argv);
    if (!strcmp(c, "cat")) return cmd_cat(argc, argv);
    if (!strcmp(c, "env")) return cmd_env();
    if (!strcmp(c, "echo")) {
        for (int i = 1; i < argc; i++) printf("%s%s", argv[i], i + 1 < argc ? " " : "");
        printf("\n");
        return 0;
    }
    if (!strcmp(c, "export")) {
        if (argc == 1) return cmd_env();
        int r = 0;
        for (int i = 1; i < argc; i++) r |= set_var("export", argv[i]);
        return r;
    }
    if (!strcmp(c, "unset")) {
        for (int i = 1; i < argc; i++) unsetenv(argv[i]);
        return 0;
    }
    if (!strcmp(c, "poweroff")) return check(c, reboot(RB_POWER_OFF), "");
    if (!strcmp(c, "reboot")) return check(c, reboot(RB_AUTOBOOT), "");
    if (!strcmp(c, "pwd")) {
        char buf[1024];
        if (!getcwd(buf, sizeof buf)) return check("pwd", -1, ".");
        printf("%s\n", buf);
        return 0;
    }
    if (!strcmp(c, "cd")) {
        const char *dir = argc > 1 ? argv[1] : getenv("HOME") ? getenv("HOME") : "/home";
        if (check("cd", chdir(dir), dir)) return 1;
        char buf[1024];
        if (getcwd(buf, sizeof buf)) setenv("PWD", buf, 1);
        return 0;
    }
    if (!strcmp(c, "mkdir")) return need_args(argc, argv, 1, "mkdir DIR") || check(c, mkdir(argv[1], 0777), argv[1]);
    if (!strcmp(c, "rmdir")) return need_args(argc, argv, 1, "rmdir DIR") || check(c, rmdir(argv[1]), argv[1]);
    if (!strcmp(c, "rm")) return need_args(argc, argv, 1, "rm FILE") || check(c, unlink(argv[1]), argv[1]);
    if (!strcmp(c, "touch")) {
        if (need_args(argc, argv, 1, "touch FILE")) return 1;
        int fd = open(argv[1], O_WRONLY | O_CREAT, 0666);
        if (fd >= 0) close(fd);
        return check(c, fd, argv[1]);
    }
    if (!strcmp(c, "mv")) return need_args(argc, argv, 2, "mv OLD NEW") || check(c, rename(argv[1], argv[2]), argv[1]);
    if (!strcmp(c, "ln")) {
        int sym = argc > 1 && !strcmp(argv[1], "-s");
        if (need_args(argc, argv, 2 + sym, "ln [-s] TARGET NAME")) return 1;
        const char *target = argv[1 + sym], *name = argv[2 + sym];
        return check(c, sym ? symlink(target, name) : link(target, name), name);
    }
    return run(argv, vars, nvars);
}

/* ---- reading a line ----------------------------------------------------------------------- */

/* Appends $NAME, ${NAME} or $? (p points after the '$'). Returns the characters used from p. */
static size_t expand(const char *p, char *out, size_t *n, size_t size) {
    char name[256], num[16];
    const char *value;
    size_t used;
    if (*p == '?') {
        snprintf(num, sizeof num, "%d", last_status);
        value = num;
        used = 1;
    } else {
        int braced = *p == '{';
        const char *s = p + braced;
        size_t len = 0;
        while (valid_name(s, len + 1)) len++;
        if (!len || (braced && s[len] != '}')) {   /* not a variable: a plain '$' */
            if (*n < size - 1) out[(*n)++] = '$';
            return 0;
        }
        snprintf(name, sizeof name, "%.*s", (int)len, s);
        value = getenv(name);
        used = len + 2 * (size_t)braced;
    }
    for (; value && *value && *n < size - 1; value++) out[(*n)++] = *value;
    return used;
}

/* Splits a line into words (see the top of the file). Returns the number of words. */
static int split(const char *p, char **argv, char *buf, size_t size) {
    int argc = 0;
    size_t n = 0;
    while (*p && argc < ARGS_MAX - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        size_t start = n;
        char quote = 0;
        int quoted = 0;   /* "" is a word; an unquoted $NOTHING is not */
        while (*p && (quote || (*p != ' ' && *p != '\t'))) {
            char ch = *p++;
            if (quote == '\'' && ch != '\'') {
                if (n < size - 1) buf[n++] = ch;
            } else if (ch == quote) {
                quote = 0;
            } else if (!quote && (ch == '"' || ch == '\'')) {
                quote = ch;
                quoted = 1;
            } else if (ch == '\\' && *p && (!quote || *p == '"' || *p == '\\' || *p == '$')) {
                if (n < size - 1) buf[n++] = *p;
                p++;
            } else if (ch == '$') {
                p += expand(p, buf, &n, size);
            } else if (n < size - 1) {
                buf[n++] = ch;
            }
        }
        if (n == start && !quoted) continue;
        buf[n++] = 0;
        argv[argc++] = buf + start;
        if (n >= size) break;
    }
    argv[argc] = NULL;
    return argc;
}

/* Runs one line. Returns 1 if it was "exit" (the code is in last_status). */
static int run_line(const char *line) {
    static char words[SH_LINE * 4];
    char *argv[ARGS_MAX];
    int argc = split(line, argv, words, sizeof words);
    if (!argc) return 0;
    if (!strcmp(argv[0], "exit")) {
        if (argc > 1) last_status = atoi(argv[1]);
        return 1;
    }

    /* "> FILE" / ">> FILE" at the end: redirect standard output */
    int saved = -1, out = -1;
    if (argc >= 3 && (!strcmp(argv[argc - 2], ">") || !strcmp(argv[argc - 2], ">>"))) {
        int append = argv[argc - 2][1] == '>';
        /* both descriptors are closed on execve: the program sees only its standard output */
        out = open(argv[argc - 1], O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0666);
        if (out < 0) {
            err("sh", argv[argc - 1], errno);
            last_status = 1;
            return 0;
        }
        argc -= 2;
        argv[argc] = NULL;
        fflush(stdout);
        saved = fcntl(1, F_DUPFD_CLOEXEC, 10);
        dup2(out, 1);
    }
    if (argc) last_status = execute(argc, argv);
    if (saved >= 0) {
        fflush(stdout);
        dup2(saved, 1);
        close(saved);
        close(out);
    }
    return 0;
}

static void on_interrupt(int sig) { (void)sig; }

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "-c")) {
        run_line(argv[2]);
        return last_status;
    }
    interactive = 1;
    static char motd[1024];
    int fd = open("/etc/motd", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, motd, sizeof motd - 1);
        if (n > 0) write(1, motd, (size_t)n);
        close(fd);
    }
    printf("Type \"help\" for the list of commands.\n");
    /* the shell itself is not stopped or ended from the keyboard; its programs are (they start
       with the default actions again) */
    for (unsigned i = 0; i < sizeof job_signals / sizeof job_signals[0]; i++) signal(job_signals[i], SIG_IGN);
    /* except that Ctrl+C at the prompt must interrupt the read (no SA_RESTART), so that the loop
       below prints a fresh prompt; the terminal has already thrown the line away */
    struct sigaction sa = {0};
    sa.sa_handler = on_interrupt;
    sigaction(SIGINT, &sa, NULL);
    if (chdir("/home") == 0) setenv("PWD", "/home", 1);
    have_tio = tcgetattr(0, &shell_tio) == 0;

    static char line[SH_LINE];
    int prompt = 1;
    for (;;) {
        if (prompt) {
            char cwd[1024];
            if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "?");
            printf("opaline:%s$ ", cwd);
            fflush(stdout);
        }
        prompt = 1;
        ssize_t n = read(0, line, sizeof line - 1);
        if (n < 0 && errno == EINTR) continue;   /* Ctrl+C: a fresh prompt */
        if (n < 0) {
            /* EIO: the shell is not the terminal's foreground job (a program took the terminal
               and left it). Take it back and read again; only Ctrl+D ends the shell. */
            tcsetpgrp(0, getpgrp());
            usleep(100000);   /* no busy loop if the error stays */
            prompt = 0;
            continue;
        }
        if (n == 0) {
            printf("\n");
            return 0;   /* Ctrl+D */
        }
        line[n] = 0;
        if (line[n - 1] == '\n') line[n - 1] = 0;
        if (run_line(line)) return last_status;
    }
}
