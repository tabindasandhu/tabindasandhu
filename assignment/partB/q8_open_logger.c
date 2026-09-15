/*
 * q8_open_logger.c - an LD_PRELOAD shim that logs every file a program opens.
 *
 * Question 8. Built as a shared library and injected ahead of libc:
 *
 *     gcc -shared -fPIC -o libopenlog.so q8_open_logger.c -ldl
 *     LD_PRELOAD=./libopenlog.so cat notes.txt
 *     cat /tmp/open_trace.log
 *
 * The dynamic linker resolves a symbol to the FIRST object that defines it.
 * Because LD_PRELOAD puts this library ahead of libc in the search order,
 * every call the target makes to open()/openat() lands here instead. We log
 * it, then hand it on to the genuine libc implementation, which we look up
 * with dlsym(RTLD_NEXT, ...) - "the next definition after me".
 *
 * Environment:
 *     OPEN_LOG_FILE   where to append the log (default /tmp/open_trace.log)
 *     OPEN_LOG_QUIET  set to 1 to suppress the banner on stderr
 *
 * Two things make this shim safe that a naive version gets wrong:
 *
 *   1. The log is written with RAW SYSCALLS, never with fopen/fprintf. If
 *      logging itself called open(), it would resolve straight back into
 *      this file and recurse forever.
 *   2. A re-entrancy guard, because libc helpers we call (localtime_r
 *      reading /etc/localtime, for instance) may themselves open files.
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

/* Real libc entry points, resolved lazily on first use. */
static int    (*real_open)(const char *, int, ...);
static int    (*real_open64)(const char *, int, ...);
static int    (*real_openat)(int, const char *, int, ...);
static FILE  *(*real_fopen)(const char *, const char *);

/*
 * Re-entrancy guard. __thread gives each thread its own copy, so a
 * multi-threaded target cannot have one thread's guard block another's
 * legitimate logging.
 */
static __thread int in_hook = 0;

static int   log_fd = -1;
static int   banner_done = 0;

/* --------------------------------------------------------------- logging */

/* Raw write(2) - deliberately not fwrite, see the header comment. */
static void raw_write(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        long n = syscall(SYS_write, fd, buf + off, len - off);
        if (n <= 0)
            return;
        off += (size_t)n;
    }
}

/* Raw openat(2) for the log file itself. */
static void open_log_file(void)
{
    const char *path = getenv("OPEN_LOG_FILE");

    if (path == NULL || path[0] == '\0')
        path = "/tmp/open_trace.log";

    /*
     * O_APPEND means every write lands at the current end of file, atomically
     * for reasonably sized records - so several preloaded processes can share
     * one log without shredding each other's lines.
     */
    log_fd = (int)syscall(SYS_openat, AT_FDCWD, path,
                          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

    if (log_fd >= 0 && !banner_done) {
        banner_done = 1;
        if (getenv("OPEN_LOG_QUIET") == NULL) {
            char msg[512];
            int n = snprintf(msg, sizeof msg,
                             "[openlog] pid %d preloaded, logging to %s\n",
                             (int)getpid(), path);
            raw_write(2, msg, (size_t)n);
        }
    }
}

/* Decode an open(2) flags word into O_RDONLY|O_CLOEXEC form. */
static void flags_text(int flags, char *out, size_t outsz)
{
    static const struct { int bit; const char *name; } table[] = {
        { O_CREAT,     "O_CREAT"     },
        { O_EXCL,      "O_EXCL"      },
        { O_NOCTTY,    "O_NOCTTY"    },
        { O_TRUNC,     "O_TRUNC"     },
        { O_APPEND,    "O_APPEND"    },
        { O_NONBLOCK,  "O_NONBLOCK"  },
        { O_DIRECTORY, "O_DIRECTORY" },
        { O_NOFOLLOW,  "O_NOFOLLOW"  },
        { O_CLOEXEC,   "O_CLOEXEC"   },
        { O_DIRECT,    "O_DIRECT"    },
        { O_SYNC,      "O_SYNC"      },
    };
    int accmode = flags & O_ACCMODE;
    size_t used;

    const char *acc = (accmode == O_RDONLY) ? "O_RDONLY"
                    : (accmode == O_WRONLY) ? "O_WRONLY"
                    : (accmode == O_RDWR)   ? "O_RDWR"
                    : "O_?";
    used = (size_t)snprintf(out, outsz, "%s", acc);

    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if ((flags & table[i].bit) && used < outsz - 1)
            used += (size_t)snprintf(out + used, outsz - used,
                                     "|%s", table[i].name);
}

/*
 * Append one record:
 *   2026-09-15 11:02:44.317 pid=4821 openat  "/etc/passwd"  flags=0x80000 (O_RDONLY|O_CLOEXEC) -> fd 3
 */
static void log_open(const char *fn, const char *path, int flags, int result)
{
    char line[PATH_MAX + 512], stamp[64], ftext[192];
    struct timespec ts;
    struct tm tm;
    int n;

    if (log_fd < 0)
        open_log_file();
    if (log_fd < 0)
        return;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);

    flags_text(flags, ftext, sizeof ftext);

    if (result >= 0)
        n = snprintf(line, sizeof line,
                     "%s.%03ld pid=%-6d %-7s \"%s\" flags=0x%x (%s) -> fd %d\n",
                     stamp, ts.tv_nsec / 1000000L, (int)getpid(),
                     fn, path, flags, ftext, result);
    else
        n = snprintf(line, sizeof line,
                     "%s.%03ld pid=%-6d %-7s \"%s\" flags=0x%x (%s) -> FAILED (%s)\n",
                     stamp, ts.tv_nsec / 1000000L, (int)getpid(),
                     fn, path, flags, ftext, strerror(errno));

    if (n > 0)
        raw_write(log_fd, line, (size_t)n);
}

/* ------------------------------------------------------- the interposers */

int open(const char *pathname, int flags, ...)
{
    mode_t mode = 0;
    int ret;

    if (real_open == NULL)
        real_open = dlsym(RTLD_NEXT, "open");

    /* The mode argument only exists when the caller passed O_CREAT/O_TMPFILE. */
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    ret = real_open(pathname, flags, mode);

    if (!in_hook) {
        in_hook = 1;
        log_open("open", pathname, flags, ret);
        in_hook = 0;
    }
    return ret;
}

int open64(const char *pathname, int flags, ...)
{
    mode_t mode = 0;
    int ret;

    if (real_open64 == NULL)
        real_open64 = dlsym(RTLD_NEXT, "open64");

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    ret = real_open64(pathname, flags, mode);

    if (!in_hook) {
        in_hook = 1;
        log_open("open64", pathname, flags, ret);
        in_hook = 0;
    }
    return ret;
}

/*
 * openat() matters more than open() on a current system: glibc implements
 * open() in terms of openat(), and coreutils call openat() directly, so a
 * shim that only overrides open() catches almost nothing.
 */
int openat(int dirfd, const char *pathname, int flags, ...)
{
    mode_t mode = 0;
    int ret;

    if (real_openat == NULL)
        real_openat = dlsym(RTLD_NEXT, "openat");

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    ret = real_openat(dirfd, pathname, flags, mode);

    if (!in_hook) {
        in_hook = 1;
        log_open("openat", pathname, flags, ret);
        in_hook = 0;
    }
    return ret;
}

/* Programs that use stdio never call open() directly - catch fopen() too. */
FILE *fopen(const char *pathname, const char *mode)
{
    FILE *ret;

    if (real_fopen == NULL)
        real_fopen = dlsym(RTLD_NEXT, "fopen");

    ret = real_fopen(pathname, mode);

    if (!in_hook) {
        in_hook = 1;
        log_open("fopen", pathname, 0, ret != NULL ? 0 : -1);
        in_hook = 0;
    }
    return ret;
}
