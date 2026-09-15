# Environment Setup

Everything in this assignment is Linux-specific: `getopts`, `declare -A`, `trap`,
raw `syscall()`, `ptrace()`, `getdents64`, and `LD_PRELOAD`. None of it runs in
PowerShell or CMD. So step 1 is getting a real Linux userland on the machine
via WSL, and step 2 is installing the toolchain inside it.

---

## Step 1 - Install WSL 2 on Windows

Open **PowerShell as Administrator** (Start → type "PowerShell" → Run as
administrator) and run:

```powershell
wsl --install -d Ubuntu
```

That single command enables the `VirtualMachinePlatform` and
`Microsoft-Windows-Subsystem-Linux` features, downloads the WSL 2 kernel, and
installs Ubuntu. **Reboot when it asks.**

After the reboot, Ubuntu opens and asks for a UNIX username and password. The
password is what `sudo` will ask for later; it is not your Windows password and
it will not echo while you type.

### If `wsl --install` is not recognised (older Windows 10 builds)

```powershell
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart
# reboot, then:
wsl --set-default-version 2
wsl --install -d Ubuntu
```

Download the WSL 2 kernel update package from Microsoft if prompted:
<https://aka.ms/wsl2kernel>

### Verify the install

```powershell
wsl --status
wsl -l -v
```

`wsl -l -v` must show `VERSION 2` next to Ubuntu. If it shows `1`:

```powershell
wsl --set-version Ubuntu 2
```

WSL 1 is not enough here. Question 7 and Question 9 need `ptrace()` with
`PTRACE_GETREGS` and `PTRACE_SYSCALL`, and Question 6 needs `getdents64`. WSL 1
is itself a syscall translation layer (exactly the thing Part B is about) and
its `ptrace` support is incomplete — so ironically, the assignment about
recreating WSL 1 cannot be done on WSL 1.

### Keep it current

```powershell
wsl --update
wsl --shutdown
```

---

## Step 2 - Install the toolchain inside WSL

Open the **Ubuntu** app (not PowerShell) and run:

```bash
cd ~
git clone <this-repo-url> tabindasandhu
cd tabindasandhu
bash assignment/setup/install_tools.sh
```

The script installs, and states why each is needed:

| Package | Needed for |
|---|---|
| `build-essential`, `gcc`, `make`, `libc6-dev` | Q6-Q10 — compiling C against raw syscalls |
| `linux-libc-dev` | Q6 — `getdents64` / `linux_dirent64` layout |
| `gdb` | Q7, Q9 — debugging the tracer when registers look wrong |
| `strace` | Q7 — the reference output your `mini_strace` is compared against |
| `ltrace` | Q8 — library-call tracing, the contrast to syscall tracing |
| `binutils` | `objdump` / `readelf` for inspecting your binaries |
| `coreutils` | Q2 `md5sum`, Q3 `timeout`, timestamps everywhere |
| `procps` | Q1 — `ps`, `free`, `vmstat` for `check_cpu` / `check_memory` |
| `sysstat` | Q1 — `mpstat`, a second CPU measurement source |
| `bsdmainutils` | Q3 — `column` for the summary table |
| `bc` | Q1 — floating-point threshold comparisons |
| `rsync`, `tar`, `gzip` | Q2 — `smart_backup.sh` |
| `shellcheck` | Q1-Q5 — lint every script before submitting |
| `manpages-dev` | `man 2 openat`, `man 2 ptrace`, `man 2 getdents64` |

---

## Step 3 - Verify

```bash
bash assignment/setup/verify_env.sh
```

This does more than check that binaries exist. It compiles and runs a miniature
version of each Part B technique:

- a `syscall(SYS_write, ...)` call — proves Q6 is possible
- `openat` + `getdents64` + `close` — proves `myls` is possible
- a real `fork()` + `PTRACE_TRACEME` + `PTRACE_SYSCALL` + `PTRACE_GETREGS`
  tracer that reports the first syscall number of `/bin/true` — proves Q7/Q9
- a `.so` built with `dlsym(RTLD_NEXT, "open")` — proves Q8
- a check that `/bin/cat` is dynamically linked, since `LD_PRELOAD` has no
  effect on static binaries

It exits `0` only when every required check passes.

---

## Step 4 - Two things that will bite you

**1. Do not keep the files under `/mnt/c`.**

Work in the Linux filesystem (`~/tabindasandhu`), not `/mnt/c/Users/...`. The
Windows drives are mounted through DrvFs, which does not honour Linux file
permissions properly. Question 6 explicitly asks you to test *"a file with no
read permission"* — under `/mnt/c`, `chmod 000 file` silently does nothing and
the read succeeds, so that edge case cannot be demonstrated. File I/O there is
also several times slower.

**2. `ptrace` scope.**

`fork()` + `PTRACE_TRACEME`, which is what Q7 and Q9 use, works under any
setting. Only attaching to an *already-running* process (`PTRACE_ATTACH`) is
restricted. If you need that:

```bash
sudo sysctl -w kernel.yama.ptrace_scope=0     # until next reboot
```

---

## Optional - editor integration

To edit these files in VS Code on Windows while they live inside WSL:

```powershell
winget install Microsoft.VisualStudioCode
code --install-extension ms-vscode-remote.remote-wsl
```

Then from the Ubuntu shell, inside the repo:

```bash
code .
```
