/*
 * q6_mycat.c - a cat(1) built directly on system calls.
 *
 * Question 6. Uses openat / fstat / read / write / close through syscall(2).
 * No fopen, no fread, no printf.
 *
 *     ./mycat notes.txt
 *     SC_TRACE=0 ./mycat notes.txt      # suppress the trace, output only
 */
#define _GNU_SOURCE
#include "sc_trace.h"

#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>      /* snprintf only */
#include <unistd.h>

#define BUFSZ 65536

/* Report an error on stderr without using perror/fprintf. */
static void complain(const char *prog, const char *path, int err)
{
    char msg[512];
    int n = snprintf(msg, sizeof msg, "%s: %s: %s\n",
                     sc_basename(prog), path, strerror(err));
    sc_write(2, msg, (size_t)n);
}

/*
 * Copy one file to stdout.
 * Returns 0 on success, 1 on failure (mirrors cat's exit status).
 */
static int cat_one(const char *prog, const char *path)
{
    char buf[BUFSZ];
    struct stat st;
    long fd, n;
    int rc = 0;

    fd = sc_openat(AT_FDCWD, path, O_RDONLY, 0);
    if (fd < 0) {
        complain(prog, path, errno);
        return 1;
    }

    /*
     * fstat first, the way real cat does: it wants st_blksize to size its
     * buffer, and it needs to know whether it was handed a directory.
     * Note that openat() on a directory SUCCEEDS - the failure only shows
     * up on the first read(), which returns EISDIR. That is why cat has to
     * stat at all, and it is one of the differences discussed in README.md.
     */
    if (sc_fstat((int)fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        complain(prog, path, EISDIR);
        sc_close((int)fd);
        return 1;
    }

    for (;;) {
        n = sc_read((int)fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            complain(prog, path, errno);
            rc = 1;
            break;
        }
        if (n == 0)
            break;                       /* end of file */
        if (sc_write(1, buf, (size_t)n) < 0) {
            complain(prog, path, errno);
            rc = 1;
            break;
        }
    }

    sc_close((int)fd);
    return rc;
}

int main(int argc, char **argv)
{
    int rc = 0;

    sc_trace_init();

    if (argc < 2) {
        const char *usage = "usage: mycat FILE [FILE...]\n";
        sc_write(2, usage, strlen(usage));
        return 2;
    }

    for (int i = 1; i < argc; i++)
        if (cat_one(argv[0], argv[i]) != 0)
            rc = 1;

    sc_trace_summary();
    return rc;
}
