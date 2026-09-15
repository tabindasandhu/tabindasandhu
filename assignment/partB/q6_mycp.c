/*
 * q6_mycp.c - a cp(1) built directly on system calls.
 *
 * Question 6. Uses openat / fstat / read / write / close through syscall(2).
 *
 *     ./mycp source.txt dest.txt
 */
#define _GNU_SOURCE
#include "sc_trace.h"

#include <fcntl.h>
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

int main(int argc, char **argv)
{
    char buf[BUFSZ];
    struct stat st;
    long src = -1, dst = -1, n;
    mode_t mode = 0644;
    int rc = 0;

    sc_trace_init();

    if (argc != 3) {
        const char *usage = "usage: mycp SOURCE DEST\n";
        sc_write(2, usage, strlen(usage));
        return 2;
    }

    /* --- open the source ------------------------------------------------ */
    src = sc_openat(AT_FDCWD, argv[1], O_RDONLY, 0);
    if (src < 0) {
        complain(argv[0], argv[1], errno);
        return 1;
    }

    /*
     * Ask the kernel about the source so the copy inherits its permission
     * bits, which is what cp does. Without this the destination would always
     * be created 0644 and a copy of an executable would lose its +x bit.
     */
    if (sc_fstat((int)src, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            complain(argv[0], argv[1], EISDIR);
            sc_close((int)src);
            return 1;
        }
        mode = st.st_mode & 07777;
    }

    /* --- create the destination ----------------------------------------- */
    dst = sc_openat(AT_FDCWD, argv[2], O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (dst < 0) {
        complain(argv[0], argv[2], errno);
        sc_close((int)src);
        return 1;
    }

    /* --- the copy loop --------------------------------------------------- */
    for (;;) {
        n = sc_read((int)src, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            complain(argv[0], argv[1], errno);
            rc = 1;
            break;
        }
        if (n == 0)
            break;
        if (sc_write((int)dst, buf, (size_t)n) < 0) {
            complain(argv[0], argv[2], errno);
            rc = 1;
            break;
        }
    }

    sc_close((int)src);
    sc_close((int)dst);

    sc_trace_summary();
    return rc;
}
