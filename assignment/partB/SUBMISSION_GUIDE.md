# Part B — Step-by-step run guide and screenshot checklist

Follow this top to bottom inside **WSL Ubuntu**. Every command is copy-pasteable.
Twenty screenshot points are marked **📸 S1 … S20**, each with the caption to
write underneath it — a screenshot on its own earns nothing, the caption is
where you show you understood what is on screen.

> **Do not work under `/mnt/c`.** Keep everything in the Linux filesystem
> (`~/`). DrvFs ignores `chmod`, so screenshot **S4** (the unreadable file)
> cannot be produced there — `cat` would simply print the file.
>
> **Do not run any of this as `root`/`sudo`.** Root bypasses permission checks,
> which also breaks **S4**. Run as your normal WSL user.

---

## Phase 0 — Set up the machine (once)

### Step 0.1 — Confirm WSL 2

In **Windows PowerShell**:

```powershell
wsl -l -v
```

It must say `VERSION 2`. If it says 1:

```powershell
wsl --set-version Ubuntu 2
```

WSL 1 cannot run Part B — its `ptrace` support is incomplete, so Questions 7
and 9 will not work. (Worth a sentence in your report: the assignment about
recreating WSL 1 cannot be done *on* WSL 1.)

### Step 0.2 — Get the code

In the **Ubuntu** terminal:

```bash
cd ~
git clone https://github.com/tabindasandhu/tabindasandhu.git
cd tabindasandhu
git checkout claude/shell-scripting-assignment-073bqa
```

### Step 0.3 — Install the toolchain

```bash
bash assignment/setup/install_tools.sh
```

Takes a few minutes. Installs gcc, make, gdb, strace, ltrace, manpages-dev
and the rest.

### Step 0.4 — Verify the environment

```bash
bash assignment/setup/verify_env.sh
```

This compiles and runs a miniature version of each Part B technique rather
than only checking that binaries exist. You want **`failed: 0`**.

> ### 📸 S1 — the verification result
> Capture the bottom of the output showing `passed: 31  failed: 0` and the
> green `Environment is ready for all 10 questions.`
>
> **Caption:** "Environment verification. The check compiles and runs a raw
> `syscall(SYS_write)`, an `openat`+`getdents64` sequence, a real
> `fork`+`PTRACE_TRACEME`+`PTRACE_GETREGS` tracer and a
> `dlsym(RTLD_NEXT)` shared library, confirming the machine supports all of
> Part B before any work begins."

---

## Phase 1 — Build

```bash
cd ~/tabindasandhu/assignment/partB
make
```

Seven targets build with no warnings under `-Wall -Wextra`.

> ### 📸 S2 — the clean build
> Capture the full `make` output.
>
> **Caption:** "All seven Part B targets compile cleanly under `-Wall
> -Wextra`. Note `./gen_syscall_names.sh > syscall_names.h` — the syscall
> number-to-name table used by Questions 7 and 9 is generated from the
> system headers rather than hand-typed, so it matches the running kernel."

Now generate the demonstration workspace and captured evidence:

```bash
make demo
```

This creates `output/` (five capture files you submit) and `demo_workspace/`
(the sample files used below). **Everything from here runs inside that
workspace:**

```bash
cd demo_workspace
```

---

## Phase 2 — Question 6: raw-syscall utilities

### Step 2.1 — The basic trace

```bash
../mycat sample.txt
```

> ### 📸 S3 — command-to-syscall mapping
> **Caption:** "`mycat` printing a 29-byte file in six system calls:
> `openat` → `fstat` → `read`(29) → `write`(29) → `read`(0) → `close`.
> Every call is issued with `syscall(2)` directly; no `fopen`/`fread` is
> used anywhere. End-of-file is not a special condition — it is simply
> `read()` returning 0."

### Step 2.2 — Edge case 1: an empty file

```bash
echo "--- real cat ---"; cat empty.txt; echo "exit=$?"
echo "--- mycat ---";    ../mycat empty.txt; echo "exit=$?"
```

> ### 📸 S4 — empty file
> **Caption:** "Both produce no output and exit 0 — identical behaviour. The
> trace shows why: the first `read()` returns 0, so the loop never runs and
> nothing is ever written. Four syscalls instead of six."

### Step 2.3 — Edge case 2: a file with no read permission

```bash
ls -l noperm.txt
echo "--- real cat ---"; cat noperm.txt;      echo "exit=$?"
echo "--- mycat ---";    ../mycat noperm.txt; echo "exit=$?"
```

Both must print `Permission denied` and exit 1. **If `cat` prints the file
contents instead, you are running as root** — open a normal user shell and
retry.

> ### 📸 S5 — permission denied
> **Caption:** "Identical behaviour, and it could not be otherwise: the
> decision is not made by either program. The kernel refuses at `openat`
> and returns `-EACCES`, visible in the trace as
> `openat(...) = -1 EACCES`. Both programs can only report what the kernel
> told them. Two syscalls and the command is over."

### Step 2.4 — Edge case 3: an empty directory ⭐ *the best one*

```bash
echo "--- real ls ---";  ls emptydir;  echo "exit=$?"
echo "--- real ls -a ---"; ls -a emptydir
echo "--- myls ---";     ../myls emptydir
echo "--- myls -a ---";  ../myls -a emptydir
```

> ### 📸 S6 — empty directory
> **Caption:** "The key result of Question 6. `ls` shows nothing, but
> `getdents64` returned **48 bytes, not 0** — an 'empty' directory still
> contains `.` and `..`. Hiding dot-entries is a convention `ls` implements
> in user space, not a fact the kernel reports; `myls` only matches `ls`
> because it was written to skip names beginning with `.`, as `myls -a`
> proves."

### Step 2.5 — `cat` on a directory

```bash
echo "--- real cat ---"; cat emptydir;      echo "exit=$?"
echo "--- mycat ---";    ../mycat emptydir; echo "exit=$?"
```

> ### 📸 S7 — openat on a directory succeeds
> **Caption:** "Counter-intuitive result: `openat()` on a directory
> **succeeds** and returns fd 3. The failure only surfaces at the first
> `read()`, which returns `EISDIR`. This is exactly why real `cat` calls
> `fstat` on everything it opens, and why `mycat` does too."

### Step 2.6 — Ordering, and `mycp` mode preservation

```bash
echo "--- ls (sorted) ---";        ls -1 . | head -6
echo "--- myls (kernel order) ---"; SC_TRACE=0 ../myls . | sed 's/^..//' | head -6
echo
../mycp script.sh proof_copy.sh
ls -l script.sh proof_copy.sh
```

> ### 📸 S8 — ordering and mode preservation
> **Caption:** "Left: `ls` sorts alphabetically **in user space** — the
> kernel does not sort. `myls` prints the raw order `getdents64` returned
> (ext4 hash order). Below: `mycp` `fstat`s the source and passes
> `st_mode & 07777` to `openat(..., O_CREAT, ...)`, so a 0755 script stays
> 0755; without that step every copy would come out 0644 and executables
> would lose their `+x` bit."

---

## Phase 3 — Question 7: the ptrace tracer

### Step 3.1 — Target 1

```bash
../mini_strace /bin/ls emptydir
```

> ### 📸 S9 — tracing `/bin/ls`
> **Caption:** "`mini_strace` tracing an unmodified `/bin/ls`. It uses
> `fork()` + `PTRACE_TRACEME` to launch the target, then `PTRACE_SYSCALL`
> to stop it twice per system call — once on entry, where the number is read
> from `orig_rax` and the arguments from `rdi/rsi/rdx/r10/r8/r9`, and once
> on exit, where the return value is read from `rax`."

### Step 3.2 — Target 2

```bash
../mini_strace /bin/cat sample.txt
```

> ### 📸 S10 — tracing `/bin/cat`
> **Caption:** "The second required target. The trace shows the whole life
> of the process: the dynamic linker probing `/etc/ld.so.preload` (the same
> mechanism Question 8 exploits), mapping `ld.so.cache` and libc, and only
> then the four calls that actually print the file — `openat`, `read`,
> `write`, `close`."

### Step 3.3 — Prove the tracer is correct ⭐

```bash
echo "=== real strace ===";   strace -c /bin/cat sample.txt 2>&1 >/dev/null | tail -2
echo "=== our tracer ===";    ../mini_strace -c /bin/cat sample.txt 2>&1 >/dev/null | tail -2
```

Both must report the same total (44 on the reference machine; your number may
differ slightly with a different libc, but the **two must match each other**).

> ### 📸 S11 — validation against real strace
> **Caption:** "Validation. The real `strace -c` and my `mini_strace -c`
> count exactly the same number of system calls for the same command. An
> exact match is the strongest available evidence that the tracer is neither
> missing syscall stops nor double-counting entry and exit as two calls."

---

## Phase 4 — Question 8: the LD_PRELOAD wrapper

### Step 4.1 — Wrap an unmodified program

```bash
rm -f mylog.txt
LD_PRELOAD=../libopenlog.so OPEN_LOG_FILE=./mylog.txt cat sample.txt
echo "--- the log ---"
cat mylog.txt
```

> ### 📸 S12 — wrapper on unmodified `cat`
> **Caption:** "A stock `/bin/cat`, not recompiled and with no source
> changes, has its file-opening wrapped. `LD_PRELOAD` puts my library ahead
> of libc in the dynamic linker's search order, so `cat`'s call to `open()`
> binds to mine; I log the filename, flags and timestamp, then forward to
> the real implementation obtained with `dlsym(RTLD_NEXT, "open")`."

### Step 4.2 — A program that opens many files

```bash
rm -f mylog.txt
LD_PRELOAD=../libopenlog.so OPEN_LOG_FILE=./mylog.txt grep -r "line two" . >/dev/null 2>&1
LD_PRELOAD=../libopenlog.so OPEN_LOG_FILE=./mylog.txt cat /definitely/not/here 2>&1
echo "--- $(wc -l < mylog.txt) opens logged ---"
cat mylog.txt
```

> ### 📸 S13 — a full log, including failures
> **Caption:** "`grep -r` walking a tree, with every open recorded. Note the
> decoded flags `O_DIRECTORY|O_NOFOLLOW` — `grep` refuses to follow symlinks
> during recursive descent. The final line shows a **failed** open is logged
> too. The shim writes its log using raw syscalls rather than `fopen`,
> because logging through `open()` would resolve straight back into the
> interposed symbol and recurse infinitely."

### Step 4.3 — What LD_PRELOAD *cannot* see ⭐ *high-value*

```bash
rm -f blind.log
LD_PRELOAD=../libopenlog.so OPEN_LOG_FILE=./blind.log SC_TRACE=0 ../mycat sample.txt
echo "--- log after running mycat with the shim preloaded ---"
cat blind.log 2>/dev/null || echo "(the log file was never even created)"
```

> ### 📸 S14 — the limits of symbol interposition
> **Caption:** "The shim is completely blind to my own `mycat`, which calls
> `syscall()` directly and never goes through libc's `open()`. It is
> likewise blind to the dynamic linker's own opens of libc, which S10 shows
> the ptrace tracer catching. The two techniques intercept at different
> boundaries: `LD_PRELOAD` at the **library call**, defeated by static
> linking or direct syscalls; `ptrace` at the **kernel entry**, which
> nothing can bypass. This is precisely why Question 9's translation layer
> must be built on `ptrace` — and why WSL v1 could not have been a library
> shim."

---

## Phase 5 — Question 9: path redirection

### Step 5.1 — The core demonstration

```bash
echo "=== what the file really contains ==="
cat secret.txt
echo "=== same command, same arguments, through the redirector ==="
../redirect -r secret.txt=decoy.txt -- /bin/cat secret.txt
```

> ### 📸 S15 — transparent redirection
> **Caption:** "`/bin/cat` is asked for `secret.txt` and prints
> `decoy.txt`'s contents instead. At the `openat` entry stop the tracer
> reads the path out of the tracee with `PTRACE_PEEKDATA`, writes the
> replacement into the tracee's memory, repoints the `rsi` register at it
> with `PTRACE_SETREGS`, and lets the syscall proceed. `cat` was not
> recompiled and has no way to detect this."

### Step 5.2 — Proof the tracee cannot tell ⭐ *the strongest evidence*

```bash
../redirect -r secret.txt=/does/not/exist -- /bin/cat secret.txt
```

> ### 📸 S16 — the tracee's view of reality
> **Caption:** "Redirected to a path that does not exist, `cat` reports
> **`secret.txt`** — the name it asked for — while the path that actually
> failed was `/does/not/exist`. The program's entire view of the filesystem
> is the one constructed for it. This is the mechanism WSL v1 used to make a
> Linux binary asking for `/etc/hosts` receive
> `C:\Windows\System32\drivers\etc\hosts`."

### Step 5.3 — Longer replacement, subtree rule, and another program

```bash
echo "=== replacement path LONGER than the original ==="
../redirect -r secret.txt=replacement_with_a_very_long_filename.txt -- /bin/cat secret.txt
echo "=== prefix rule: a whole subtree ==="
../redirect -r realdir/=fakedir/ -- /bin/cat realdir/data.txt
echo "=== a different unmodified program ==="
../redirect -r secret.txt=decoy.txt -- /usr/bin/head -1 secret.txt
```

> ### 📸 S17 — robustness of the rewrite
> **Caption:** "Three harder cases. The replacement path is longer than the
> original, which would corrupt the tracee if the string were overwritten in
> place — instead it is staged in scratch space below the tracee's stack
> pointer, with the original bytes saved and restored at the syscall exit
> stop. The prefix rule relocates an entire subtree, as WSL v1's DrvFs did
> for `/mnt/c`. The last line shows it works on `head` as well as `cat` —
> the layer is not specific to any program."

---

## Phase 6 — Question 10: the dispatcher

### Step 6.1 — Interactive session

```bash
../wsl_lite
```

Then type these at the `wsl_lite>` prompt:

```
help
wsl-read sample.txt
wsl-stat sample.txt
wsl-copy sample.txt demo_copy.txt
wsl-list .
exit
```

> ### 📸 S18 — the command table
> Screenshot the output of `help`.
>
> **Caption:** "The dispatch table: each custom command mapped to the raw
> syscall sequence that implements it. WSL v1 had a table of the same shape
> indexed by Linux syscall number, whose entries pointed at the NT-call
> sequence emulating each one."
>
> ### 📸 S19 — a live session
> Screenshot `wsl-read` and `wsl-copy` with their traces.
>
> **Caption:** "Each command prints the system calls it issued and a count.
> `wsl-copy` is eight calls: `openat` ×2, `fstat`, `read`, `write`, `read`
> (returning 0), `close` ×2. No Linux utility is executed — the dispatcher
> issues the calls itself."

### Step 6.2 — Prove it never shells out ⭐

```bash
SC_TRACE=0 ../mini_strace ../wsl_lite wsl-read sample.txt 2>&1 | grep -c execve
```

Must print `0`.

> ### 📸 S20 — verification with my own tracer
> **Caption:** "Question 7's tracer used to verify Question 10.
> `wsl_lite`'s own startup `execve` happens before the tracer attaches, so
> **any** `execve` appearing in the trace would mean it had launched a
> helper program such as `/bin/cat`. The count is zero: the command was
> carried out entirely by `wsl_lite`'s own system calls."

---

## Phase 7 — Package the submission

Regenerate the captured evidence on **your** machine so the output files show
your username, paths and kernel:

```bash
cd ~/tabindasandhu/assignment/partB
make clean && make && make demo
ls -l output/
```

Then build the archive (`install_tools.sh` installs `zip`; if you skipped it,
run `sudo apt install zip` first):

```bash
cd ~/tabindasandhu/assignment
zip -r partB_submission.zip partB \
    -x 'partB/demo_workspace/*' 'partB/*.o' 'partB/mycat' 'partB/mycp' \
       'partB/myls' 'partB/mini_strace' 'partB/redirect' 'partB/wsl_lite' \
       'partB/libopenlog.so' 'partB/syscall_names.h'
```

Copy it to Windows so you can attach it:

```bash
cp partB_submission.zip /mnt/c/Users/$USER/Desktop/
```

(If that path is wrong, run `ls /mnt/c/Users/` to find your Windows username.)

### What to hand in

| Item | Where it comes from |
|---|---|
| Source for Q6–Q10 | `partB/*.c`, `*.h`, `Makefile` |
| Captured syscall traces (Q7 requires these) | `partB/output/q7_mini_strace.txt` |
| The LD_PRELOAD log (Q8 requires it) | `partB/output/q8_ld_preload.txt` |
| Edge-case comparison (Q6 requires it) | `partB/output/q6_raw_syscall_utils.txt` |
| Redirection demo (Q9) | `partB/output/q9_path_redirection.txt` |
| Dispatcher session (Q10) | `partB/output/q10_wsl_lite.txt` |
| Written analysis + WSL v1 comparison + reflection | `partB/README.md` |
| Screenshots S1–S20 with captions | your report document |

---

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `cat noperm.txt` prints the file (S5 fails) | You are root, or the file is on `/mnt/c`. Use a normal user in `~/`. |
| `ptrace: Operation not permitted` | WSL 1, or yama restrictions. Confirm `wsl -l -v` says VERSION 2; `sudo sysctl -w kernel.yama.ptrace_scope=0` if attaching. |
| `make`: `sys/syscall.h: No such file` | `sudo apt install build-essential libc6-dev` |
| The preload log is empty for a program | Expected for statically linked binaries and for anything calling `syscall()` directly — that is the point of **S14**, not a bug. |
| `myls` order differs run to run | Correct. `getdents64` returns filesystem order, which is not stable. |
| `permission denied` running `../mycat` | `chmod +x ../mycat`, or rebuild with `make`. |
| `zip: command not found` | `sudo apt install zip`, or use `tar czf partB_submission.tar.gz partB` instead. |
