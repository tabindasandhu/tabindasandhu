/*
 * ptrace_common.h - machinery shared by Question 7 (mini_strace) and
 *                   Question 9 (the path-redirection layer).
 *
 * Question 9 is explicitly an extension of Question 7, so the syscall
 * decoding, the tracee-memory access and the argument formatting live here
 * and both programs link against them.
 *
 * Architecture note: register names below (orig_rax, rdi, rsi, ...) are
 * x86-64 specific. That is the calling convention this assignment targets.
 */
#ifndef PTRACE_COMMON_H
#define PTRACE_COMMON_H

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/user.h>
#include <stddef.h>

#define SC_NARGS 6

/* Map a syscall number to its name, e.g. 257 -> "openat". */
const char *sc_name(long nr);

/* Symbolic errno name for a negated kernel return value, e.g. -2 -> ENOENT. */
const char *errno_symbol(int e);

/*
 * Pull the six syscall arguments out of a register snapshot.
 * The x86-64 Linux syscall convention passes them in
 * rdi, rsi, rdx, r10, r8, r9 - note r10, NOT rcx, because the SYSCALL
 * instruction clobbers rcx with the return address.
 */
void regs_get_args(const struct user_regs_struct *r, unsigned long args[SC_NARGS]);

/*
 * Read a NUL-terminated string out of the traced process's address space
 * using PTRACE_PEEKDATA, one machine word at a time. Returns the length
 * copied, or -1 on failure.
 */
long tracee_read_string(pid_t pid, unsigned long addr, char *out, size_t outsz);

/*
 * Write raw bytes into the traced process's address space through
 * /proc/<pid>/mem. Returns 0 on success, -1 on failure.
 */
int tracee_write_mem(pid_t pid, unsigned long addr, const void *src, size_t len);

/*
 * Read raw bytes out of the traced process's address space through
 * /proc/<pid>/mem. Used by Question 9 to save the scratch area before
 * overwriting it. Returns 0 on success, -1 on failure.
 */
int tracee_read_mem(pid_t pid, unsigned long addr, void *dst, size_t len);

/* Render an open(2)/openat(2) flags word as O_RDONLY|O_CLOEXEC etc. */
void open_flags_text(long flags, char *out, size_t outsz);

/*
 * Format a syscall's arguments for display. Known syscalls get their real
 * argument shapes (paths as quoted strings, flags decoded); everything else
 * falls back to hex.
 */
void format_syscall_args(pid_t pid, long nr,
                         const unsigned long args[SC_NARGS],
                         char *out, size_t outsz);

/* Format a syscall return value the way strace does. */
void format_syscall_ret(long nr, long ret, char *out, size_t outsz);

#endif /* PTRACE_COMMON_H */
