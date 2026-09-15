# Part B — Build Your Own Mini WSL v1

Wrapping kernel system calls: raw `syscall()` utilities, a `ptrace()` tracer,
an `LD_PRELOAD` shim, a path-translation layer, and a dispatcher that ties
them together.

## Contents

| File | Question | What it is |
|---|---|---|
| `sc_trace.c` / `.h` | 6, 10 | Traced wrappers around raw system calls |
| `q6_mycat.c` | 6 | `cat` built on `openat`/`fstat`/`read`/`write`/`close` |
| `q6_mycp.c` | 6 | `cp` built on the same calls |
| `q6_myls.c` | 6 | `ls` built on `openat(O_DIRECTORY)`/`getdents64` |
| `ptrace_common.c` / `.h` | 7, 9 | Syscall decoding and tracee-memory access |
| `gen_syscall_names.sh` | 7, 9 | Generates the syscall-number → name table |
| `q7_mini_strace.c` | 7 | The tracer |
| `q8_open_logger.c` | 8 | `LD_PRELOAD` shared library wrapping `open`/`openat` |
| `q9_redirect.c` | 9 | The path-translation layer |
| `q10_wsl_lite.c` | 10 | The dispatcher |
| `run_demos.sh` | all | Runs every demonstration, captures `output/` |

## Building

```bash
make            # build everything
make demo       # build, then run every demonstration into output/
make clean
```

Requires an x86-64 Linux userland — WSL 2 or native. See `../setup/SETUP.md`
for the environment install; `../setup/verify_env.sh` confirms the machine can
do all of this before you start. **WSL 1 will not work**, because its `ptrace`
support is incomplete — which is itself a nice comment on Part B's subject.

---

# Question 6 — Bypassing the standard library

## What "raw" means here

`fopen`, `fread`, `opendir` and friends are library code. They keep buffers in
user space, batch your reads, cache directory entries, and eventually call the
kernel on your behalf. These three utilities skip all of it and issue
`syscall(SYS_openat, ...)`, `syscall(SYS_read, ...)`, `syscall(SYS_getdents64, ...)`
directly.

One deliberate exception: `snprintf` is used to *format text into a buffer*.
It performs no I/O and issues no system call — the buffer is then handed to
the kernel by `syscall(SYS_write, ...)`. Every byte that leaves these programs
goes through exactly one function, `raw_write()` in `sc_trace.c`.

Because `opendir`/`readdir` are off the table, `myls` also has to declare the
kernel's own directory-entry layout itself:

```c
struct linux_dirent64 {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;   /* records are variable length */
    unsigned char      d_type;
    char               d_name[];
};
```

`getdents64` returns a **byte count**, not an entry count, and packs
variable-length records into the buffer. Walking it by hand — `off +=
d->d_reclen` — is precisely the work `readdir()` normally hides.

## The trace

Every wrapper prints the call, its arguments and its return value:

```
$ ./mycat sample.txt
[syscall] openat(AT_FDCWD, "sample.txt", O_RDONLY)         = 3
[syscall] fstat(3, {mode=0100644, size=29})                = 0
[syscall] read(3, 0x7ffd04867c60, 65536)                   = 29
line one
line two
line three
[syscall] write(1, 0x7ffd04867c60, 29)                     = 29
[syscall] read(3, 0x7ffd04867c60, 65536)                   = 0
[syscall] close(3)                                         = 0
[syscall] ---- 6 system calls issued ----
```

Set `SC_TRACE=0` to silence the trace and use the programs as plain utilities —
which is how the output-equivalence checks in `run_demos.sh` compare them
against the real thing.

## Edge-case comparison

Full captured output: **`output/q6_raw_syscall_utils.txt`**.

### 1. An empty file — `mycat empty.txt` vs `cat empty.txt`

| | Output | Exit |
|---|---|---|
| `cat` | nothing | 0 |
| `mycat` | nothing | 0 |

**Identical, and the trace explains why.** The first `read()` returns 0, the
loop ends immediately, nothing is ever written:

```
[syscall] openat(AT_FDCWD, "empty.txt", O_RDONLY)          = 3
[syscall] fstat(3, {mode=0100644, size=0})                 = 0
[syscall] read(3, 0x7ffebc6d17e0, 65536)                   = 0
[syscall] close(3)                                         = 0
```

Four syscalls instead of the six a non-empty file needs. "End of file" is not a
special condition or an error — it is just `read()` returning 0, and that is
the *only* way a program learns a file has ended.

### 2. A file with no read permission — `chmod 000 noperm.txt`

| | Output | Exit |
|---|---|---|
| `cat` | `cat: noperm.txt: Permission denied` | 1 |
| `mycat` | `mycat: noperm.txt: Permission denied` | 1 |

**Same behaviour, and it must be**, because the decision is not made by either
program. The kernel refuses at `openat` and returns `-EACCES`; both programs can
only report what they were told:

```
[syscall] openat(AT_FDCWD, "noperm.txt", O_RDONLY)         = -1 EACCES (Permission denied)
```

Two syscalls and it is over. Getting the message to *look* like `cat`'s took
one deliberate choice: printing `sc_basename(argv[0])` rather than `argv[0]`,
so the program identifies itself as `mycat`, not `/long/path/to/mycat`.

> Note: this case cannot be demonstrated as **root**, which bypasses permission
> checks entirely — `cat` would simply print the file. `run_demos.sh` detects
> this and re-runs the two commands through `setpriv` as an unprivileged user.

### 3. An empty directory — `myls emptydir` vs `ls emptydir`

| | Output |
|---|---|
| `ls` | nothing |
| `myls` | nothing (plus a note on stderr) |
| `ls -a` | `.` and `..` |
| `myls -a` | `.` and `..` |

**The outputs agree, but only because `myls` reproduces a convention that lives
entirely in user space.** The trace shows what the kernel actually returned:

```
[syscall] openat(AT_FDCWD, "emptydir", O_RDONLY|O_DIRECTORY) = 3
[syscall] getdents64(3, 0x7ffd9c417e70, 65536)             = 48
[syscall] getdents64(3, 0x7ffd9c417e70, 65536)             = 0
[syscall] close(3)                                         = 0
```

**48 bytes, not 0.** An "empty" directory is not empty at the kernel level — it
contains `.` and `..`. Hiding dot-entries is a decision `ls` makes, not a fact
the kernel reports, and `myls` only matches `ls` because it was written to skip
names starting with `.`. This is the clearest illustration in the whole of
Question 6 of how much of a familiar command's behaviour is convention rather
than kernel semantics.

### 4. `cat` on a directory (an extra case worth the trace)

Both print `Is a directory` and exit 1, but the trace shows something
counter-intuitive:

```
[syscall] openat(AT_FDCWD, "emptydir", O_RDONLY)           = 3     <-- SUCCEEDS
[syscall] fstat(3, {mode=040755, size=4096})               = 0
mycat: emptydir: Is a directory
```

`openat()` on a directory **succeeds**. The failure only surfaces at the first
`read()`, which returns `EISDIR`. That is exactly why real `cat` calls `fstat`
on everything it opens, and why `mycat` does too — without it, the error would
arrive one step later and read less clearly.

### 5. Ordering — the difference `ls` hides most successfully

```
--- ls (sorted by ls itself) ---     --- myls (kernel order) ---
empty.txt                            emptydir
emptydir                             noperm.txt
noperm.txt                           sample.txt
sample.txt                           empty.txt
script.sh                            script_copy.sh
script_copy.sh                       script.sh
```

`getdents64` returns entries in whatever order the filesystem stores them —
typically hash order on ext4. **The alphabetical listing you have seen your
whole life is `ls` sorting in user space**, not something the kernel provides.
`myls` prints the raw order, so the difference finally becomes visible.

### 6. `mycp` — mode preservation

`mycp` `fstat`s the source and passes `st_mode & 07777` as the `mode` argument
to `openat(..., O_CREAT, ...)`, so a copy of a 0755 script is still 0755. Skip
that step and every copy silently comes out 0644 — an executable would lose
its `+x` bit. The trace makes the mechanism explicit:

```
[syscall] fstat(3, {mode=0100755, size=18})                = 0
[syscall] openat(AT_FDCWD, "script_copy.sh", O_WRONLY|O_CREAT|O_TRUNC, 0755) = 4
```

### Summary of differences

| Difference | Where it comes from |
|---|---|
| `ls` sorts, `myls` does not | `ls` sorts in user space; the kernel does not |
| `ls` hides `.`/`..`, kernel returns them | A user-space convention |
| `openat` on a directory succeeds | Failure is deferred to `read` → `EISDIR` |
| Permission errors identical | The kernel decides, not the program |
| `cat` uses a large buffer, sometimes `mmap` | An optimisation; behaviour is unchanged |
| Real `cat` issues tens of syscalls, `mycat` 6 | Most of `cat`'s are dynamic-linker startup |

That last row is worth dwelling on. Printing a 29-byte file takes `mycat` six
system calls; the real `cat` takes several dozen, and **only about six of them
do the actual work**. The rest are the dynamic loader mapping libc before
`main()` ever runs. `mycat` looks lean only because the comparison is unfair —
it is dynamically linked too, but its own tracing does not begin until its
startup is over. Question 7 measures the honest figure from outside.

### Which `cat` is on the machine matters

Ubuntu 25.10 and later ship the **Rust `uutils` reimplementation** of coreutils
as the default, not GNU coreutils. Both were used while developing this
assignment, and they are not syscall-for-syscall identical:

| | GNU `cat` | uutils `cat` |
|---|---|---|
| Opens the file with | `openat` | `open64` |
| Before doing any work | — | loads Fluent locale files from `/usr/share/coreutils/locales/` |
| Syscalls for a 29-byte file | ~44 | differs |

Neither is more correct; they are two implementations of the same specification,
which is the point. The kernel-level behaviour `mycat` was written against —
`openat` succeeding on a directory, `read` returning 0 at end of file, `-EACCES`
from the kernel rather than from the program — is identical for both, because
that behaviour belongs to the kernel and not to whoever wrote the utility.
Run `cat --version` to see which one this machine has.

---

# Question 7 — A syscall tracer with `ptrace()`

## How it works

```
parent (mini_strace)                child (target)
────────────────────                ──────────────
fork() ─────────────────────────▶   ptrace(PTRACE_TRACEME)
waitpid() ◀── stop on exec ──────   execvp("/bin/ls")
PTRACE_SETOPTIONS
   │
   ├─ PTRACE_SYSCALL ──────────▶    runs until it enters a syscall
   │  waitpid()  ◀───── stop        ENTRY:  read orig_rax + args
   │
   └─ PTRACE_SYSCALL ──────────▶    kernel executes the syscall
      waitpid()  ◀───── stop        EXIT:   read rax (return value)
                                    ... repeat ...
```

Four details matter:

- **`orig_rax`, not `rax`.** By the time we see the entry stop, the kernel has
  already overwritten `rax` with `-ENOSYS`. The original syscall number is
  preserved in `orig_rax` for exactly this reason.
- **`r10`, not `rcx`.** The syscall ABI passes arguments in
  `rdi, rsi, rdx, r10, r8, r9` — `r10` where the normal function ABI would use
  `rcx`, because the `SYSCALL` instruction clobbers `rcx` with the return
  address.
- **`PTRACE_O_TRACESYSGOOD`** sets bit `0x80` in the stop signal so a syscall
  stop can be told apart from a `SIGTRAP` the program raised itself.
- **`PTRACE_O_EXITKILL`** makes the kernel kill the tracee if the tracer dies,
  so a crash never leaves a stopped orphan process behind.

Entry and exit alternate strictly, so a single toggling flag distinguishes
them. The entry line is printed without a newline and the return value is
appended at the exit stop, giving `strace`'s one-line-per-call format.

## The name table

Rather than hand-typing 350 entries that go stale every kernel release,
`gen_syscall_names.sh` asks the preprocessor what it knows:

```bash
echo '#include <sys/syscall.h>' | gcc -E -dM -x c - \
  | awk '/^#define __NR_[A-Za-z0-9_]+ [0-9]+$/ { ... }'
```

and emits a designated-initialiser array. On this machine that is **374
syscalls**, correct for whatever kernel is installed.

## Captured traces

Full output: **`output/q7_mini_strace.txt`** — four targets:
`/bin/ls`, `/bin/cat sample.txt`, our own `mycat`, and a `-c` count summary.

```
$ ./mini_strace /bin/cat sample.txt
mini_strace: tracing pid 549 (/bin/cat)
brk(0x0, ...)                                 = 0x5602bc3d5000
access("/etc/ld.so.preload", ...)             = -1 ENOENT (No such file or directory)
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
fstat(3, 0x7fffedd0f260)                      = 0
mmap(0x0, 34091, 0x1, 0x2, 3, 0)              = 0x7f967e1c2000
close(3)                                      = 0
openat(AT_FDCWD, "/lib/x86_64-linux-gnu/libc.so.6", O_RDONLY|O_CLOEXEC) = 3
...
openat(AT_FDCWD, "sample.txt", O_RDONLY)      = 3
read(3, 0x7fcc42462000, 131072)               = 29
write(1, 0x7fcc42462000, 29)                  = 29
read(3, 0x7fcc42462000, 131072)               = 0
close(3)                                      = 0
exit_group(0)
+++ exited with 0 +++
```

The trace shows the whole life of the process: the dynamic linker looking for
`/etc/ld.so.preload` (the same mechanism Question 8 exploits), mapping
`ld.so.cache` and libc, and only then the four calls that actually print the
file.

**Cross-check against the real thing.** On `/bin/cat sample.txt`:

| | Total syscalls counted |
|---|---|
| `strace -c` | 44 |
| `./mini_strace -c` | **44** |

An exact match, which is the strongest evidence available that the tracer is
neither missing stops nor double-counting them.

The absolute figure is machine-specific — it depends on the libc version and on
whether `cat` is GNU or uutils — so it is not a number to memorise. What matters
is that **the two tools agree with each other on the same binary**: had the
tracer skipped stops the total would come out low, and had it counted entry and
exit as two separate calls it would come out at roughly double.

---

# Question 8 — Wrapping library calls with `LD_PRELOAD`

## How it works

The dynamic linker resolves each symbol to the **first** object that defines
it. `LD_PRELOAD` inserts our library ahead of libc in that search order, so a
program's call to `open()` binds to ours. We log it and forward to the genuine
implementation, found with `dlsym(RTLD_NEXT, "open")` — "the next definition
after me".

```
cat's code ──calls open()──▶ libopenlog.so (ours) ──dlsym(RTLD_NEXT)──▶ libc's open() ──▶ kernel
                                    │
                                    └──▶ /tmp/open_trace.log
```

No recompilation, no source access, no cooperation from `cat` whatsoever.

## Two traps a naive version falls into

1. **Infinite recursion.** If the logging code called `open()` or `fopen()` to
   write its log, that call would resolve straight back into our own library
   and recurse until the stack died. The shim writes its log with **raw
   syscalls** — `syscall(SYS_openat, ...)` and `syscall(SYS_write, ...)` — which
   bypass symbol resolution completely. Question 6's technique is what makes
   Question 8 safe.

2. **Re-entrancy.** Libc helpers we call while logging can open files
   themselves — `localtime_r` reads `/etc/localtime` for the timestamp. A
   `__thread` guard flag stops that becoming a nested log record.

`open()` alone catches very little on a current system: glibc implements
`open()` in terms of `openat()`, GNU coreutils call `openat()` directly, and
the Rust uutils build that Ubuntu 25.10 ships calls `open64()`. A shim
overriding only `open` would therefore log almost nothing on one system and
nothing at all on another. This one overrides `open`, `open64`, `openat` and
`fopen`. Variadic
handling matters too — the `mode` argument only exists when `O_CREAT` or
`O_TMPFILE` is set, so `va_arg` is only read in that case.

## Demonstration

Full output: **`output/q8_ld_preload.txt`** — unmodified `cat`, `head`, `wc`
and `grep -r`.

```bash
$ LD_PRELOAD=./libopenlog.so cat sample.txt
$ cat /tmp/open_trace.log
2026-09-15 15:26:20.071 pid=928  open   "sample.txt" flags=0x0 (O_RDONLY) -> fd 3
2026-09-15 15:26:20.076 pid=931  openat "." flags=0x90900 (O_RDONLY|O_NOCTTY|O_NONBLOCK|O_DIRECTORY|O_CLOEXEC) -> fd 3
2026-09-15 15:26:20.076 pid=931  openat "emptydir" flags=0xb0900 (...|O_NOFOLLOW|O_CLOEXEC) -> fd 3
2026-09-15 15:26:20.078 pid=933  open   "/definitely/not/here" flags=0x0 (O_RDONLY) -> FAILED (No such file or directory)
```

Failures are logged too, and `grep -r` shows the flags it uses to walk a tree
safely — `O_DIRECTORY|O_NOFOLLOW` so a symlink cannot redirect its descent.

## What `LD_PRELOAD` *cannot* see — and why that matters for Question 9

Compare the log above with Question 7's trace of the same command. The tracer
shows `openat("/etc/ld.so.cache")` and `openat("/lib/x86_64-linux-gnu/libc.so.6")`;
**the `LD_PRELOAD` log does not.** Those opens are performed by the dynamic
linker itself, before our library is loaded, and through internal calls that
never go near the PLT.

The two techniques intercept at different boundaries:

| | Boundary | Sees | Defeated by |
|---|---|---|---|
| `LD_PRELOAD` (Q8) | library call | dynamically linked calls through the PLT | static linking, direct `syscall()`, libc-internal calls |
| `ptrace` (Q9) | kernel entry | **everything**, without exception | nothing — the kernel is the only way out |

`mycat` from Question 6 makes the point concrete: preload the shim against it
and the log stays empty, because it calls `syscall()` directly. Run it under
`mini_strace` and every call is there. **This is exactly why Question 9 builds
the translation layer on `ptrace` and not on `LD_PRELOAD`** — and why WSL v1 had
to intercept at the kernel boundary rather than by swapping a library.

---

# Question 9 — Translating system calls: a path-redirection layer

## From observing to intervening

Question 7's tracer looked and moved on. `redirect` acts while the tracee is
stopped at the syscall **entry**, before the kernel has done anything:

1. Read the path out of the tracee with `PTRACE_PEEKDATA`.
2. If a rule matches, compute the replacement.
3. Write the replacement into the tracee's memory.
4. Point the syscall's path register at it and `PTRACE_SETREGS`.
5. Let the syscall run — the kernel sees only the new path.
6. At the exit stop, restore the memory and the register.

## The hard part: the replacement is a different length

The obvious approach — overwrite the original string in place — corrupts the
tracee as soon as the new path is longer than the old one, because whatever
follows that buffer gets trampled.

Instead, `redirect` borrows scratch space **below the tracee's stack pointer**:

```
         tracee stack
    ┌────────────────────┐
    │ live frames        │
    ├────────────────────┤ ◀── rsp
    │ red zone (128 B)   │
    ├────────────────────┤ ◀── rsp - 512
    │ our new path       │     saved first, restored at the syscall exit
    └────────────────────┘
```

Nothing live lives there, and nothing can grow into it while the process sits
stopped inside a syscall. The original bytes are saved via `/proc/<pid>/mem`
before being overwritten and put back at the exit stop. By then the kernel has
already copied the string into kernel space, so the restore is safe *and*
invisible. Both memory-access methods the question mentions are used:
`PTRACE_PEEKDATA` to read the path, `/proc/<pid>/mem` to read and write the
scratch area in one go rather than word by word.

## Demonstration

Full output: **`output/q9_path_redirection.txt`**.

```bash
$ cat secret.txt
THIS IS THE REAL SECRET FILE

$ ./redirect -r secret.txt=decoy.txt -- /bin/cat secret.txt
redirect: rule 1: "secret.txt" -> "decoy.txt"
[redirect] "secret.txt" -> "decoy.txt"
this is the harmless decoy file
+++ exited with 0 +++
```

Same binary, same arguments, different file — and `/bin/cat` was neither
recompiled nor consulted. Also demonstrated:

- a replacement path **much longer** than the original, proving the scratch-space
  approach;
- **prefix rules** (`-r realdir/=fakedir/`) that relocate a whole subtree;
- redirecting a system file the program trusts (`/etc/hostname`);
- a different unmodified program (`/usr/bin/head`).

**The proof that the tracee cannot tell** is the most convincing case. Redirect
to a path that does not exist:

```bash
$ ./redirect -r secret.txt=/does/not/exist -- /bin/cat secret.txt
[redirect] "secret.txt" -> "/does/not/exist"
/bin/cat: secret.txt: No such file or directory
```

`cat` reports **`secret.txt`** — the name it asked for — while the path that
actually failed was `/does/not/exist`. Its view of reality is entirely the one
we constructed for it.

## How this mirrors WSL v1

WSL v1 ran unmodified Linux ELF binaries on Windows **with no Linux kernel
anywhere**. A component called `lxss.sys`, a kernel driver, registered itself
to receive the Linux `SYSCALL` instructions those binaries issued and serviced
each one using Windows NT facilities instead.

The structure of that is the structure of this program:

| | `redirect` (Q9) | WSL v1 |
|---|---|---|
| Who is intercepted | an unmodified ELF binary | an unmodified ELF binary |
| Where | at the syscall boundary | at the syscall boundary |
| What is rewritten | the path argument | the entire call: number, arguments, semantics |
| Translated into | the same syscall, different path | the equivalent NT call (`NtCreateFile`, …) |
| Does the program know | no | no |
| Cost | two context switches per call | one kernel transition, but emulation work per call |

Path translation is not an analogy for WSL v1 — it is a *component* of it. A
Linux program asking for `/etc/hosts` had to be given
`C:\Windows\System32\drivers\etc\hosts`; `/mnt/c/Users/x` had to become
`\??\C:\Users\x`. WSL v1's VolFs and DrvFs layers did exactly what `translate()`
in `q9_redirect.c` does, just with a much larger and more intricate rule set,
and with the path being only one of many things that had to be converted.

The difference is scope and depth. `redirect` changes one argument and lets
Linux handle the rest. WSL v1 had no Linux underneath to fall back on: it had
to reimplement `fork`, `openat`, `getdents64`, signals, pipes, `/proc` and the
rest on top of NT primitives whose semantics were never designed to match. The
principle is identical — **intercept at the syscall boundary, rewrite, let it
proceed** — but where this program substitutes one path for another, WSL v1
substituted one operating system for another.

---

# Question 10 — A mini WSL-style command dispatcher

## Design

`wsl_lite` reads commands in its own vocabulary and carries each one out by
issuing raw system calls itself. It never runs `cat`, `ls` or `cp`.

```
user types "wsl-read notes.txt"
        │
        ▼
   tokenize()
        │
        ▼
   dispatch table  ──┬─▶ wsl-read   → openat, fstat, read*, write*, close
                     ├─▶ wsl-list   → openat(O_DIRECTORY), getdents64*, close
                     ├─▶ wsl-copy   → openat×2, fstat, read*, write*, close×2
                     ├─▶ wsl-stat   → openat, fstat, close
                     └─▶ wsl-write  → openat(O_CREAT|O_TRUNC), write*, close
        │
        ▼
   sc_* wrappers ──▶ syscall() ──▶ kernel
        │
        └──▶ the trace, printed as it happens
```

The dispatch table is the design's centrepiece, and it is deliberate: WSL v1
had a table of the same shape, indexed by Linux syscall number, whose entries
pointed at the NT-call sequence emulating each one. Ours is indexed by command
name and its entries point at raw-syscall sequences, but it is doing the same
job — mapping a request in one vocabulary onto operations in another.

Run it and every command reports its own kernel traffic:

```
wsl_lite> wsl-copy sample.txt copied.txt
--- wsl-copy: issuing system calls ---
[syscall] openat(AT_FDCWD, "sample.txt", O_RDONLY)         = 3
[syscall] fstat(3, {mode=0100644, size=29})                = 0
[syscall] openat(AT_FDCWD, "copied.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644) = 4
[syscall] read(3, 0x7ffd..., 65536)                        = 29
[syscall] write(4, 0x7ffd..., 29)                          = 29
[syscall] read(3, 0x7ffd..., 65536)                        = 0
[syscall] close(3)                                         = 0
[syscall] close(4)                                         = 0
--- wsl-copy: 8 system calls, exit 0 ---
```

One design decision worth stating: the dispatcher's own reads of your
keystrokes are **not** routed through the traced wrappers. They are the
program's own I/O, not work done on a command's behalf, and logging them would
bury the very mapping the program exists to show.

`help` prints the command-to-syscall table directly. Full session output:
**`output/q10_wsl_lite.txt`**.

## Verifying it really bypasses the utilities

The claim "it does not call `cat`" is checkable with Question 7's tracer:

```bash
$ ./mini_strace ./wsl_lite wsl-read sample.txt
openat(AT_FDCWD, "sample.txt", O_RDONLY)  = 3
read(3, 0x7ffece293080, 65536)            = 29
write(1, 0x7ffece293080, 29)              = 29
read(3, 0x7ffece293080, 65536)            = 0
close(3)                                  = 0
exit_group(0)
```

`wsl_lite`'s own startup `execve` happens before the tracer attaches, so **any**
`execve` in that trace would mean it had launched a helper program. The count
is zero. The work is done entirely by its own system calls — the three
techniques of Part B validating each other.

## Reflection

`wsl_lite` resembles WSL v1 in the one respect that defined it: there is no
guest kernel anywhere in the design, only a dispatcher that receives a request
in one vocabulary and services it by issuing calls in another, with a table in
the middle deciding which translation applies. Where WSL v1's `lxss.sys` caught
a Linux `SYSCALL` instruction and satisfied it with NT calls such as
`NtCreateFile` and `NtReadFile`, `wsl_lite` catches a typed command and
satisfies it with `openat` and `read` — and Question 9's `redirect` demonstrates
the harder half of that job, rewriting a call in flight while the program that
made it remains none the wiser. WSL v2 abandoned this approach entirely:
it ships a genuine Linux kernel inside a lightweight Hyper-V virtual machine, so
a Linux syscall is handled by real Linux code and nothing needs translating at
all. The trade-off is visible even at this scale — my dispatcher only has to be
correct for five commands and a handful of syscalls, whereas WSL v1 had to
reimplement the semantics of hundreds of them, faithfully enough that
unmodified binaries could not tell the difference.

That is where the translation approach broke down. **The concrete reason
Microsoft moved to WSL v2 was filesystem performance**, and it is a direct
consequence of translation rather than an implementation flaw. Linux workloads
are metadata-intensive — `git status`, `npm install` and `./configure` issue
tens or hundreds of thousands of small `stat`, `openat` and `getdents64` calls —
and each one had to be emulated on top of NTFS, whose per-file-operation cost is
considerably higher than ext4's and which sits behind a filter-driver stack that
antivirus software also hooks. Operations that take seconds on native Linux took
minutes under WSL v1, and no amount of optimisation could close a gap that came
from the mismatch itself. Running a real ext4 filesystem inside a real kernel in
a VM made that cost disappear, at the price of the seamless Windows filesystem
access WSL v1 had enjoyed for free — which is why Microsoft still recommends
keeping WSL v2 project files inside the Linux filesystem rather than on `/mnt/c`,
the same advice this assignment's own setup notes give.

Secondary pressures pushed the same way: full syscall coverage. WSL v1 never
implemented the complete Linux ABI, so anything reaching for an untranslated
corner of it — `FUSE`, raw sockets, `iptables`, and above all Docker, which
depends on cgroups and namespace behaviour that NT simply does not have — either
failed or behaved subtly differently. With a real kernel, that entire category of
bug stops existing, because there is no longer any translation that can be
incomplete.
