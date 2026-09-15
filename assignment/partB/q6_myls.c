/*
 * q6_myls.c - an ls(1) built directly on system calls.
 *
 * Question 6. Uses openat(O_DIRECTORY) / getdents64 / close through
 * syscall(2). No opendir, no readdir, no scandir.
 *
 *     ./myls              # current directory
 *     ./myls /etc
 *     ./myls -a somedir   # include dot entries
 *
 * getdents64 is the call glibc's readdir() is built on top of. Talking to it
 * directly means we see exactly what the kernel hands back: an opaque byte
 * buffer of variable-length records that we have to walk ourselves.
 */
#define _GNU_SOURCE
#include "sc_trace.h"

#include <fcntl.h>
#include <dirent.h>     /* DT_* constants only */
#include <limits.h>     /* NAME_MAX */
#include <errno.h>
#include <string.h>
#include <stdio.h>      /* snprintf only */
#include <unistd.h>

#define BUFSZ 65536

static void complain(const char *prog, const char *path, int err)
{
    char msg[512];
    int n = snprintf(msg, sizeof msg, "%s: %s: %s\n",
                     sc_basename(prog), path, strerror(err));
    sc_write(2, msg, (size_t)n);
}

/* One-letter type tag, taken from d_type, so the output shows what the
 * kernel told us without us having to stat every entry separately. */
static char type_char(unsigned char d_type)
{
    switch (d_type) {
    case DT_REG:  return '-';
    case DT_DIR:  return 'd';
    case DT_LNK:  return 'l';
    case DT_FIFO: return 'p';
    case DT_SOCK: return 's';
    case DT_CHR:  return 'c';
    case DT_BLK:  return 'b';
    default:      return '?';   /* DT_UNKNOWN - some filesystems do this */
    }
}

int main(int argc, char **argv)
{
    char buf[BUFSZ];
    const char *path = ".";
    int show_all = 0;
    long fd, nread;
    int entries = 0;

    sc_trace_init();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0)
            show_all = 1;
        else
            path = argv[i];
    }

    /*
     * O_DIRECTORY makes the kernel refuse the open if the path is not a
     * directory, so we get ENOTDIR here rather than a confusing failure
     * from getdents64 later.
     */
    fd = sc_openat(AT_FDCWD, path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        complain(argv[0], path, errno);
        return 1;
    }

    /*
     * getdents64 fills our buffer with as many variable-length records as
     * fit and returns the number of BYTES used - not a count of entries.
     * It returns 0 when the directory is exhausted, which is how we detect
     * the end. An empty directory still yields the "." and ".." entries,
     * so the first call returns > 0 even then.
     */
    for (;;) {
        nread = sc_getdents64((int)fd, buf, sizeof buf);
        if (nread < 0) {
            complain(argv[0], path, errno);
            sc_close((int)fd);
            return 1;
        }
        if (nread == 0)
            break;

        for (long off = 0; off < nread; ) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            char line[NAME_MAX + 64];
            int n;

            off += d->d_reclen;

            if (!show_all && d->d_name[0] == '.')
                continue;       /* hide dot entries unless -a, like ls */

            n = snprintf(line, sizeof line, "%c %s\n",
                         type_char(d->d_type), d->d_name);
            sc_write(1, line, (size_t)n);
            entries++;
        }
    }

    sc_close((int)fd);

    if (entries == 0 && sc_trace_enabled()) {
        const char *note = "[myls] directory contains no visible entries\n";
        sc_write(2, note, strlen(note));
    }

    sc_trace_summary();
    return 0;
}
