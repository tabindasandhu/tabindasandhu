/*
 * q10_wsl_lite.c - a mini WSL-style command dispatcher.
 *
 * Question 10. wsl_lite accepts its own command vocabulary (wsl-read,
 * wsl-list, wsl-copy, ...) and, rather than shelling out to cat/ls/cp,
 * carries each one out itself by issuing the correct sequence of raw system
 * calls - the same approach as Question 6 - while printing that sequence so
 * the command-to-syscall mapping is visible as you type.
 *
 * This is the same shape as WSL v1's job: take a request expressed in one
 * vocabulary and service it by issuing calls in another, with a dispatch
 * table in the middle deciding which translation applies. See README.md for
 * the full comparison.
 *
 *     ./wsl_lite                          # interactive
 *     ./wsl_lite wsl-read notes.txt       # one-shot
 *     echo 'wsl-list /etc' | ./wsl_lite   # piped
 */
#define _GNU_SOURCE
#include "sc_trace.h"

#include <sys/syscall.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>      /* snprintf only - no stdio I/O in the data path */
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#define BUFSZ     65536
#define MAX_ARGS  8
#define LINESZ    4096

static const char *ANSI_BOLD = "\033[1m";
static const char *ANSI_DIM  = "\033[2m";
static const char *ANSI_RST  = "\033[0m";

/* Small helper: write a NUL-terminated string to a fd with a raw write. */
static void say(int fd, const char *s)
{
    syscall(SYS_write, fd, s, strlen(s));
}

static void sayf(int fd, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void sayf(int fd, const char *fmt, ...)
{
    char buf[LINESZ];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);   /* formats into memory only */
    va_end(ap);

    if (n > 0)
        syscall(SYS_write, fd, buf, (size_t)n);
}

static void complain(const char *cmd, const char *what, int err)
{
    sayf(2, "wsl_lite: %s: %s: %s\n", cmd, what, strerror(err));
}

/* ------------------------------------------------------------- commands */

/* wsl-read FILE - openat, fstat, read/write loop, close. */
static int cmd_read(int argc, char **argv)
{
    char buf[BUFSZ];
    struct stat st;
    long fd, n;
    int rc = 0;

    if (argc != 2) {
        say(2, "usage: wsl-read FILE\n");
        return 2;
    }

    fd = sc_openat(AT_FDCWD, argv[1], O_RDONLY, 0);
    if (fd < 0) {
        complain("wsl-read", argv[1], errno);
        return 1;
    }

    if (sc_fstat((int)fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        complain("wsl-read", argv[1], EISDIR);
        sc_close((int)fd);
        return 1;
    }

    for (;;) {
        n = sc_read((int)fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            complain("wsl-read", argv[1], errno);
            rc = 1;
            break;
        }
        if (n == 0)
            break;
        sc_write(1, buf, (size_t)n);
    }

    sc_close((int)fd);
    return rc;
}

/* wsl-list DIR - openat(O_DIRECTORY), getdents64 loop, close. */
static int cmd_list(int argc, char **argv)
{
    char buf[BUFSZ];
    const char *path = (argc >= 2) ? argv[1] : ".";
    long fd, nread;

    fd = sc_openat(AT_FDCWD, path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        complain("wsl-list", path, errno);
        return 1;
    }

    for (;;) {
        nread = sc_getdents64((int)fd, buf, sizeof buf);
        if (nread < 0) {
            complain("wsl-list", path, errno);
            sc_close((int)fd);
            return 1;
        }
        if (nread == 0)
            break;

        for (long off = 0; off < nread; ) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            char line[NAME_MAX + 32];
            int n;

            off += d->d_reclen;
            if (d->d_name[0] == '.')
                continue;

            n = snprintf(line, sizeof line, "%c %s\n",
                         d->d_type == DT_DIR ? 'd' :
                         d->d_type == DT_LNK ? 'l' : '-', d->d_name);
            sc_write(1, line, (size_t)n);
        }
    }

    sc_close((int)fd);
    return 0;
}

/* wsl-copy SRC DST - openat x2, fstat, read/write loop, close x2. */
static int cmd_copy(int argc, char **argv)
{
    char buf[BUFSZ];
    struct stat st;
    long src, dst, n;
    mode_t mode = 0644;
    int rc = 0;

    if (argc != 3) {
        say(2, "usage: wsl-copy SRC DST\n");
        return 2;
    }

    src = sc_openat(AT_FDCWD, argv[1], O_RDONLY, 0);
    if (src < 0) {
        complain("wsl-copy", argv[1], errno);
        return 1;
    }

    if (sc_fstat((int)src, &st) == 0)
        mode = st.st_mode & 07777;

    dst = sc_openat(AT_FDCWD, argv[2], O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (dst < 0) {
        complain("wsl-copy", argv[2], errno);
        sc_close((int)src);
        return 1;
    }

    for (;;) {
        n = sc_read((int)src, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0) {
                complain("wsl-copy", argv[1], errno);
                rc = 1;
            }
            break;
        }
        if (sc_write((int)dst, buf, (size_t)n) < 0) {
            complain("wsl-copy", argv[2], errno);
            rc = 1;
            break;
        }
    }

    sc_close((int)src);
    sc_close((int)dst);
    return rc;
}

/* wsl-stat FILE - openat, fstat, close. Shows a metadata call translated. */
static int cmd_stat(int argc, char **argv)
{
    struct stat st;
    long fd;

    if (argc != 2) {
        say(2, "usage: wsl-stat FILE\n");
        return 2;
    }

    fd = sc_openat(AT_FDCWD, argv[1], O_RDONLY, 0);
    if (fd < 0) {
        complain("wsl-stat", argv[1], errno);
        return 1;
    }

    if (sc_fstat((int)fd, &st) < 0) {
        complain("wsl-stat", argv[1], errno);
        sc_close((int)fd);
        return 1;
    }

    sayf(1, "  path   : %s\n", argv[1]);
    sayf(1, "  type   : %s\n",
         S_ISDIR(st.st_mode)  ? "directory" :
         S_ISREG(st.st_mode)  ? "regular file" :
         S_ISLNK(st.st_mode)  ? "symlink" : "other");
    sayf(1, "  size   : %lld bytes\n", (long long)st.st_size);
    sayf(1, "  mode   : 0%03o\n", (unsigned)(st.st_mode & 07777));
    sayf(1, "  inode  : %llu\n", (unsigned long long)st.st_ino);
    sayf(1, "  links  : %lu\n", (unsigned long)st.st_nlink);

    sc_close((int)fd);
    return 0;
}

/* wsl-write FILE TEXT... - openat(O_CREAT|O_TRUNC), write, close. */
static int cmd_write(int argc, char **argv)
{
    long fd;
    int rc = 0;

    if (argc < 3) {
        say(2, "usage: wsl-write FILE TEXT...\n");
        return 2;
    }

    fd = sc_openat(AT_FDCWD, argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        complain("wsl-write", argv[1], errno);
        return 1;
    }

    for (int i = 2; i < argc; i++) {
        if (sc_write((int)fd, argv[i], strlen(argv[i])) < 0) { rc = 1; break; }
        sc_write((int)fd, (i + 1 < argc) ? " " : "\n", 1);
    }

    sc_close((int)fd);
    return rc;
}

static int cmd_help(int argc, char **argv);

/* ------------------------------------------------------ dispatch table */

/*
 * The heart of the program. WSL v1 had a table like this too, indexed by
 * Linux syscall number, whose entries pointed at the NT-call sequence that
 * emulated each one. Ours is indexed by command name and its entries point
 * at raw-syscall sequences.
 */
static const struct command {
    const char *name;
    int (*handler)(int argc, char **argv);
    const char *syscalls;
    const char *help;
} commands[] = {
    { "wsl-read",  cmd_read,  "openat, fstat, read*, write*, close",
      "print a file's contents"                                     },
    { "wsl-list",  cmd_list,  "openat(O_DIRECTORY), getdents64*, close",
      "list a directory"                                            },
    { "wsl-copy",  cmd_copy,  "openat x2, fstat, read*, write*, close x2",
      "copy SRC to DST"                                             },
    { "wsl-stat",  cmd_stat,  "openat, fstat, close",
      "show a file's metadata"                                      },
    { "wsl-write", cmd_write, "openat(O_CREAT|O_TRUNC), write*, close",
      "write TEXT into FILE"                                        },
    { "help",      cmd_help,  "-", "show this table"                },
};

#define NCOMMANDS ((int)(sizeof commands / sizeof commands[0]))

static int cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;

    sayf(1, "\n%swsl_lite - commands and the system calls they issue%s\n\n",
         ANSI_BOLD, ANSI_RST);
    sayf(1, "  %-22s %-42s %s\n", "COMMAND", "SYSCALL SEQUENCE", "MEANING");
    sayf(1, "  %-22s %-42s %s\n", "----------------------",
         "------------------------------------------", "-------");
    for (int i = 0; i < NCOMMANDS; i++)
        sayf(1, "  %-22s %-42s %s\n",
             commands[i].name, commands[i].syscalls, commands[i].help);
    sayf(1, "\n  %-22s %s\n", "exit | quit", "leave the dispatcher");
    sayf(1, "\n  (* = issued repeatedly until the operation completes)\n\n");
    return 0;
}

/* --------------------------------------------------------- the dispatcher */

/* Split a line into argv-style tokens, in place. */
static int tokenize(char *line, char **argv, int maxargs)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < maxargs) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t')
            p++;
        if (*p != '\0')
            *p++ = '\0';
    }
    return argc;
}

static int dispatch(int argc, char **argv)
{
    if (argc == 0)
        return 0;

    if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "quit") == 0)
        return -1;

    for (int i = 0; i < NCOMMANDS; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            int rc;

            /* Per-command trace, so the mapping is unambiguous. */
            sc_trace_reset();
            if (sc_trace_enabled())
                sayf(2, "%s--- %s: issuing system calls ---%s\n",
                     ANSI_DIM, commands[i].name, ANSI_RST);

            rc = commands[i].handler(argc, argv);

            if (sc_trace_enabled())
                sayf(2, "%s--- %s: %ld system calls, exit %d ---%s\n",
                     ANSI_DIM, commands[i].name, sc_trace_count(), rc, ANSI_RST);
            return rc;
        }
    }

    sayf(2, "wsl_lite: unknown command '%s' (try 'help')\n", argv[0]);
    return 127;
}

/*
 * Read one line from stdin with a raw read(2).
 *
 * Deliberately NOT routed through sc_read(): these are the dispatcher's own
 * reads of the user's keystrokes, not syscalls issued on behalf of a
 * command. Logging them would bury the command-to-syscall mapping that the
 * whole program exists to show. Reading a byte at a time keeps us from
 * swallowing input past the newline.
 */
static long read_line(char *buf, size_t bufsz)
{
    size_t len = 0;

    while (len < bufsz - 1) {
        char c;
        long n = syscall(SYS_read, 0, &c, (size_t)1);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)                      /* EOF */
            return (len > 0) ? (long)len : -1;
        if (c == '\n')
            break;
        buf[len++] = c;
    }

    buf[len] = '\0';
    return (long)len;
}

int main(int argc, char **argv)
{
    char line[LINESZ];
    char *args[MAX_ARGS];
    int rc = 0;

    sc_trace_init();

    /* One-shot mode: everything after argv[0] is a single command. */
    if (argc > 1) {
        int n = (argc - 1 < MAX_ARGS) ? argc - 1 : MAX_ARGS;
        rc = dispatch(n, &argv[1]);
        return (rc < 0) ? 0 : rc;
    }

    /* Interactive mode. */
    if (isatty(0)) {
        sayf(1, "\n%swsl_lite%s - a mini WSL-style dispatcher over raw syscalls\n",
             ANSI_BOLD, ANSI_RST);
        sayf(1, "Type 'help' for the command table, 'exit' to quit.\n");
    }

    for (;;) {
        if (isatty(0))
            say(1, "wsl_lite> ");

        if (read_line(line, sizeof line) < 0)
            break;                        /* EOF - Ctrl+D */

        int n = tokenize(line, args, MAX_ARGS);
        rc = dispatch(n, args);
        if (rc < 0)
            break;
    }

    if (isatty(0))
        say(1, "\nwsl_lite: goodbye.\n");
    return (rc < 0) ? 0 : rc;
}
