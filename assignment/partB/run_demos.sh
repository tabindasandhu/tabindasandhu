#!/usr/bin/env bash
#
# run_demos.sh - run every Part B demonstration and capture its output.
#
#     ./run_demos.sh
#
# Results land in ./output/, one file per question, which is what gets
# submitted alongside the source.
set -u

BOLD=$'\033[1m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[0;33m'; RESET=$'\033[0m'

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/output"
WORK="$HERE/demo_workspace"

rm -rf "$OUT" "$WORK"
mkdir -p "$OUT" "$WORK"

banner() { printf '\n%s===== %s =====%s\n' "$BOLD" "$*" "$RESET"; }
note()   { printf '%s  -> %s%s\n' "$GREEN" "$*" "$RESET"; }
warn()   { printf '%s  !! %s%s\n' "$YELLOW" "$*" "$RESET"; }

# Section header inside a capture file.
section() { printf '\n########## %s ##########\n\n' "$*"; }

# ---------------------------------------------------------------------------
# The "no read permission" edge case cannot be demonstrated as root, because
# root bypasses the permission check entirely. If we are root, re-run just
# those commands as an unprivileged user.
# ---------------------------------------------------------------------------
UNPRIV=()
if [[ $EUID -eq 0 ]]; then
    if command -v setpriv >/dev/null 2>&1; then
        UNPRIV=(setpriv --reuid=65534 --regid=65534 --clear-groups)
        warn "running as root - permission tests drop to uid 65534 (nobody)"
    else
        warn "running as root and setpriv is unavailable:"
        warn "the no-read-permission case will not fail as it should"
    fi
fi

# ===========================================================================
banner "Question 6 - raw-syscall utilities (mycat / mycp / myls)"
# ===========================================================================
{
    cd "$WORK" || exit 1

    printf 'line one\nline two\nline three\n' > sample.txt
    : > empty.txt                       # a zero-byte file
    printf 'secret contents\n' > noperm.txt
    chmod 000 noperm.txt
    mkdir -p emptydir

    section "mycat on a normal file - full syscall trace"
    "$HERE/mycat" sample.txt

    section "EDGE CASE 1: an EMPTY file"
    echo "--- real cat ---"
    cat empty.txt; echo "cat exit status: $?"
    echo "--- mycat ---"
    "$HERE/mycat" empty.txt; echo "mycat exit status: $?"

    section "EDGE CASE 2: a file with NO READ PERMISSION"
    ls -l noperm.txt
    echo "--- real cat ---"
    "${UNPRIV[@]}" cat noperm.txt; echo "cat exit status: $?"
    echo "--- mycat ---"
    "${UNPRIV[@]}" "$HERE/mycat" noperm.txt; echo "mycat exit status: $?"

    section "EDGE CASE 3: an EMPTY directory"
    echo "--- real ls ---"
    ls emptydir; echo "ls exit status: $?"
    echo "--- real ls -a (shows what is really there) ---"
    ls -a emptydir
    echo "--- myls ---"
    "$HERE/myls" emptydir; echo "myls exit status: $?"
    echo "--- myls -a (the kernel's raw view) ---"
    "$HERE/myls" -a emptydir

    section "EDGE CASE 4: cat/mycat on a DIRECTORY"
    echo "--- real cat ---"
    cat emptydir; echo "cat exit status: $?"
    echo "--- mycat ---"
    "$HERE/mycat" emptydir; echo "mycat exit status: $?"

    section "mycp - copying, and preserving the source's mode bits"
    printf '#!/bin/sh\necho hi\n' > script.sh
    chmod 755 script.sh
    "$HERE/mycp" script.sh script_copy.sh
    echo "--- permissions after copy (mode must be preserved) ---"
    ls -l script.sh script_copy.sh

    section "mycp - destination that cannot be created"
    "$HERE/mycp" sample.txt /proc/definitely_not_writable
    echo "mycp exit status: $?"

    section "output equivalence check (trace suppressed with SC_TRACE=0)"
    # The comparison files are kept OUTSIDE the directory being listed, so
    # creating them cannot change what myls and ls are being asked to list.
    cmp_dir="$(mktemp -d)"

    SC_TRACE=0 "$HERE/mycat" sample.txt > "$cmp_dir/mycat_out" 2>/dev/null
    cat sample.txt > "$cmp_dir/cat_out"
    if diff -q "$cmp_dir/mycat_out" "$cmp_dir/cat_out" >/dev/null; then
        echo "PASS: mycat output is byte-identical to cat"
    else
        echo "FAIL: mycat output differs from cat"
        diff "$cmp_dir/cat_out" "$cmp_dir/mycat_out"
    fi

    # myls prefixes each name with a type character, so strip it before
    # comparing, and sort both because myls returns kernel (hash) order.
    SC_TRACE=0 "$HERE/myls" . 2>/dev/null | sed 's/^..//' | sort > "$cmp_dir/myls_names"
    ls -1 . | sort > "$cmp_dir/ls_names"
    if diff -q "$cmp_dir/myls_names" "$cmp_dir/ls_names" >/dev/null; then
        echo "PASS: myls lists exactly the same names as ls"
    else
        echo "FAIL: myls and ls name lists differ:"
        diff "$cmp_dir/ls_names" "$cmp_dir/myls_names"
    fi

    section "ORDERING: myls returns kernel order, ls sorts alphabetically"
    echo "--- ls (sorted by ls itself, not by the kernel) ---"
    ls -1 . | head -8
    echo "--- myls (exactly the order getdents64 handed back) ---"
    SC_TRACE=0 "$HERE/myls" . 2>/dev/null | sed 's/^..//' | head -8

    rm -rf "$cmp_dir"
} > "$OUT/q6_raw_syscall_utils.txt" 2>&1
note "output/q6_raw_syscall_utils.txt"

# ===========================================================================
banner "Question 7 - mini_strace"
# ===========================================================================
{
    cd "$WORK" || exit 1

    section "TARGET 1: /bin/ls on a small directory"
    "$HERE/mini_strace" /bin/ls emptydir

    section "TARGET 2: /bin/cat sample.txt"
    "$HERE/mini_strace" /bin/cat sample.txt

    section "TARGET 3: our own mycat - raw syscalls seen from the outside"
    SC_TRACE=0 "$HERE/mini_strace" "$HERE/mycat" sample.txt

    section "TARGET 4: syscall count summary for /bin/ls (-c)"
    "$HERE/mini_strace" -c /bin/ls emptydir

    section "CROSS-CHECK: the real strace on the same command"
    if command -v strace >/dev/null 2>&1; then
        strace -f /bin/cat sample.txt 2>&1 | tail -20
    else
        echo "(strace not installed - skipping cross-check)"
    fi
} > "$OUT/q7_mini_strace.txt" 2>&1
note "output/q7_mini_strace.txt"

# ===========================================================================
banner "Question 8 - LD_PRELOAD open() wrapper"
# ===========================================================================
LOG="$WORK/open_trace.log"
{
    cd "$WORK" || exit 1
    rm -f "$LOG"

    section "unmodified /bin/cat, with the wrapper preloaded"
    LD_PRELOAD="$HERE/libopenlog.so" OPEN_LOG_FILE="$LOG" cat sample.txt

    section "unmodified /bin/head and /usr/bin/wc"
    LD_PRELOAD="$HERE/libopenlog.so" OPEN_LOG_FILE="$LOG" head -2 sample.txt
    LD_PRELOAD="$HERE/libopenlog.so" OPEN_LOG_FILE="$LOG" wc -l sample.txt

    section "a program that opens MANY files: grep -r"
    LD_PRELOAD="$HERE/libopenlog.so" OPEN_LOG_FILE="$LOG" \
        grep -r "line two" . 2>/dev/null | head -5

    section "a failing open is logged too"
    LD_PRELOAD="$HERE/libopenlog.so" OPEN_LOG_FILE="$LOG" \
        cat /definitely/not/here 2>&1

    section "WHAT LD_PRELOAD CANNOT SEE: our own mycat from Question 6"
    echo "mycat calls syscall() directly and never goes through libc's open(),"
    echo "so symbol interposition has nothing to intercept. Compare this with"
    echo "Question 7, whose ptrace tracer sees every one of its calls."
    rm -f "$WORK/blind.log"
    LD_PRELOAD="$HERE/libopenlog.so" OPEN_LOG_FILE="$WORK/blind.log" \
        SC_TRACE=0 "$HERE/mycat" sample.txt
    echo "--- log after running mycat with the shim preloaded ---"
    if [[ -s "$WORK/blind.log" ]]; then
        cat "$WORK/blind.log"
    else
        echo "(empty - the shim saw nothing at all)"
    fi

    section "THE RESULTING LOG"
    cat "$LOG"

    section "how many opens were captured"
    printf 'total logged opens: %s\n' "$(wc -l < "$LOG")"
} > "$OUT/q8_ld_preload.txt" 2>&1
note "output/q8_ld_preload.txt"

# ===========================================================================
banner "Question 9 - path redirection"
# ===========================================================================
{
    cd "$WORK" || exit 1

    printf 'THIS IS THE REAL SECRET FILE\n' > secret.txt
    printf 'this is the harmless decoy file\n' > decoy.txt
    printf 'a replacement whose path is deliberately much longer\n' \
        > replacement_with_a_very_long_filename.txt
    mkdir -p realdir fakedir
    printf 'real directory content\n' > realdir/data.txt
    printf 'FAKE directory content\n'  > fakedir/data.txt

    section "BEFORE: cat secret.txt with no interception"
    cat secret.txt

    section "AFTER: same command, same arguments, through the redirector"
    "$HERE/redirect" -r secret.txt=decoy.txt -- /bin/cat secret.txt

    section "the replacement path may be LONGER than the original"
    "$HERE/redirect" \
        -r secret.txt=replacement_with_a_very_long_filename.txt \
        -- /bin/cat secret.txt

    section "PREFIX rule: redirect a whole subtree"
    echo "--- without redirection ---"
    cat realdir/data.txt
    echo "--- with realdir/ -> fakedir/ ---"
    "$HERE/redirect" -r realdir/=fakedir/ -- /bin/cat realdir/data.txt

    section "redirecting a system file the program trusts"
    printf 'this machine is called something-else\n' > my_hostname
    "$HERE/redirect" -r /etc/hostname=my_hostname -- /bin/cat /etc/hostname

    section "it works on a program we did not write and cannot recompile: head"
    "$HERE/redirect" -r secret.txt=decoy.txt -- /usr/bin/head -1 secret.txt

    section "VERBOSE: the full trace with the rewrite visible in context"
    "$HERE/redirect" -v -r secret.txt=decoy.txt -- /bin/cat secret.txt 2>&1 \
        | grep -E 'openat|redirect|exited'

    section "PROOF the tracee cannot tell: it still reports the ORIGINAL name"
    echo "cat was asked for secret.txt and printed decoy.txt's contents,"
    echo "and any error message it produces still names secret.txt:"
    "$HERE/redirect" -r secret.txt=/does/not/exist -- /bin/cat secret.txt
} > "$OUT/q9_path_redirection.txt" 2>&1
note "output/q9_path_redirection.txt"

# ===========================================================================
banner "Question 10 - wsl_lite dispatcher"
# ===========================================================================
{
    cd "$WORK" || exit 1

    section "the command table"
    printf 'help\nexit\n' | "$HERE/wsl_lite"

    section "a full session"
    printf 'wsl-read sample.txt\nwsl-stat sample.txt\nwsl-copy sample.txt copied.txt\nwsl-read copied.txt\nwsl-write generated.txt hello from wsl_lite\nwsl-read generated.txt\nwsl-list .\nwsl-read /nonexistent\nbogus-command\nexit\n' \
        | "$HERE/wsl_lite"

    section "one-shot mode"
    "$HERE/wsl_lite" wsl-read sample.txt

    section "wsl_lite seen from the OUTSIDE by our own Question 7 tracer"
    echo "(its raw syscalls appear in mini_strace's trace, proving wsl_lite"
    echo " really does go straight to the kernel rather than shelling out)"
    SC_TRACE=0 "$HERE/mini_strace" "$HERE/wsl_lite" wsl-read sample.txt 2>&1 \
        | grep -E 'openat|read\(|write\(|close\(|exited' | head -20

    section "proof it never shells out: no execve appears in the trace"
    echo "wsl_lite's own startup execve happens before the tracer attaches,"
    echo "so ANY execve in the trace below would mean it launched a helper"
    echo "program such as /bin/cat. The count is:"
    SC_TRACE=0 "$HERE/mini_strace" "$HERE/wsl_lite" wsl-read sample.txt 2>&1 \
        | grep -c 'execve'
    echo "(0 = the command was carried out entirely by wsl_lite's own syscalls)"
} > "$OUT/q10_wsl_lite.txt" 2>&1
note "output/q10_wsl_lite.txt"

# ---------------------------------------------------------------------------
# Make sure the workspace can be removed later by `make clean`.
#
# Only DIRECTORIES need their permissions restored: unlinking a file depends on
# the containing directory's write bit, not on the file's own mode. Restoring
# files too would clear the 0600->000 on noperm.txt and silently break the
# "no read permission" edge case for anyone re-running the commands by hand
# afterwards, since the owner would then be able to read it again.
find "$WORK" -type d -exec chmod u+rwx {} + 2>/dev/null

banner "Done"
printf 'All demonstration output is in %s/\n\n' "$OUT"
ls -l "$OUT"
