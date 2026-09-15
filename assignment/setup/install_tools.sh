#!/usr/bin/env bash
#
# install_tools.sh - installs every tool needed for the OS assignment
#                    (Part A: shell scripting, Part B: syscalls/ptrace/LD_PRELOAD)
#
# RUN THIS INSIDE WSL (Ubuntu/Debian), NOT in Windows PowerShell:
#     bash assignment/setup/install_tools.sh
#
set -euo pipefail

BOLD=$'\033[1m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'; RED=$'\033[31m'; RESET=$'\033[0m'

info()  { printf '%s[*]%s %s\n' "$BOLD" "$RESET" "$*"; }
ok()    { printf '%s[+]%s %s\n' "$GREEN" "$RESET" "$*"; }
warn()  { printf '%s[!]%s %s\n' "$YELLOW" "$RESET" "$*"; }
fail()  { printf '%s[x]%s %s\n' "$RED" "$RESET" "$*" >&2; }

# ---------------------------------------------------------------- sanity checks
if [[ "$(uname -s)" != "Linux" ]]; then
    fail "This script must run inside WSL/Linux, not Windows PowerShell."
    exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
    fail "No apt-get found. This script targets Ubuntu/Debian WSL distros."
    fail "On Fedora/Arch install the equivalents of the PACKAGES list manually."
    exit 1
fi

if ! grep -qi microsoft /proc/version 2>/dev/null; then
    warn "Not detected as WSL - continuing anyway (plain Linux works too)."
fi

SUDO=""
if [[ $EUID -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    else
        fail "Not root and sudo is unavailable."
        exit 1
    fi
fi

# ------------------------------------------------------------------- packages
# Grouped by which question needs them, so the report can justify each one.
PACKAGES=(
    # --- toolchain: Q6-Q10 compile C against raw syscalls ---
    build-essential          # gcc, g++, make, libc6-dev (brings <sys/syscall.h>)
    gcc
    make
    libc6-dev
    linux-libc-dev           # kernel uapi headers: <linux/fcntl.h>, dirent64 layout

    # --- debugging / tracing: Q7, Q8, Q9 ---
    gdb                      # inspect the tracee when ptrace misbehaves
    strace                   # reference implementation to compare mini_strace against
    ltrace                   # library-call tracer, useful contrast for the LD_PRELOAD part
    binutils                 # objdump / readelf / nm

    # --- shell scripting: Q1-Q5 ---
    coreutils                # md5sum, timeout, date, stat, df
    procps                   # ps, top, free, vmstat  -> check_cpu/check_memory
    sysstat                  # mpstat/iostat, a second CPU source for resource_monitor.sh
    bsdmainutils             # `column`, used for the Q3 summary table
    bc                       # float comparison of CPU/memory thresholds
    rsync                    # smart_backup.sh file copying
    tar
    gzip
    shellcheck               # lint every .sh before submitting

    # --- docs & convenience ---
    man-db
    manpages
    manpages-dev             # man 2 openat, man 2 getdents64, man 2 ptrace
    git
    vim
    nano
    tree
    file
    curl
)

info "Updating package lists..."
$SUDO apt-get update -y

info "Installing ${#PACKAGES[@]} packages (this takes a few minutes)..."
# Install one-by-one so a single unavailable package in a given Ubuntu release
# does not abort the whole run.
missing_pkgs=()
for pkg in "${PACKAGES[@]}"; do
    if $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y "$pkg" >/dev/null 2>&1; then
        ok "installed: $pkg"
    else
        warn "could not install: $pkg"
        missing_pkgs+=("$pkg")
    fi
done

# ------------------------------------------------------- ptrace permission (Q7/Q9)
# fork()+PTRACE_TRACEME (what Q7 and Q9 use) is allowed under any yama setting,
# but attaching to an already-running process needs ptrace_scope=0.
if [[ -e /proc/sys/kernel/yama/ptrace_scope ]]; then
    scope=$(cat /proc/sys/kernel/yama/ptrace_scope)
    if [[ "$scope" != "0" ]]; then
        warn "yama ptrace_scope=$scope - PTRACE_ATTACH to running processes is restricted."
        warn "Your fork()+TRACEME tracer still works. To relax it for this boot:"
        warn "    sudo sysctl -w kernel.yama.ptrace_scope=0"
    else
        ok "yama ptrace_scope=0 (PTRACE_ATTACH permitted)"
    fi
else
    ok "no yama LSM - ptrace unrestricted"
fi

# --------------------------------------------------------------------- summary
echo
if (( ${#missing_pkgs[@]} == 0 )); then
    ok "All packages installed."
else
    warn "Skipped ${#missing_pkgs[@]} package(s): ${missing_pkgs[*]}"
    warn "They are optional; the assignment still builds without them."
fi
info "Now run:  bash assignment/setup/verify_env.sh"
