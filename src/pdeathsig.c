// This code is licenced under BSD (see LICENSE file) by Yan Shoshitaishvili

/*
 * preeny's "pdeathsig" module: make a process -- and everything it spawns --
 * die when its parent dies.
 *
 * This is Linux's prctl(PR_SET_PDEATHSIG), which the kernel clears in every
 * newly created task but preserves across execve(). That asymmetry is the
 * whole design:
 *
 *   - the constructor covers every descendant that exec()s, because LD_PRELOAD
 *     is inherited through exec and so the constructor simply runs again over
 *     there. This is what covers system(), popen() and posix_spawn(), none of
 *     which reach an interposable fork()/clone() symbol -- glibc issues the
 *     clone syscall inline for all three.
 *   - the fork()/_Fork()/__fork() hooks cover the children that never exec
 *     (the classic forking server), which nothing else can reach.
 *   - the clone()/__clone() hooks cover programs that call clone() themselves.
 *
 * Every link only ever arms itself against its own immediate parent, and the
 * kernel keeps the armed signal across reparenting, so killing the top of a
 * process tree takes the whole tree with it.
 *
 * Configuration:
 *   PREENY_PDEATHSIG -- the signal, as a number ("9", "0x9") or a name
 *                       ("SIGKILL", "KILL", "kill"). Default SIGKILL.
 *
 * Two symbols are deliberately exported, for the test suite and for anyone who
 * wants to re-arm by hand after dropping privileges (which silently clears
 * pdeathsig in the kernel):
 *
 *   int preeny_pdeathsig;                       -- the configured signal
 *   void preeny_pdeathsig_arm(pid_t expected);  -- arm, and check for orphanhood
 *                                                  (pass 0 to just arm)
 */

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "logging.h"

// Both build systems link this module's object *before* logging.o, so
// logging.c's constructor has not run yet when ours does and preeny_debug_on /
// preeny_info_on are still 0. preeny_logging_init() only ORs flags in, so
// calling it by hand is idempotent. Declared here rather than in logging.h so
// that this module is a single new file with no shared-header dependency.
void preeny_logging_init(void);

#ifndef __linux__

//
// PR_SET_PDEATHSIG is Linux-only. We still define the exported symbols, so
// that anything linking against them (the test suite) builds and skips
// cleanly rather than failing to link.
//

int preeny_pdeathsig = 0;

void preeny_pdeathsig_arm(pid_t expected_parent)
{
	(void)expected_parent;
}

__attribute__((constructor)) void preeny_pdeathsig_init(void)
{
	preeny_logging_init();
	preeny_error("pdeathsig needs Linux's prctl(PR_SET_PDEATHSIG); doing nothing.\n");
}

#else /* __linux__ */

#include <dlfcn.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/types.h>

// Carving the clone() context off the caller's child stack (see
// preeny_clone_common) needs a downward-growing stack. Every architecture
// preeny is plausibly built for qualifies except hppa and ia64.
#if defined(__hppa__) || defined(__ia64__)
#define PREENY_STACK_GROWS_DOWN 0
#else
#define PREENY_STACK_GROWS_DOWN 1
#endif

int preeny_pdeathsig = SIGKILL;

static pid_t (*original_fork)(void);
static pid_t (*original__Fork)(void);
static pid_t (*original___fork)(void);
static int (*original_clone)(int (*)(void *), void *, int, void *, ...);
static int (*original___clone)(int (*)(void *), void *, int, void *, ...);

//
// Logging.
//
// preeny_debug()/preeny_info()/preeny_error() are printf() plus an fflush(),
// which is not safe on the paths below: a just-fork()ed child inherits its
// parent's dirty stdio buffer, so flushing there re-emits bytes the parent had
// already produced -- corrupting the target's output whenever stdout is a pipe
// or a file, with no threads involved at all. (In a child of a *threaded*
// parent it can also deadlock outright.) So the child-side paths get their own
// write(2) logger, and printf-based logging stays in the parent and the
// constructor.
//

static void preeny_pdeathsig_log(const char *prefix, const char *msg, int num)
{
	char buf[192];
	char digits[16];
	unsigned int value;
	size_t i = 0;
	int d = 0;

	while (*prefix && i < sizeof(buf) - 32) buf[i++] = *prefix++;
	while (*msg && i < sizeof(buf) - 24) buf[i++] = *msg++;

	if (num < 0) { buf[i++] = '-'; value = (unsigned int)(-(long)num); }
	else value = (unsigned int)num;
	do { digits[d++] = (char)('0' + value % 10); value /= 10; } while (value != 0 && d < (int)sizeof(digits));
	while (d > 0) buf[i++] = digits[--d];

	buf[i++] = '\n';
	if (write(2, buf, i) < 0) { /* nothing useful to do about it */ }
}

//
// Arming.
//

/*
 * Arm pdeathsig on the calling thread, then make sure we didn't lose the race.
 *
 * The ordering is load-bearing. Checking first and arming second leaves a real
 * window: a parent that dies in between leaves us armed against a corpse, and
 * nothing is ever delivered -- an immortal orphan. Arming first means either
 * the parent died after the prctl() (the kernel signals us) or it died before
 * it, in which case the reparenting has already happened and getppid() cannot
 * lie to us. That only holds when expected_parent was sampled in the *parent*,
 * which is what the fork/clone hooks do. The constructor can only sample
 * getppid() over in the child, so a parent that died before the child's
 * constructor ran is invisible to it: getppid() already reads 1 and both sides
 * agree. See the README for that hole.
 *
 * expected_parent <= 0 means "just arm, don't check" -- either because we
 * can't know (CLONE_PARENT, a new PID namespace) or because the caller only
 * wants the arm. A getppid() of 0 means our parent lives in an ancestor PID
 * namespace, which is unknowable rather than orphaned, so we leave it alone.
 *
 * The self-signal is kill(getpid(), sig) rather than raise(): the kernel's own
 * pdeathsig is process-directed (it lands in ShdPnd), while glibc's raise() is
 * pthread_kill(pthread_self()) and is thread-directed. kill() is the faithful
 * emulation, and it is on the POSIX async-signal-safe list -- as are prctl(),
 * getpid(), getppid() and write().
 */
void preeny_pdeathsig_arm(pid_t expected_parent)
{
	int saved_errno = errno;
	pid_t current_parent;

	if (prctl(PR_SET_PDEATHSIG, preeny_pdeathsig, 0, 0, 0) != 0)
	{
		if (preeny_error_on)
			preeny_pdeathsig_log("!!! ERROR: ", "pdeathsig: prctl(PR_SET_PDEATHSIG) failed in pid ", (int)getpid());
		errno = saved_errno;
		return;
	}

	if (expected_parent > 0)
	{
		current_parent = getppid();
		if (current_parent != expected_parent && current_parent != 0)
		{
			// Loud on purpose: a process that kills itself with no output
			// looks like a crash. preeny_error_on defaults to 1.
			if (preeny_error_on)
				preeny_pdeathsig_log("!!! ERROR: ", "pdeathsig: parent died before we could arm; self-signaling pid ", (int)getpid());
			kill(getpid(), preeny_pdeathsig);
		}
	}

	errno = saved_errno;
}

//
// fork(), __fork() and _Fork().
//
// These are independent entry points: glibc's fork() reaches its own _Fork()
// by a direct call rather than through the PLT, so hooking one does not catch
// the other (and hooking all three does not double-fire). vfork() is
// deliberately NOT hooked -- its child borrows the parent's stack and may only
// _exit() or exec(), so running a wrapper frame there is a corruption hazard,
// and it always exec()s in practice, which the constructor covers.
//

static pid_t preeny_pdeathsig_fork(pid_t (*original)(void), const char *name)
{
	pid_t parent, child;
	int saved_errno;

	if (original == NULL)
	{
		preeny_error("pdeathsig: no underlying %s() to call!\n", name);
		errno = ENOSYS;
		return -1;
	}

	parent = getpid();		// sampled here, never cached: the target may have
					// changed PID namespace since we loaded
	child = original();
	saved_errno = errno;

	// getpid() != parent is what proves a fork actually happened. Another
	// preloaded module -- defork.so, notably -- can return 0 without forking,
	// and taking that at face value would make the *parent* run the child path
	// and signal itself to death.
	if (child == 0 && getpid() != parent) preeny_pdeathsig_arm(parent);
	else if (child > 0) preeny_debug("pdeathsig: armed signal %d in %s()ed child %d\n", preeny_pdeathsig, name, (int)child);

	errno = saved_errno;
	return child;
}

pid_t fork(void)
{
	// Resolved lazily as well as in the constructor: another preloaded
	// library's constructor can fork before ours has run.
	if (original_fork == NULL) original_fork = dlsym(RTLD_NEXT, "fork");
	return preeny_pdeathsig_fork(original_fork, "fork");
}

pid_t __fork(void)
{
	if (original___fork == NULL) original___fork = dlsym(RTLD_NEXT, "__fork");
	if (original___fork == NULL && original_fork == NULL) original_fork = dlsym(RTLD_NEXT, "fork");
	return preeny_pdeathsig_fork(original___fork ? original___fork : original_fork, "__fork");
}

pid_t _Fork(void)
{
	// _Fork() is glibc 2.34+. If the libc underneath us doesn't have it, we
	// fall back to fork() -- which differs only in that pthread_atfork()
	// handlers run, and which can only happen for a program that was linked
	// against a libc that *did* export _Fork.
	if (original__Fork == NULL) original__Fork = dlsym(RTLD_NEXT, "_Fork");
	if (original__Fork == NULL && original_fork == NULL) original_fork = dlsym(RTLD_NEXT, "fork");
	return preeny_pdeathsig_fork(original__Fork ? original__Fork : original_fork, "_Fork");
}

//
// clone() and __clone().
//
// Only reachable when the program calls clone() itself. glibc's own users of
// the clone syscall -- pthread_create(), posix_spawn(), system(), popen() --
// go straight to the kernel and cannot be interposed; the first must not be
// armed anyway, and the other three are covered by the constructor after the
// exec they always do.
//

struct preeny_clone_ctx
{
	int (*fn)(void *);
	void *arg;
	pid_t parent;
};

static int preeny_clone_trampoline(void *raw)
{
	// Copy the context onto our own stack first: with CLONE_VM it lives in
	// memory the parent still owns. No logging and no malloc in here -- with
	// CLONE_VM|CLONE_VFORK the parent is suspended and holding its own locks.
	struct preeny_clone_ctx ctx = *(struct preeny_clone_ctx *)raw;

	preeny_pdeathsig_arm(ctx.parent);
	return ctx.fn(ctx.arg);
}

static int preeny_clone_common(
	int (*original)(int (*)(void *), void *, int, void *, ...), const char *name,
	int (*fn)(void *), void *stack, int flags, void *arg,
	pid_t *parent_tid, void *tls, pid_t *child_tid
)
{
	struct preeny_clone_ctx *context;
	uintptr_t sp;

	if (original == NULL)
	{
		preeny_error("pdeathsig: no underlying %s() to call!\n", name);
		errno = ENOSYS;
		return -1;
	}

	// CLONE_THREAD means the new task is a thread of *this* process, and
	// pdeathsig is a per-thread property that fires when the creating thread
	// exits -- arming a thread would be actively harmful rather than merely
	// useless. CLONE_SETTLS means the child starts on a thread pointer the
	// caller built, so the trampoline cannot run *any* glibc code over there:
	// preeny_pdeathsig_arm() touches errno, which lives at a negative offset
	// from the TCB, and would fault (or silently scribble on caller memory).
	// Everything else here, including CLONE_VM without CLONE_THREAD, is a
	// real process and gets armed.
	if (!PREENY_STACK_GROWS_DOWN || fn == NULL || stack == NULL || (flags & (CLONE_THREAD | CLONE_SETTLS)))
		return original(fn, stack, flags, arg, parent_tid, tls, child_tid);

	// Carve the context off the top of the caller's child stack, exactly the
	// way glibc's own __clone stashes fn/arg there. malloc() is not an option:
	// with CLONE_VM the block is shared with the caller, so free()ing it in
	// the child takes the parent's malloc lock (deadlock if another thread
	// held it), and without CLONE_VM the child only frees its copy-on-write
	// copy, leaking a few dozen bytes per clone() in the parent forever.
	sp = ((uintptr_t)stack) & ~(uintptr_t)15;
	sp -= sizeof(struct preeny_clone_ctx);
	sp &= ~(uintptr_t)15;

	context = (struct preeny_clone_ctx *)sp;
	context->fn = fn;
	context->arg = arg;

	// CLONE_PARENT (implied by CLONE_THREAD) makes the new task our *sibling*,
	// and CLONE_NEWPID puts its parent in another PID namespace where getppid()
	// reads 0. In both cases the orphan check would be comparing unrelated
	// numbers and could self-kill a perfectly healthy child -- so we still arm,
	// we just skip the check.
	context->parent = (flags & (CLONE_PARENT | CLONE_NEWPID)) ? 0 : getpid();

	preeny_debug("pdeathsig: arming signal %d in %s()d child (flags=0x%x)\n", preeny_pdeathsig, name, (unsigned int)flags);
	return original(preeny_clone_trampoline, (void *)sp, flags, context, parent_tid, tls, child_tid);
}

/*
 * The real prototype is variadic and there is no vclone(), so we pull all three
 * tail arguments unconditionally and forward them as fixed ones. The kernel
 * only dereferences each one when its own flag is set, and on every SysV ABI a
 * variadic callee has already spilled its register arguments, so reading
 * arguments the caller never passed yields defined-but-ignored garbage.
 * Forwarding fewer would break CLONE_PARENT_SETTID / CLONE_SETTLS /
 * CLONE_CHILD_CLEARTID -- and do NOT "helpfully" zero the unused ones, since
 * CLONE_PIDFD reuses the parent_tid slot.
 */
#define PREENY_CLONE_WRAPPER(wrapper_name, original_pointer, symbol_name) \
	int wrapper_name(int (*fn)(void *), void *stack, int flags, void *arg, ...) \
	{ \
		va_list args; \
		pid_t *parent_tid; \
		void *tls; \
		pid_t *child_tid; \
		\
		va_start(args, arg); \
		parent_tid = va_arg(args, pid_t *); \
		tls = va_arg(args, void *); \
		child_tid = va_arg(args, pid_t *); \
		va_end(args); \
		\
		if (original_pointer == NULL) original_pointer = dlsym(RTLD_NEXT, symbol_name); \
		return preeny_clone_common(original_pointer, symbol_name, fn, stack, flags, arg, parent_tid, tls, child_tid); \
	}

PREENY_CLONE_WRAPPER(clone, original_clone, "clone")
PREENY_CLONE_WRAPPER(__clone, original___clone, "__clone")

//
// Configuration.
//

static const struct { const char *name; int sig; } preeny_signames[] = {
	{ "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "ILL", SIGILL },
	{ "ABRT", SIGABRT }, { "FPE", SIGFPE }, { "KILL", SIGKILL }, { "SEGV", SIGSEGV },
	{ "PIPE", SIGPIPE }, { "ALRM", SIGALRM }, { "TERM", SIGTERM }, { "USR1", SIGUSR1 },
	{ "USR2", SIGUSR2 }, { "CHLD", SIGCHLD }, { "CONT", SIGCONT }, { "STOP", SIGSTOP },
	{ "TSTP", SIGTSTP }, { "TRAP", SIGTRAP }, { "BUS", SIGBUS }, { "SYS", SIGSYS },
	{ NULL, 0 }
};

static int preeny_is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/*
 * Returns a signal number, or -1 if we can't make sense of the string.
 *
 * This is stricter than preeny's usual bare atoi() on purpose, and it is the
 * one module where that matters: atoi("SIGKILL") is 0, and
 * prctl(PR_SET_PDEATHSIG, 0) *succeeds* -- it means "disable". A typo would
 * otherwise produce a module that loads, says nothing, and does nothing.
 */
static int preeny_parse_signal(const char *string)
{
	const char *name, *rest;
	char *end;
	long number;
	int i;

	if (string == NULL) return -1;
	while (preeny_is_space(*string)) string++;
	if (*string == '\0') return -1;

	// Base 10 unless there is an explicit 0x: with base 0, "010" would
	// silently mean signal 8, and nobody zero-pads a signal number meaning
	// octal (kill(1), pkill(1) and the shell all read "010" as 10).
	number = strtol(string, &end, (string[0] == '0' && (string[1] == 'x' || string[1] == 'X')) ? 16 : 10);
	if (end != string)
	{
		while (preeny_is_space(*end)) end++;
		if (*end == '\0') return (number >= 1 && number <= 64) ? (int)number : -1;
	}

	name = string;
	if (strncasecmp(name, "SIG", 3) == 0) name += 3;
	for (i = 0; preeny_signames[i].name != NULL; i++)
	{
		size_t length = strlen(preeny_signames[i].name);

		if (strncasecmp(name, preeny_signames[i].name, length) != 0) continue;
		rest = name + length;
		while (preeny_is_space(*rest)) rest++;
		if (*rest == '\0') return preeny_signames[i].sig;
	}

	return -1;
}

__attribute__((constructor)) void preeny_pdeathsig_init(void)
{
	char *setting;
	pid_t parent;

	preeny_logging_init();

	// dlsym() is not safe to call in a forked child, so everything is resolved
	// up front here; the wrappers only retry lazily for the case where another
	// library's constructor beats ours.
	original_fork = dlsym(RTLD_NEXT, "fork");
	original___fork = dlsym(RTLD_NEXT, "__fork");
	original__Fork = dlsym(RTLD_NEXT, "_Fork");	// glibc 2.34+, NULL elsewhere
	original_clone = dlsym(RTLD_NEXT, "clone");
	original___clone = dlsym(RTLD_NEXT, "__clone");

	setting = getenv("PREENY_PDEATHSIG");
	if (setting != NULL)
	{
		int signal_number = preeny_parse_signal(setting);

		if (signal_number < 0) preeny_error("pdeathsig: unrecognized signal \"%s\" in PREENY_PDEATHSIG; using SIGKILL\n", setting);
		else preeny_pdeathsig = signal_number;
	}

	// Sampled before arming, so that a parent that dies *during* the arm is
	// caught by the same re-read that covers the fork hook.
	parent = getppid();
	preeny_info("pdeathsig: pid %d will get signal %d when pid %d dies\n", (int)getpid(), preeny_pdeathsig, (int)parent);
	preeny_pdeathsig_arm(parent);
}

#endif /* __linux__ */
