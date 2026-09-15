#!/usr/bin/env bash
#
# verify_env.sh - proves the machine can actually do every part of the assignment.
#                 Not just "is the binary present" - it compiles and runs a
#                 miniature version of each Part B technique.
#
#     bash assignment/setup/verify_env.sh
#
# Exit code 0 = everything required passed.
set -uo pipefail

GREEN=$'\033[32m'; RED=$'\033[31m'; YELLOW=$'\033[33m'; BOLD=$'\033[1m'; RESET=$'\033[0m'

PASS=0; FAIL=0; WARN=0
TMPDIR_V=$(mktemp -d)
trap 'rm -rf "$TMPDIR_V"' EXIT

pass() { printf '  %s[PASS]%s %s\n' "$GREEN" "$RESET" "$*"; PASS=$((PASS+1)); }
bad()  { printf '  %s[FAIL]%s %s\n' "$RED"   "$RESET" "$*"; FAIL=$((FAIL+1)); }
soft() { printf '  %s[WARN]%s %s\n' "$YELLOW" "$RESET" "$*"; WARN=$((WARN+1)); }
head_() { printf '\n%s== %s ==%s\n' "$BOLD" "$*" "$RESET"; }

need() {  # need <command> <why>
    if command -v "$1" >/dev/null 2>&1; then
        pass "$1 -> $(command -v "$1")"
    else
        bad "$1 is MISSING ($2)"
    fi
}

want() {  # optional
    if command -v "$1" >/dev/null 2>&1; then
        pass "$1 -> $(command -v "$1")"
    else
        soft "$1 is missing ($2) - optional"
    fi
}

# =============================================================== Part A: shell
head_ "Part A - shell scripting (Q1-Q5)"

bash_major=${BASH_VERSINFO[0]}
if (( bash_major >= 4 )); then
    pass "bash $BASH_VERSION (associative arrays supported)"
else
    bad "bash $BASH_VERSION is too old - Q1/Q4 need 'declare -A' (bash 4+)"
fi

# associative array smoke test (Q1 status cache, Q4 user->role map)
if declare -A _probe 2>/dev/null && { _probe[x]=y; [[ ${_probe[x]} == y ]]; }; then
    pass "declare -A works"
else
    bad "declare -A failed"
fi

# getopts (Q1 -o/-d, Q2 -f/-i)
if ( set -- -o; while getopts "od" o; do :; done ) 2>/dev/null; then
    pass "getopts available (built-in)"
else
    bad "getopts not working"
fi

# trap (Q1, Q2, Q5)
if ( trap 'true' INT TERM ) 2>/dev/null; then
    pass "trap available for SIGINT/SIGTERM handlers"
else
    bad "trap not working"
fi

need timeout  "Q3 runs each task under 'timeout'"
need md5sum   "Q2 incremental backup compares md5 checksums"
need date     "Q1-Q5 timestamped logging"
need ps       "Q1 check_cpu"
need free     "Q1 check_memory"
need df       "Q1 check_disk"
need awk      "parsing /proc and command output"
want bc       "float threshold comparison in check_cpu"
want column   "Q3 summary table formatting"
want mpstat   "second CPU source (sysstat)"
want shellcheck "lint the scripts before submitting"

# ANSI red output (Q1 critical alerts)
printf '  %s[PASS]%s ANSI colour works -> %s this should be red %s\n' \
       "$GREEN" "$RESET" $'\033[31m' $'\033[0m'; PASS=$((PASS+1))

# ============================================================ Part B: toolchain
head_ "Part B - C toolchain (Q6-Q10)"

need gcc     "compiling mycat/mycp/myls, mini_strace, wsl_lite"
need make    "building the Part B targets"
want gdb     "debugging the tracer"
want strace  "reference output to compare mini_strace against"
want ltrace  "library-call contrast for the LD_PRELOAD part"

for hdr in sys/syscall.h unistd.h dlfcn.h sys/ptrace.h sys/user.h sys/wait.h linux/fcntl.h; do
    if echo "#include <$hdr>" | gcc -E -x c - >/dev/null 2>&1; then
        pass "header <$hdr>"
    else
        bad "header <$hdr> missing - install build-essential / linux-libc-dev"
    fi
done

# ---- Q6: raw syscall() write ------------------------------------------------
cat > "$TMPDIR_V/q6.c" <<'C'
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
int main(void) {
    const char m[] = "raw syscall write() ok\n";
    return syscall(SYS_write, 1, m, sizeof(m) - 1) > 0 ? 0 : 1;
}
C
if gcc -O0 -o "$TMPDIR_V/q6" "$TMPDIR_V/q6.c" 2>"$TMPDIR_V/q6.err" && "$TMPDIR_V/q6" >/dev/null 2>&1; then
    pass "Q6: syscall(SYS_write, ...) compiles and runs"
else
    bad "Q6: raw syscall test failed -> $(tr '\n' ' ' < "$TMPDIR_V/q6.err" | cut -c1-120)"
fi

# ---- Q6: getdents64 ---------------------------------------------------------
cat > "$TMPDIR_V/q6b.c" <<'C'
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
int main(void) {
    char buf[1024];
    int fd = syscall(SYS_openat, AT_FDCWD, ".", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return 1;
    long n = syscall(SYS_getdents64, fd, buf, sizeof buf);
    syscall(SYS_close, fd);
    return n > 0 ? 0 : 1;
}
C
if gcc -O0 -o "$TMPDIR_V/q6b" "$TMPDIR_V/q6b.c" 2>/dev/null && (cd "$TMPDIR_V" && ./q6b); then
    pass "Q6: openat + getdents64 + close work (myls is feasible)"
else
    bad "Q6: getdents64 probe failed"
fi

# ---- Q7/Q9: ptrace ----------------------------------------------------------
cat > "$TMPDIR_V/q7.c" <<'C'
#define _GNU_SOURCE
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <unistd.h>
#include <stdio.h>
int main(void) {
    pid_t p = fork();
    if (p == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        execlp("/bin/true", "true", (char *)NULL);
        _exit(127);
    }
    int st; waitpid(p, &st, 0);
    if (!WIFSTOPPED(st)) return 1;
    ptrace(PTRACE_SYSCALL, p, 0, 0);
    waitpid(p, &st, 0);
    if (!WIFSTOPPED(st)) return 1;
    struct user_regs_struct r;
    if (ptrace(PTRACE_GETREGS, p, 0, &r) < 0) return 1;
    printf("first syscall nr=%llu\n", (unsigned long long)r.orig_rax);
    ptrace(PTRACE_KILL, p, 0, 0);
    waitpid(p, &st, 0);
    return 0;
}
C
if gcc -O0 -o "$TMPDIR_V/q7" "$TMPDIR_V/q7.c" 2>"$TMPDIR_V/q7.err"; then
    if out=$("$TMPDIR_V/q7" 2>&1) && [[ "$out" == *"first syscall nr="* ]]; then
        pass "Q7/Q9: ptrace(TRACEME/SYSCALL/GETREGS) works -> $out"
    else
        bad "Q7/Q9: ptrace compiled but failed at runtime -> $out"
    fi
else
    bad "Q7/Q9: ptrace test would not compile -> $(tr '\n' ' ' < "$TMPDIR_V/q7.err" | cut -c1-120)"
fi

# ---- Q9: reading tracee memory via /proc/<pid>/mem ---------------------------
if [[ -r /proc/self/mem ]]; then
    pass "Q9: /proc/<pid>/mem readable (path rewriting is possible)"
else
    bad "Q9: /proc/<pid>/mem not readable"
fi

# ---- Q8: LD_PRELOAD ---------------------------------------------------------
cat > "$TMPDIR_V/q8.c" <<'C'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
int open(const char *path, int flags, ...) {
    static int (*real_open)(const char *, int, ...);
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    fprintf(stderr, "PRELOAD open(%s)\n", path);
    return real_open(path, flags, mode);
}
C
if gcc -shared -fPIC -o "$TMPDIR_V/q8.so" "$TMPDIR_V/q8.c" -ldl 2>"$TMPDIR_V/q8.err"; then
    pass "Q8: shared library builds with dlsym(RTLD_NEXT, ...)"
    if ldd "$(command -v cat)" 2>/dev/null | grep -q 'libc'; then
        pass "Q8: /bin/cat is dynamically linked (LD_PRELOAD will take effect)"
    else
        soft "Q8: /bin/cat looks statically linked - pick another target program"
    fi
else
    bad "Q8: shared library build failed -> $(tr '\n' ' ' < "$TMPDIR_V/q8.err" | cut -c1-120)"
fi

# ================================================================ environment
head_ "Environment"

if grep -qi microsoft /proc/version 2>/dev/null; then
    pass "running under WSL: $(grep -oi 'microsoft.*' /proc/version | head -1)"
    if [[ -d /mnt/c ]]; then
        pass "Windows C: drive mounted at /mnt/c"
        soft "Keep assignment files in the Linux filesystem (~/), NOT /mnt/c -"
        soft "  DrvFs does not support Linux permissions, which breaks the"
        soft "  'file with no read permission' edge case in Q6."
    fi
else
    soft "not WSL - plain Linux is fine for every question"
fi

pass "kernel: $(uname -r)"

# ==================================================================== verdict
printf '\n%s================ RESULT ================%s\n' "$BOLD" "$RESET"
printf '  passed: %d   failed: %d   warnings: %d\n' "$PASS" "$FAIL" "$WARN"
if (( FAIL == 0 )); then
    printf '  %sEnvironment is ready for all 10 questions.%s\n' "$GREEN" "$RESET"
    exit 0
else
    printf '  %sFix the FAIL items above, then re-run.%s\n' "$RED" "$RESET"
    printf '  Most are fixed by: bash assignment/setup/install_tools.sh\n'
    exit 1
fi
