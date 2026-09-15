# Part B - Build Your Own Mini WSL v1
#
#   make            build everything
#   make demo       build, then run every demonstration and capture the output
#   make clean      remove binaries and generated files
#
# Everything here is x86-64 Linux specific (register names, syscall numbers).
# Run it inside WSL 2 or a native Linux box - see ../setup/SETUP.md.

CC      := gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -O2 -g
LDFLAGS :=

# Question 6 utilities and Question 10 dispatcher share the raw-syscall
# tracing wrappers; Questions 7 and 9 share the ptrace machinery.
SC_OBJS     := sc_trace.o
PTRACE_OBJS := ptrace_common.o

BINARIES := mycat mycp myls mini_strace redirect wsl_lite
LIBS     := libopenlog.so

.PHONY: all demo clean help

all: $(BINARIES) $(LIBS)

# ---------------------------------------------------------------- generated
# The syscall number -> name table is generated from the system headers
# rather than hand-typed, so it is correct for whatever kernel is installed.
syscall_names.h: gen_syscall_names.sh
	./gen_syscall_names.sh > $@

ptrace_common.o: ptrace_common.c ptrace_common.h syscall_names.h
	$(CC) $(CFLAGS) -c -o $@ $<

sc_trace.o: sc_trace.c sc_trace.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ------------------------------------------------------ Q6: raw-syscall utils
mycat: q6_mycat.c $(SC_OBJS) sc_trace.h
	$(CC) $(CFLAGS) -o $@ q6_mycat.c $(SC_OBJS) $(LDFLAGS)

mycp: q6_mycp.c $(SC_OBJS) sc_trace.h
	$(CC) $(CFLAGS) -o $@ q6_mycp.c $(SC_OBJS) $(LDFLAGS)

myls: q6_myls.c $(SC_OBJS) sc_trace.h
	$(CC) $(CFLAGS) -o $@ q6_myls.c $(SC_OBJS) $(LDFLAGS)

# ------------------------------------------------------------ Q7: the tracer
mini_strace: q7_mini_strace.c $(PTRACE_OBJS) ptrace_common.h
	$(CC) $(CFLAGS) -o $@ q7_mini_strace.c $(PTRACE_OBJS) $(LDFLAGS)

# ------------------------------------------------------- Q8: LD_PRELOAD shim
# -shared -fPIC makes a loadable object; -ldl provides dlsym().
libopenlog.so: q8_open_logger.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ q8_open_logger.c -ldl

# -------------------------------------------------- Q9: path-redirection layer
redirect: q9_redirect.c $(PTRACE_OBJS) ptrace_common.h
	$(CC) $(CFLAGS) -o $@ q9_redirect.c $(PTRACE_OBJS) $(LDFLAGS)

# ---------------------------------------------------------- Q10: dispatcher
wsl_lite: q10_wsl_lite.c $(SC_OBJS) sc_trace.h
	$(CC) $(CFLAGS) -o $@ q10_wsl_lite.c $(SC_OBJS) $(LDFLAGS)

# ---------------------------------------------------------------- demo/clean
demo: all
	./run_demos.sh

clean:
	rm -f $(BINARIES) $(LIBS) *.o syscall_names.h
	rm -rf output demo_workspace

help:
	@echo "targets: all (default), demo, clean"
