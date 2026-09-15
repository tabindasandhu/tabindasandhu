/*
 * q9_redirect.c - a path-translation layer built on ptrace().
 *
 * Question 9. Question 7's tracer only watched. This one intervenes: when
 * the tracee enters openat(), we read the path out of its memory, and if it
 * matches a rule we REWRITE that memory and point the syscall argument at
 * the replacement before letting the kernel execute the call. The target
 * program is never told; it believes it opened the file it asked for.
 *
 *     ./redirect -r secret.txt=decoy.txt -- /bin/cat secret.txt
 *     ./redirect -r /etc/hostname=./my_hostname -- /bin/cat /etc/hostname
 *     ./redirect -r /realdir/=/faked/ -- /bin/ls /realdir/sub
 *
 * A rule ending in '/' is a prefix rule and rewrites whole subtrees; any
 * other rule is an exact match. -v also prints the full syscall trace.
 *
 * How the rewrite is done safely
 * ------------------------------
 * The replacement path is usually not the same length as the original, so
 * we cannot just overwrite it in place - that would run off the end of the
 * tracee's buffer and corrupt whatever follows. Instead we use scratch space
 * well below the tracee's stack pointer:
 *
 *   entry stop:  save the original bytes there -> write the new path there
 *                -> point the path register at it -> PTRACE_SETREGS
 *   exit stop:   restore the saved bytes and the original register
 *
 * By the time the syscall returns, the kernel has already copied the path
 * string into kernel space, so restoring the tracee's memory afterwards is
 * both safe and invisible.
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

#define MAX_RULES     16
#define SCRATCH_BELOW 512      /* bytes below rsp to borrow for the new path */

struct rule {
    char from[PATH_MAX];
    char to[PATH_MAX];
    int  is_prefix;            /* rule ended in '/': rewrite whole subtrees */
};

static struct rule rules[MAX_RULES];
static int  rule_count   = 0;
static int  verbose      = 0;
static long redirections = 0;

static void usage(void)
{
    fprintf(stderr,
        "usage: redirect -r FROM=TO [-r FROM=TO ...] [-v] -- PROGRAM [ARGS...]\n"
        "   -r FROM=TO  redirect openat(\"FROM\") to open TO instead\n"
        "               (a FROM ending in '/' redirects the whole subtree)\n"
        "   -v          also print the full syscall trace, like Question 7\n");
}

static int add_rule(const char *spec)
{
    const char *eq = strchr(spec, '=');
    size_t fromlen;

    if (eq == NULL || eq == spec || eq[1] == '\0') {
        fprintf(stderr, "redirect: bad rule '%s', expected FROM=TO\n", spec);
        return -1;
    }
    if (rule_count >= MAX_RULES) {
        fprintf(stderr, "redirect: too many rules (max %d)\n", MAX_RULES);
        return -1;
    }

    fromlen = (size_t)(eq - spec);
    if (fromlen >= PATH_MAX || strlen(eq + 1) >= PATH_MAX) {
        fprintf(stderr, "redirect: rule too long\n");
        return -1;
    }

    memcpy(rules[rule_count].from, spec, fromlen);
    rules[rule_count].from[fromlen] = '\0';
    snprintf(rules[rule_count].to, PATH_MAX, "%s", eq + 1);
    rules[rule_count].is_prefix = (fromlen > 0 && spec[fromlen - 1] == '/');
    rule_count++;
    return 0;
}

/*
 * Apply the first matching rule. Returns 1 and fills `out` if the path
 * should be rewritten, 0 if it should be left alone.
 */
static int translate(const char *path, char *out, size_t outsz)
{
    for (int i = 0; i < rule_count; i++) {
        size_t flen = strlen(rules[i].from);

        if (rules[i].is_prefix) {
            if (strncmp(path, rules[i].from, flen) == 0) {
                snprintf(out, outsz, "%s%s", rules[i].to, path + flen);
                return 1;
            }
        } else if (strcmp(path, rules[i].from) == 0) {
            snprintf(out, outsz, "%s", rules[i].to);
            return 1;
        }
    }
    return 0;
}

static void run_child(char **argv)
{
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) {
        perror("ptrace(PTRACE_TRACEME)");
        _exit(126);
    }
    execvp(argv[0], argv);
    fprintf(stderr, "redirect: cannot run %s: %s\n", argv[0], strerror(errno));
    _exit(127);
}

int main(int argc, char **argv)
{
    int opt, status, in_syscall = 0, exit_code = 0;
    long pending_nr = -1;
    pid_t child;

    /* State carried from a rewritten syscall's entry to its exit stop. */
    int           rewrote = 0;
    unsigned long scratch_addr = 0;
    unsigned long saved_reg = 0;
    char          saved_bytes[PATH_MAX];
    size_t        saved_len = 0;

    while ((opt = getopt(argc, argv, "+r:vh")) != -1) {
        switch (opt) {
        case 'r': if (add_rule(optarg) < 0) return 2; break;
        case 'v': verbose = 1; break;
        case 'h': usage(); return 0;
        default:  usage(); return 2;
        }
    }

    if (optind >= argc) {
        usage();
        return 2;
    }
    if (rule_count == 0)
        fprintf(stderr, "redirect: warning - no -r rules, this is just a tracer\n");

    for (int i = 0; i < rule_count; i++)
        fprintf(stderr, "redirect: rule %d: \"%s\" -> \"%s\"%s\n",
                i + 1, rules[i].from, rules[i].to,
                rules[i].is_prefix ? " (prefix)" : "");

    child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0)
        run_child(&argv[optind]);

    if (waitpid(child, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    ptrace(PTRACE_SETOPTIONS, child, 0,
           PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL);

    for (;;) {
        if (ptrace(PTRACE_SYSCALL, child, 0, 0) < 0) {
            if (errno == ESRCH)
                break;
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

        if (WSTOPSIG(status) != (SIGTRAP | 0x80)) {
            int sig = WSTOPSIG(status);
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
            /* ======================= syscall ENTRY ======================= */
            unsigned long args[SC_NARGS];

            pending_nr = (long)regs.orig_rax;
            regs_get_args(&regs, args);

            if (verbose) {
                char argtext[PATH_MAX + 256];
                format_syscall_args(child, pending_nr, args,
                                    argtext, sizeof argtext);
                fprintf(stderr, "%s(%s)", sc_name(pending_nr), argtext);
                fflush(stderr);
            }

            rewrote = 0;

            /*
             * openat() is where essentially all file opening ends up on a
             * modern glibc; open() is included so statically linked or older
             * programs are covered too.
             */
            if (pending_nr == SYS_openat || pending_nr == SYS_open) {
                /* openat's path is arg 1; open's is arg 0. */
                int path_argi = (pending_nr == SYS_openat) ? 1 : 0;
                char orig[PATH_MAX], newpath[PATH_MAX];

                if (tracee_read_string(child, args[path_argi],
                                       orig, sizeof orig) >= 0 &&
                    translate(orig, newpath, sizeof newpath)) {

                    size_t nlen = strlen(newpath) + 1;   /* include the NUL */

                    /*
                     * Borrow scratch space below the tracee's stack pointer.
                     * Nothing live lives there: it is past the red zone and
                     * only a deeper call would touch it, which cannot happen
                     * while the process is stopped inside a syscall.
                     */
                    scratch_addr = (unsigned long)regs.rsp - SCRATCH_BELOW - nlen;

                    saved_len = nlen;
                    if (tracee_read_mem(child, scratch_addr,
                                        saved_bytes, saved_len) == 0 &&
                        tracee_write_mem(child, scratch_addr,
                                         newpath, nlen) == 0) {

                        /* Repoint the syscall's path argument at our copy. */
                        saved_reg = (path_argi == 1) ? regs.rsi : regs.rdi;
                        if (path_argi == 1)
                            regs.rsi = scratch_addr;
                        else
                            regs.rdi = scratch_addr;

                        if (ptrace(PTRACE_SETREGS, child, 0, &regs) == 0) {
                            rewrote = 1;
                            redirections++;
                            /* In verbose mode the entry line is still open,
                             * so break the line before reporting. */
                            fprintf(stderr, "%s[redirect] \"%s\" -> \"%s\"\n",
                                    verbose ? "\n" : "", orig, newpath);
                        } else {
                            perror("ptrace(PTRACE_SETREGS)");
                            tracee_write_mem(child, scratch_addr,
                                             saved_bytes, saved_len);
                        }
                    }
                }
            }

            in_syscall = 1;
        } else {
            /* ======================== syscall EXIT ======================= */
            if (verbose) {
                char rettext[128];
                format_syscall_ret(pending_nr, (long)regs.rax,
                                   rettext, sizeof rettext);
                fprintf(stderr, " = %s\n", rettext);
            }

            if (rewrote) {
                /*
                 * The kernel has finished with the string, so put the
                 * tracee's memory and registers back exactly as they were.
                 * The program has no way to detect that anything happened.
                 */
                tracee_write_mem(child, scratch_addr, saved_bytes, saved_len);

                if (pending_nr == SYS_openat)
                    regs.rsi = saved_reg;
                else
                    regs.rdi = saved_reg;

                if (ptrace(PTRACE_SETREGS, child, 0, &regs) < 0)
                    perror("ptrace(PTRACE_SETREGS) restore");

                rewrote = 0;
            }

            in_syscall = 0;
        }
    }

    fprintf(stderr, "redirect: %ld path(s) translated\n", redirections);
    return exit_code;
}
