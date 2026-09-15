/*
 * sc_trace.c - implementation of the raw-syscall logging wrappers.
 *
 * See sc_trace.h for the rationale. The important property of this file is
 * that the ONLY way a byte reaches a file descriptor is:
 *
 *     syscall(SYS_write, fd, buf, len)
 *
 * There is no printf, no fputs, no fwrite anywhere in this program.
 */
#define _GNU_SOURCE
#include "sc_trace.h"

#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>      /* snprintf only - formats into memory, no I/O */
#include <string.h>
#include <stdlib.h>

#define LINE_MAX_LEN 1024

static int  trace_on    = 1;   /* default: show the trace                */
static long trace_count = 0;   /* how many syscalls we have issued       */

void sc_trace_init(void)
{
    /*
     * SC_TRACE=0 silences the trace so the commands can be used as plain
     * utilities (and so their output can be diffed against the real ones
     * without the trace lines getting in the way).
     */
    const char *env = getenv("SC_TRACE");
    if (env != NULL && env[0] == '0' && env[1] == '\0')
        trace_on = 0;
}

int sc_trace_enabled(void) { return trace_on; }

/* ---------------------------------------------------------------- output */

/*
 * The one and only exit point for bytes. Everything else funnels here.
 * We loop because write(2) is allowed to transfer fewer bytes than asked.
 */
static long raw_write(int fd, const void *buf, size_t count)
{
    const char *p = (const char *)buf;
    size_t written = 0;

    while (written < count) {
        long n = syscall(SYS_write, fd, p + written, count - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;           /* interrupted by a signal - retry */
            return -1;
        }
        if (n == 0)
            break;                  /* should not happen for regular fds */
        written += (size_t)n;
    }
    return (long)written;
}

void sc_emit(const char *text)
{
    if (!trace_on)
        return;
    raw_write(2, text, strlen(text));
}

const char *sc_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return (slash != NULL && slash[1] != '\0') ? slash + 1 : path;
}

long sc_write(int fd, const void *buf, size_t count)
{
    char args[256], line[LINE_MAX_LEN];
    long ret;
    int saved;

    ret   = raw_write(fd, buf, count);
    saved = errno;
    trace_count++;

    if (trace_on) {
        snprintf(args, sizeof args, "write(%d, %p, %zu)", fd, buf, count);
        snprintf(line, sizeof line, "[syscall] %-48s = %ld\n", args, ret);
        raw_write(2, line, strlen(line));
    }

    errno = saved;
    return ret;
}

/* ------------------------------------------------------- pretty printing */

/* Symbolic names for the errno values these utilities can realistically hit. */
static const char *errno_name(int e)
{
    switch (e) {
    case EACCES:       return "EACCES";
    case ENOENT:       return "ENOENT";
    case EISDIR:       return "EISDIR";
    case ENOTDIR:      return "ENOTDIR";
    case EPERM:        return "EPERM";
    case EBADF:        return "EBADF";
    case EEXIST:       return "EEXIST";
    case ELOOP:        return "ELOOP";
    case ENAMETOOLONG: return "ENAMETOOLONG";
    case ENOSPC:       return "ENOSPC";
    case EMFILE:       return "EMFILE";
    case EINVAL:       return "EINVAL";
    case EFAULT:       return "EFAULT";
    case ENOMEM:       return "ENOMEM";
    case EINTR:        return "EINTR";
    case ESPIPE:       return "ESPIPE";
    default:           return NULL;
    }
}

/* Render an open(2) flags word the way strace does: O_RDONLY|O_DIRECTORY */
static void flags_to_text(int flags, char *out, size_t outsz)
{
    size_t used = 0;
    int accmode = flags & O_ACCMODE;

    /* The access mode is not a bit flag - it is a 2-bit field. */
    const char *acc = (accmode == O_RDONLY) ? "O_RDONLY"
                    : (accmode == O_WRONLY) ? "O_WRONLY"
                    : (accmode == O_RDWR)   ? "O_RDWR"
                    : "O_?";
    used = (size_t)snprintf(out, outsz, "%s", acc);

    /* Now the real bit flags, appended with '|' like strace prints them. */
    struct { int bit; const char *name; } table[] = {
        { O_CREAT,     "O_CREAT"     },
        { O_TRUNC,     "O_TRUNC"     },
        { O_APPEND,    "O_APPEND"    },
        { O_EXCL,      "O_EXCL"      },
        { O_DIRECTORY, "O_DIRECTORY" },
        { O_NOFOLLOW,  "O_NOFOLLOW"  },
        { O_CLOEXEC,   "O_CLOEXEC"   },
        { O_NONBLOCK,  "O_NONBLOCK"  },
    };

    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if ((flags & table[i].bit) && used < outsz - 1)
            used += (size_t)snprintf(out + used, outsz - used, "|%s", table[i].name);
    }
}

/*
 * Print one trace line: the call with its raw arguments, then its return
 * value - or "-1 ENOENT (No such file or directory)" on failure, exactly
 * the way strace reports it.
 */
static void trace_line(const char *args, long ret, int saved_errno)
{
    char line[LINE_MAX_LEN];

    trace_count++;
    if (!trace_on)
        return;

    if (ret < 0) {
        const char *name = errno_name(saved_errno);
        if (name != NULL)
            snprintf(line, sizeof line, "[syscall] %-48s = -1 %s (%s)\n",
                     args, name, strerror(saved_errno));
        else
            snprintf(line, sizeof line, "[syscall] %-48s = -1 errno=%d (%s)\n",
                     args, saved_errno, strerror(saved_errno));
    } else {
        snprintf(line, sizeof line, "[syscall] %-48s = %ld\n", args, ret);
    }
    raw_write(2, line, strlen(line));
}

/* ------------------------------------------------------ syscall wrappers */

long sc_openat(int dirfd, const char *path, int flags, mode_t mode)
{
    char flagtext[128], args[512];
    long ret;
    int saved;

    ret   = syscall(SYS_openat, dirfd, path, flags, mode);
    saved = errno;

    flags_to_text(flags, flagtext, sizeof flagtext);
    /* strace prints the mode argument only when O_CREAT makes it meaningful. */
    if (flags & O_CREAT)
        snprintf(args, sizeof args, "openat(%s, \"%s\", %s, 0%03o)",
                 (dirfd == AT_FDCWD) ? "AT_FDCWD" : "fd",
                 path, flagtext, (unsigned)mode);
    else
        snprintf(args, sizeof args, "openat(%s, \"%s\", %s)",
                 (dirfd == AT_FDCWD) ? "AT_FDCWD" : "fd", path, flagtext);
    trace_line(args, ret, saved);

    errno = saved;
    return ret;
}

long sc_read(int fd, void *buf, size_t count)
{
    char args[256];
    long ret;
    int saved;

    ret   = syscall(SYS_read, fd, buf, count);
    saved = errno;

    snprintf(args, sizeof args, "read(%d, %p, %zu)", fd, buf, count);
    trace_line(args, ret, saved);

    errno = saved;
    return ret;
}

long sc_close(int fd)
{
    char args[64];
    long ret;
    int saved;

    ret   = syscall(SYS_close, fd);
    saved = errno;

    snprintf(args, sizeof args, "close(%d)", fd);
    trace_line(args, ret, saved);

    errno = saved;
    return ret;
}

long sc_fstat(int fd, struct stat *st)
{
    char args[256];
    long ret;
    int saved;

    /*
     * On x86-64 the kernel's struct stat and glibc's struct stat have the
     * same layout, so passing glibc's struct straight to the raw syscall
     * is safe here. (This is an architecture-specific assumption.)
     */
    ret   = syscall(SYS_fstat, fd, st);
    saved = errno;

    if (ret == 0)
        snprintf(args, sizeof args, "fstat(%d, {mode=0%o, size=%lld})",
                 fd, (unsigned)st->st_mode, (long long)st->st_size);
    else
        snprintf(args, sizeof args, "fstat(%d, %p)", fd, (void *)st);
    trace_line(args, ret, saved);

    errno = saved;
    return ret;
}

long sc_getdents64(int fd, void *buf, size_t count)
{
    char args[256];
    long ret;
    int saved;

    ret   = syscall(SYS_getdents64, fd, buf, count);
    saved = errno;

    snprintf(args, sizeof args, "getdents64(%d, %p, %zu)", fd, buf, count);
    trace_line(args, ret, saved);

    errno = saved;
    return ret;
}

long sc_lseek(int fd, off_t offset, int whence)
{
    char args[128];
    long ret;
    int saved;

    ret   = syscall(SYS_lseek, fd, offset, whence);
    saved = errno;

    snprintf(args, sizeof args, "lseek(%d, %lld, %d)",
             fd, (long long)offset, whence);
    trace_line(args, ret, saved);

    errno = saved;
    return ret;
}

void sc_trace_reset(void) { trace_count = 0; }

long sc_trace_count(void) { return trace_count; }

void sc_trace_summary(void)
{
    char line[128];

    if (!trace_on)
        return;
    snprintf(line, sizeof line,
             "[syscall] ---- %ld system calls issued ----\n", trace_count);
    raw_write(2, line, strlen(line));
}
