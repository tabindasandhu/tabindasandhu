/*
 * sc_trace.h - thin logging wrappers around raw Linux system calls.
 *
 * Every function here issues its system call through syscall(2) directly.
 * No libc I/O wrapper (fopen/fread/opendir/printf/...) is used anywhere in
 * the data path. snprintf() appears only to format a message INTO A BUFFER;
 * it performs no I/O and issues no system call of its own. The buffer is
 * then handed to the kernel with a raw syscall(SYS_write, 2, ...).
 *
 * Each wrapper prints a strace-style line to stderr so the mapping from
 * "my command" to "kernel operation" is visible:
 *
 *     [syscall] openat(AT_FDCWD, "notes.txt", O_RDONLY, 0) = 3
 *     [syscall] read(3, 0x7ffd1c2a, 65536)                 = 27
 *     [syscall] close(3)                                   = 0
 *
 * Used by Question 6 (mycat/mycp/myls) and Question 10 (wsl_lite).
 */
#ifndef SC_TRACE_H
#define SC_TRACE_H

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <stddef.h>

/*
 * The kernel's directory entry layout returned by getdents64(2).
 * glibc does not expose this struct (it hides it behind opendir/readdir),
 * so we declare it ourselves - which is precisely the point of Question 6.
 */
struct linux_dirent64 {
    unsigned long long d_ino;     /* inode number                     */
    long long          d_off;     /* offset to next entry             */
    unsigned short     d_reclen;  /* length of this whole entry       */
    unsigned char      d_type;    /* DT_REG, DT_DIR, ...              */
    char               d_name[];  /* NUL-terminated filename          */
};

/* Turn tracing on/off at runtime via the SC_TRACE environment variable. */
void sc_trace_init(void);
int  sc_trace_enabled(void);

/* Emit an arbitrary line to stderr using raw write(2). */
void sc_emit(const char *text);

/*
 * The last path component, for error messages. Real cat prints "cat: ...",
 * not "/usr/bin/cat: ...", and ours should match.
 */
const char *sc_basename(const char *path);

/* Emit a line to stdout using raw write(2) - for command output proper. */
long sc_write(int fd, const void *buf, size_t count);

/*
 * Traced system call wrappers. Each returns exactly what the kernel
 * returned (negative errno values are translated to -1 + errno by the
 * libc syscall() wrapper, matching normal Linux calling convention).
 */
long sc_openat(int dirfd, const char *path, int flags, mode_t mode);
long sc_read(int fd, void *buf, size_t count);
long sc_close(int fd);
long sc_fstat(int fd, struct stat *st);
long sc_getdents64(int fd, void *buf, size_t count);
long sc_lseek(int fd, off_t offset, int whence);

/* Print a one-line summary: how many syscalls were issued in total. */
void sc_trace_summary(void);

/* Zero the counter, so wsl_lite can report a per-command figure. */
void sc_trace_reset(void);

/* How many syscalls have been issued since the last reset. */
long sc_trace_count(void);

#endif /* SC_TRACE_H */
