/*
 * q7_mini_strace.c - a miniature strace(1).
 *
 * Question 7. fork() + ptrace(PTRACE_TRACEME) to launch the target under
 * our control, then PTRACE_SYSCALL to stop it twice per system call: once
 * on the way in (arguments available in the registers) and once on the way
 * out (return value in rax).
 *
 *     ./mini_strace /bin/ls
 *     ./mini_strace /bin/cat notes.txt
 *     ./mini_strace -c /bin/ls            # also print a per-syscall summary
 *
 * The trace goes to stderr, so the target's own output stays clean on stdout
 * and the two can be separated:  ./mini_strace /bin/ls 2> trace.txt
 */
#define _GNU_SOURCE
#include "ptrace_common.h"

#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/syscall.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#define MAX_SYSCALL_NR 512

static long counts[MAX_SYSCALL_NR];   /* for the -c summary            */
static long total_calls = 0;
static int  want_summary = 0;

static void usage(void)
{
    fprintf(stderr, "usage: mini_strace [-c] PROGRAM [ARGS...]\n"
                    "   -c   print a summary table of syscall counts at exit\n");
}

/* The child half: ask to be traced, then become the target program. */
static void run_child(char **argv)
{
    /*
     * PTRACE_TRACEME says "my parent is my tracer". The very next execve()
     * will then stop the process and hand control to the parent before the
     * new program's first instruction runs.
     */
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) {
        perror("ptrace(PTRACE_TRACEME)");
        _exit(126);
    }

    execvp(argv[0], argv);

    /* Only reached if exec failed. */
    fprintf(stderr, "mini_strace: cannot run %s: %s\n", argv[0], strerror(errno));
    _exit(127);
}

static void print_summary(void)
{
    fprintf(stderr, "\n%-20s %10s\n", "syscall", "calls");
    fprintf(stderr, "-------------------- ----------\n");
    for (long nr = 0; nr < MAX_SYSCALL_NR; nr++)
        if (counts[nr] > 0)
            fprintf(stderr, "%-20s %10ld\n", sc_name(nr), counts[nr]);
    fprintf(stderr, "-------------------- ----------\n");
    fprintf(stderr, "%-20s %10ld\n", "TOTAL", total_calls);
}

/* The parent half: the actual tracer loop. */
static int run_tracer(pid_t child)
{
    int status;
    int in_syscall = 0;          /* toggles: 0 = next stop is entry      */
    long pending_nr = -1;        /* syscall number seen at the last entry */
    int exit_code = 0;

    /*
     * Wait for the child's initial stop, which happens the moment execve()
     * completes inside run_child().
     */
    if (waitpid(child, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    /*
     * TRACESYSGOOD tags syscall stops with bit 0x80 in the signal number so
     * they can be told apart from a real SIGTRAP the program raised itself.
     * EXITKILL makes the kernel kill the tracee if the tracer dies, so a
     * crashed tracer never leaves a stopped orphan behind.
     */
    if (ptrace(PTRACE_SETOPTIONS, child, 0,
               PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) < 0)
        perror("ptrace(PTRACE_SETOPTIONS)");

    for (;;) {
        /* Run the tracee until it enters or leaves a system call. */
        if (ptrace(PTRACE_SYSCALL, child, 0, 0) < 0) {
            if (errno == ESRCH)
                break;                  /* it exited underneath us */
            perror("ptrace(PTRACE_SYSCALL)");
            break;
        }

        if (waitpid(child, &status, 0) < 0) {
            perror("waitpid");
            break;
        }

        if (WIFEXITED(status)) {
            exit_code = WEXITSTATUS(status);
            fprintf(stderr, "+++ exited with %d +++\n", exit_code);
            break;
        }
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "+++ killed by signal %d +++\n", WTERMSIG(status));
            exit_code = 128 + WTERMSIG(status);
            break;
        }
        if (!WIFSTOPPED(status))
            continue;

        /* Not a syscall stop? Pass the signal through to the tracee. */
        if (WSTOPSIG(status) != (SIGTRAP | 0x80)) {
            int sig = WSTOPSIG(status);
            fprintf(stderr, "--- stopped by signal %d (%s) ---\n",
                    sig, strsignal(sig));
            ptrace(PTRACE_SYSCALL, child, 0, sig);
            waitpid(child, &status, 0);
            continue;
        }

        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, child, 0, &regs) < 0) {
            perror("ptrace(PTRACE_GETREGS)");
            break;
        }

        if (!in_syscall) {
            /* ---------------- syscall ENTRY ---------------- */
            unsigned long args[SC_NARGS];
            char argtext[PATH_MAX + 256];

            /*
             * orig_rax holds the syscall number. rax itself has already been
             * overwritten with -ENOSYS by the kernel at this point, which is
             * why the original is kept in a separate register.
             */
            pending_nr = (long)regs.orig_rax;
            regs_get_args(&regs, args);
            format_syscall_args(child, pending_nr, args, argtext, sizeof argtext);

            /* No newline - the return value is appended at the exit stop. */
            fprintf(stderr, "%s(%s)", sc_name(pending_nr), argtext);
            fflush(stderr);

            if (pending_nr >= 0 && pending_nr < MAX_SYSCALL_NR)
                counts[pending_nr]++;
            total_calls++;

            in_syscall = 1;
        } else {
            /* ---------------- syscall EXIT ----------------- */
            char rettext[128];

            format_syscall_ret(pending_nr, (long)regs.rax, rettext, sizeof rettext);
            fprintf(stderr, " = %s\n", rettext);

            in_syscall = 0;
        }
    }

    if (want_summary)
        print_summary();

    return exit_code;
}

int main(int argc, char **argv)
{
    int opt;
    pid_t child;

    while ((opt = getopt(argc, argv, "+ch")) != -1) {
        switch (opt) {
        case 'c': want_summary = 1; break;
        case 'h': usage(); return 0;
        default:  usage(); return 2;
        }
    }

    if (optind >= argc) {
        usage();
        return 2;
    }

    child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        run_child(&argv[optind]);
        /* not reached */
    }

    fprintf(stderr, "mini_strace: tracing pid %d (%s)\n",
            (int)child, argv[optind]);
    return run_tracer(child);
}
