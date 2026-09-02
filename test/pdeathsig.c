/*
 * Test for preeny's "pdeathsig" module.
 *
 * Usage: test_pdeathsig <mode> [argument]
 *
 *   fork   [sig]  build root -> kid -> grandkid with nothing but fork(), kill
 *                 the root, assert both descendants died of <sig>.
 *   exec   [sig]  root -> exec'd kid (with pdeathsig explicitly cleared first),
 *                 kill the root, assert the kid died of <sig>.
 *   clone  [sig]  root -> clone()d kid, kill the root, assert it died of <sig>.
 *   disarm 0      a kid that disarms itself with prctl(PR_SET_PDEATHSIG, 0)
 *                 must SURVIVE -- this is a documented hole, and the test is
 *                 here so that nobody quietly turns pdeathsig.so into a module
 *                 that interposes prctl().
 *   arm    [sig]  exercise the module's lost-the-race path directly.
 *   race          lose the parent for real, through the fork() hook, and assert
 *                 no orphan ever outlives it.
 *   thread        assert a clone(CLONE_THREAD) task is never armed -- arming one
 *                 would kill the whole process.
 *   settls        assert a clone(CLONE_SETTLS) child reaches its own code
 *                 instead of faulting in the module's trampoline.
 *   config <n>    assert PREENY_PDEATHSIG parsed to <n>.
 *
 *   [sig] defaults to SIGKILL. A [sig] of 0 means "the descendants are
 *   expected to SURVIVE": that is the negative control, run without LD_PRELOAD.
 *
 * Exits 0 on success, 1 on failure, 77 if this machine cannot run the case.
 * It can neither hang nor leak processes: every process it creates arms its own
 * alarm() suicide timer, and every wait is deadline-bounded.
 *
 * How the descendants are observed
 * --------------------------------
 * The harness makes itself a CHILD SUBREAPER, so when the root of the chain is
 * killed its orphaned descendants are reparented to *us* instead of to init.
 * That lets us waitpid() them, which beats polling /proc/<pid> or kill(pid, 0):
 *   - edge-triggered, so there is no poll-interval race,
 *   - immune to PID reuse, because we do the reaping,
 *   - a zombie is not mistaken for a live process (kill(pid, 0) says a zombie
 *     is alive, which would make the negative control pass against a module
 *     that is in fact killing things), and
 *   - it reports *which* signal killed each process.
 *
 * How we know the death came from pdeathsig
 * -----------------------------------------
 *   1. we assert the exact WTERMSIG, not merely that the process died;
 *   2. the suicide timer uses SIGALRM (14), so a backstop death can never be
 *      mistaken for a pass; and
 *   3. run_tests.sh runs one case with PREENY_PDEATHSIG=SIGUSR2, and nothing in
 *      the system but PR_SET_PDEATHSIG sends these processes a signal 12.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifndef PR_SET_CHILD_SUBREAPER
#define PR_SET_CHILD_SUBREAPER 36
#endif

/*
 * The module's test seam. These are weak references: they resolve to the
 * preloaded pdeathsig.so when it is there and to NULL when it is not, which is
 * how the harness tells "module loaded" from "negative control" without
 * inspecting LD_PRELOAD, without dlsym(), and without -ldl.
 */
extern int preeny_pdeathsig __attribute__((weak));
extern void preeny_pdeathsig_arm(pid_t expected_parent) __attribute__((weak));

#define HARNESS_TIMEOUT    20	/* s: hard backstop on the whole test */
#define DESCENDANT_TIMEOUT 15	/* s: hard backstop inside every descendant */
#define REPORT_TIMEOUT_MS  5000	/* ms: waiting for the chain to announce itself */
#define REAP_TIMEOUT_MS    5000	/* ms: waiting for the chain to die */
#define SURVIVE_GRACE_MS   750	/* ms: survival mode -- how long survival must hold */
#define MAX_PROCS 8
#define CLONE_STACK_SIZE (256 * 1024)

struct preeny_report { char tag[12]; pid_t pid; };

static volatile pid_t g_pids[MAX_PROCS];
static char g_tags[MAX_PROCS][12];
static int g_status[MAX_PROCS];
static int g_reaped[MAX_PROCS];
static volatile int g_nprocs = 0;

static void track(pid_t pid, const char *tag)
{
	if (g_nprocs >= MAX_PROCS) return;
	g_pids[g_nprocs] = pid;
	snprintf(g_tags[g_nprocs], sizeof(g_tags[0]), "%s", tag);
	g_nprocs++;
}

static void kill_everything(void)
{
	int i;
	for (i = 0; i < g_nprocs; i++) if (g_pids[i] > 0) kill(g_pids[i], SIGKILL);
}

static void harness_alarm(int sig)
{
	static const char message[] = "!!! test_pdeathsig: harness timed out\n";
	(void)sig;
	if (write(2, message, sizeof(message) - 1) < 0) { }
	kill_everything();
	_exit(1);
}

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Goes straight to the kernel rather than through libc, so that this stays a
 * real clear even if some other preloaded module ever interposes prctl(). */
static void raw_set_pdeathsig(int sig)
{
	syscall(SYS_prctl, PR_SET_PDEATHSIG, (long)sig, 0L, 0L, 0L);
}

/*
 * Is the module actually here? A missing seam with LD_PRELOAD naming pdeathsig
 * is a FAILURE, not a skip -- weak references only bind through a dynamic
 * relocation, so a non-PIE build or a renamed symbol would otherwise turn a
 * broken module into a quiet "SKIP".
 */
static int seam_state(const char *mode)
{
	const char *preload = getenv("LD_PRELOAD");

	if (&preeny_pdeathsig != NULL && &preeny_pdeathsig_arm != NULL) return 0;
	if (preload != NULL && strstr(preload, "pdeathsig") != NULL)
	{
		fprintf(stderr, "!!! FAIL: pdeathsig[%s]: LD_PRELOAD names pdeathsig but its exported symbols did not bind\n", mode);
		return 1;
	}
	fprintf(stderr, "### pdeathsig[%s]: SKIP, module is not preloaded\n", mode);
	return 77;
}

/* ------------------------------------------------------------------ */
/* descendant side                                                     */
/* ------------------------------------------------------------------ */

/* Every descendant gets its own suicide timer, so even a SIGKILLed harness
 * cannot leave strays behind. SIGALRM's default action terminates, and 14 is
 * distinguishable from any signal we assert on. */
static void descendant_setup(void)
{
	signal(SIGALRM, SIG_DFL);	/* drop the harness's inherited handler */
	alarm(DESCENDANT_TIMEOUT);
}

static void report_and_wait(int wfd, const char *tag) __attribute__((noreturn));
static void report_and_wait(int wfd, const char *tag)
{
	struct preeny_report report;

	memset(&report, 0, sizeof(report));
	snprintf(report.tag, sizeof(report.tag), "%s", tag);
	report.pid = getpid();

	/* We announce ourselves only *after* our pdeathsig has been armed (the
	 * module armed it on our way in here), and the harness kills the root only
	 * after every announcement has arrived. That takes the fork-vs-prctl timing
	 * window out of the chain tests entirely -- the race is tested deliberately
	 * and deterministically by the "arm" mode instead. */
	if (write(wfd, &report, sizeof(report)) != sizeof(report)) _exit(9);

	for (;;) pause();
}

struct clone_arg { int wfd; };

static int clone_child(void *raw)
{
	descendant_setup();
	report_and_wait(((struct clone_arg *)raw)->wfd, "clone");
	return 0;
}

static void root_main(const char *mode, int wfd) __attribute__((noreturn));
static void root_main(const char *mode, int wfd)
{
	pid_t kid;

	descendant_setup();

	if (strcmp(mode, "fork") == 0)
	{
		/* fork-hook path, two links deep. Neither descendant ever exec()s, so
		 * the constructor never runs in them and only the fork() hook can have
		 * armed them. The grandkid is what proves the *chain* claim: it dies
		 * because the kid died of its own pdeathsig, not because we killed it. */
		kid = fork();
		if (kid < 0) _exit(9);
		if (kid == 0)
		{
			pid_t grandkid;

			descendant_setup();
			grandkid = fork();
			if (grandkid < 0) _exit(9);
			if (grandkid == 0)
			{
				descendant_setup();
				report_and_wait(wfd, "grandkid");
			}
			report_and_wait(wfd, "kid");
		}
	}
	else if (strcmp(mode, "exec") == 0)
	{
		/* constructor path. We CLEAR pdeathsig in the child before exec'ing,
		 * because execve() preserves it: without this, the value the fork()
		 * hook set would simply survive and we would not know which of the two
		 * code paths we had actually tested. */
		kid = fork();
		if (kid < 0) _exit(9);
		if (kid == 0)
		{
			char fdbuf[16];

			descendant_setup();
			raw_set_pdeathsig(0);
			snprintf(fdbuf, sizeof(fdbuf), "%d", wfd);
			execl("/proc/self/exe", "test_pdeathsig", "--sleeper", fdbuf, "exec", (char *)NULL);
			_exit(9);
		}
	}
	else if (strcmp(mode, "clone") == 0)
	{
		static struct clone_arg argument;
		char *stack = malloc(CLONE_STACK_SIZE);

		if (stack == NULL) _exit(9);
		argument.wfd = wfd;
		if (clone(clone_child, stack + CLONE_STACK_SIZE, SIGCHLD, &argument) < 0) _exit(9);
	}
	else if (strcmp(mode, "disarm") == 0)
	{
		/* A child that disarms itself through libc is *supposed* to survive:
		 * pdeathsig.so does not interpose prctl(). This test pins that down. */
		kid = fork();
		if (kid < 0) _exit(9);
		if (kid == 0)
		{
			descendant_setup();
			prctl(PR_SET_PDEATHSIG, 0, 0, 0, 0);
			report_and_wait(wfd, "disarmed");
		}
	}
	else
	{
		_exit(9);
	}

	for (;;) pause();
}

/* ------------------------------------------------------------------ */
/* harness side                                                        */
/* ------------------------------------------------------------------ */

static int read_report(int rfd, struct preeny_report *out, long deadline)
{
	size_t got = 0;

	while (got < sizeof(*out))
	{
		struct pollfd pfd;
		long remaining = deadline - now_ms();
		ssize_t n;
		int p;

		if (remaining <= 0) return -1;
		pfd.fd = rfd; pfd.events = POLLIN; pfd.revents = 0;
		p = poll(&pfd, 1, (int)remaining);
		if (p < 0) { if (errno == EINTR) continue; return -1; }
		if (p == 0) return -1;
		n = read(rfd, (char *)out + got, sizeof(*out) - got);
		if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
		got += (size_t)n;
	}
	return 0;
}

static void collect(long deadline, int stop_when_all_reaped)
{
	for (;;)
	{
		struct timespec nap;
		int status, i;
		pid_t reaped = waitpid(-1, &status, WNOHANG);

		if (reaped > 0)
		{
			for (i = 0; i < g_nprocs; i++)
				if (g_pids[i] == reaped) { g_status[i] = status; g_reaped[i] = 1; }
			continue;
		}
		if (stop_when_all_reaped)
		{
			int all = 1;
			for (i = 0; i < g_nprocs; i++) if (!g_reaped[i]) all = 0;
			if (all) return;
		}
		if (now_ms() >= deadline) return;
		nap.tv_sec = 0; nap.tv_nsec = 10 * 1000 * 1000;
		nanosleep(&nap, NULL);
	}
}

static const char *describe(int i)
{
	static char description[64];

	if (!g_reaped[i]) snprintf(description, sizeof(description), "still alive");
	else if (WIFSIGNALED(g_status[i])) snprintf(description, sizeof(description), "killed by signal %d", WTERMSIG(g_status[i]));
	else if (WIFEXITED(g_status[i])) snprintf(description, sizeof(description), "exited with %d", WEXITSTATUS(g_status[i]));
	else snprintf(description, sizeof(description), "unknown status 0x%x", g_status[i]);

	return description;
}

/*
 * "arm" mode: the lost-the-race path, made deterministic.
 *
 * If a parent dies in between fork() returning and the child's prctl(), the
 * kernel will never send anything and the child is immortal. The module handles
 * that by re-reading getppid() after arming and signaling itself if the answer
 * changed. Reproducing that by timing would be flaky, so we call the module's
 * own arming helper directly with a parent pid that is deliberately wrong and
 * assert the process kills itself -- and then, with the *right* pid, assert that
 * it does not, because a mitigation that fires when it shouldn't is worse than
 * no mitigation at all.
 */
static int mode_arm(int expected_signal)
{
	static const struct { const char *name; int bogus; int want_death; } cases[] = {
		{ "wrong parent -> must self-signal", 1, 1 },
		{ "right parent -> must survive",     0, 0 },
	};
	int failures = 0;
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		pid_t kid;
		int status = 0, ok;

		fflush(NULL);
		kid = fork();
		if (kid < 0) { perror("fork"); return 2; }
		if (kid == 0)
		{
			descendant_setup();
			/* getppid()+1 is guaranteed not to be our parent, and the module
			 * only ever compares the number -- it never has to exist. */
			preeny_pdeathsig_arm(cases[i].bogus ? getppid() + 1 : getppid());
			for (;;) pause();
		}

		if (cases[i].want_death)
		{
			long deadline = now_ms() + REAP_TIMEOUT_MS;
			pid_t reaped;

			do
			{
				reaped = waitpid(kid, &status, WNOHANG);
				if (reaped == 0) usleep(10000);
			} while (reaped == 0 && now_ms() < deadline);
			ok = (reaped == kid) && WIFSIGNALED(status) && WTERMSIG(status) == expected_signal;
		}
		else
		{
			usleep(SURVIVE_GRACE_MS * 1000);
			ok = (waitpid(kid, &status, WNOHANG) == 0);
			kill(kid, SIGKILL);
			waitpid(kid, NULL, 0);
		}

		printf("    %-34s -> %s\n", cases[i].name, ok ? "OK" : "FAIL");
		if (!ok) failures++;
	}

	return failures ? 1 : 0;
}

/*
 * "race" mode: the orphan-race guard, exercised through the real fork() hook.
 *
 * "arm" mode calls preeny_pdeathsig_arm() directly, which pins the helper but
 * not its *use*: mutating the fork hook to arm(0) -- the documented "just arm,
 * don't check" sentinel -- leaves every other check green while immortal
 * orphans come back. So here we lose the parent for real, and as fast as
 * possible: the launcher forks through the hook and then exit_group()s with no
 * atexit handlers and no stdio flush, so the victim's prctl() and its parent's
 * death genuinely race.
 *
 * This is one-sided, so it cannot flake. Under a correct module a survivor is
 * impossible -- either the kernel delivers the pdeathsig, or the post-arm
 * getppid() re-read catches the loss -- so "0 survivors" is a deterministic
 * pass rather than a timing-dependent one. A victim killed before it could
 * report simply closes the pipe, and the read below ends on EOF rather than
 * waiting out its deadline.
 */
static int mode_race(void)
{
	const int trials = 30;
	int survivors = 0, i;

	if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0)
	{
		fprintf(stderr, "### pdeathsig[race]: SKIP, PR_SET_CHILD_SUBREAPER failed: %s\n", strerror(errno));
		return 77;
	}

	alarm(50);	/* 30 trials x a 1s reap deadline, worst case */

	for (i = 0; i < trials && survivors < 3; i++)
	{
		struct preeny_report report;
		int pipefd[2];
		pid_t launcher;
		long deadline;
		int alive = 0;

		if (pipe(pipefd) != 0) { perror("pipe"); return 2; }

		fflush(NULL);
		launcher = fork();
		if (launcher < 0) { perror("fork"); return 2; }
		if (launcher == 0)
		{
			pid_t victim;

			close(pipefd[0]);
			victim = fork();		/* through the module's fork() hook */
			if (victim < 0) _exit(9);
			if (victim == 0)
			{
				descendant_setup();
				report_and_wait(pipefd[1], "victim");
			}
			syscall(SYS_exit_group, 0);
		}
		close(pipefd[1]);
		waitpid(launcher, NULL, 0);

		/* We are a subreaper, so the orphaned victim is reparented to us and
		 * waitpid() can see it either way. Anything still alive after the
		 * deadline never got the guard, and is the bug. */
		if (read_report(pipefd[0], &report, now_ms() + 1000) == 0)
		{
			deadline = now_ms() + 1000;
			do
			{
				alive = (waitpid(report.pid, NULL, WNOHANG) == 0);
				if (alive) usleep(2000);
			} while (alive && now_ms() < deadline);

			if (alive)
			{
				survivors++;
				kill(report.pid, SIGKILL);
				waitpid(report.pid, NULL, 0);
			}
		}
		close(pipefd[0]);
	}

	printf("    %d of %d orphans outlived the parent they were racing -> %s\n",
	       survivors, i, survivors ? "FAIL" : "OK");
	return survivors ? 1 : 0;
}

/*
 * "settls" mode: a clone(CLONE_SETTLS) child must reach its own code.
 *
 * Such a child starts on a thread pointer its caller built, so the module's
 * trampoline must not run any libc over there -- arming touches errno, which
 * lives at a negative offset from that pointer, and the child dies of SIGSEGV
 * before fn() is ever entered while clone() still reports success to the
 * parent. Regression test for exactly that: the child does nothing but a raw
 * exit syscall, and we assert its status is 42 rather than a signal.
 */
static int settls_child(void *raw)
{
	(void)raw;
	syscall(SYS_exit, 42);
	return 42;
}

static int mode_settls(void)
{
	pid_t kid;
	int status = 0, ok;

	fflush(NULL);
	kid = fork();
	if (kid < 0) { perror("fork"); return 2; }
	if (kid == 0)
	{
		char *stack = malloc(CLONE_STACK_SIZE);
		int cloned_status = 0;
		pid_t cloned;
		void *tls;

		descendant_setup();
		tls = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (stack == NULL || tls == MAP_FAILED) _exit(9);

		cloned = clone(settls_child, stack + CLONE_STACK_SIZE,
			       CLONE_VM | CLONE_SETTLS | SIGCHLD, NULL, NULL, tls, NULL);
		if (cloned < 0) _exit(77);
		if (waitpid(cloned, &cloned_status, 0) != cloned) _exit(9);
		/* signal N is reported to the harness as 100 + N */
		_exit(WIFSIGNALED(cloned_status) ? 100 + WTERMSIG(cloned_status)
						 : (WIFEXITED(cloned_status) ? WEXITSTATUS(cloned_status) : 9));
	}

	waitpid(kid, &status, 0);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 77)
	{
		fprintf(stderr, "### pdeathsig[settls]: SKIP, clone(CLONE_SETTLS) is not available here\n");
		return 77;
	}

	ok = WIFEXITED(status) && WEXITSTATUS(status) == 42;
	if (WIFEXITED(status) && WEXITSTATUS(status) > 100)
		printf("    CLONE_SETTLS child was killed by signal %d before its code ran -> FAIL\n", WEXITSTATUS(status) - 100);
	else
		printf("    CLONE_SETTLS child ran its own code and exited %d -> %s\n",
		       WIFEXITED(status) ? WEXITSTATUS(status) : -1, ok ? "OK" : "FAIL");
	return ok ? 0 : 1;
}

/*
 * "thread" mode: a CLONE_THREAD task must never be armed.
 *
 * pthread_create() cannot reach the hook (glibc goes straight to the kernel),
 * so the only way to cover that guard is to call clone() with CLONE_THREAD
 * ourselves. If the module ever armed such a task, its getppid() would be the
 * *grandparent*, the orphan check would mismatch, and kill(getpid(), ...) --
 * which is process-directed -- would take the whole process down with it. So we
 * do it in a subprocess and assert that the subprocess is still standing.
 *
 * The cloned task shares our thread pointer, so it does raw syscalls only.
 */
static int clone_thread_child(void *raw)
{
	(void)raw;
	syscall(SYS_exit, 0);
	return 0;
}

static int mode_thread(void)
{
	pid_t kid;
	int status = 0, ok;

	fflush(NULL);
	kid = fork();
	if (kid < 0) { perror("fork"); return 2; }
	if (kid == 0)
	{
		char *stack = malloc(CLONE_STACK_SIZE);

		descendant_setup();
		if (stack == NULL) _exit(9);
		if (clone(clone_thread_child, stack + CLONE_STACK_SIZE,
			  CLONE_THREAD | CLONE_VM | CLONE_SIGHAND | CLONE_FS | CLONE_FILES, NULL) < 0) _exit(77);
		usleep(200000);		/* leave a wrongly-armed task time to kill us */
		_exit(0);
	}

	waitpid(kid, &status, 0);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 77)
	{
		fprintf(stderr, "### pdeathsig[thread]: SKIP, clone(CLONE_THREAD) is not available here\n");
		return 77;
	}

	ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	printf("    CLONE_THREAD task left unarmed, process survived (%s) -> %s\n",
	       WIFSIGNALED(status) ? "killed by signal" : "exited normally", ok ? "OK" : "FAIL");
	return ok ? 0 : 1;
}

/* "config" mode: PREENY_PDEATHSIG parsing, asserted directly rather than
 * inferred from a corpse. */
static int mode_config(int expected)
{
	const char *setting = getenv("PREENY_PDEATHSIG");

	printf("    PREENY_PDEATHSIG=%s parsed to %d, expected %d -> %s\n",
	       setting ? setting : "(unset)", preeny_pdeathsig, expected,
	       preeny_pdeathsig == expected ? "OK" : "FAIL");
	return preeny_pdeathsig == expected ? 0 : 1;
}

int main(int argc, char **argv)
{
	const char *mode;
	int expected_signal = SIGKILL;
	int expected_reports;
	int pipefd[2];
	long deadline;
	pid_t root;
	int failures = 0;
	int i;

	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <fork|exec|clone|disarm|arm|race|thread|settls|config> [signal, 0 = expect survival]\n", argv[0]);
		return 2;
	}
	mode = argv[1];

	/* re-exec'd by "exec" mode: we are a leaf of the chain, not the harness */
	if (strcmp(mode, "--sleeper") == 0)
	{
		if (argc < 4) return 2;
		descendant_setup();
		report_and_wait(atoi(argv[2]), argv[3]);
	}

	if (argc > 2) expected_signal = atoi(argv[2]);

	signal(SIGALRM, harness_alarm);
	alarm(HARNESS_TIMEOUT);

	printf("### pdeathsig[%s]:\n", mode);
	fflush(NULL);

	if (strcmp(mode, "config") == 0 || strcmp(mode, "arm") == 0 ||
	    strcmp(mode, "race") == 0 || strcmp(mode, "thread") == 0 ||
	    strcmp(mode, "settls") == 0)
	{
		int state = seam_state(mode);
		int result;

		if (state != 0) { fflush(NULL); return state; }
		if (strcmp(mode, "config") == 0) result = mode_config(expected_signal);
		else if (strcmp(mode, "arm") == 0) result = mode_arm(expected_signal);
		else if (strcmp(mode, "race") == 0) result = mode_race();
		else if (strcmp(mode, "settls") == 0) result = mode_settls();
		else result = mode_thread();
		fflush(NULL);
		return result;
	}

	expected_reports = (strcmp(mode, "fork") == 0) ? 2 : 1;
	printf("    expecting descendants to %s\n", expected_signal ? "die" : "SURVIVE");
	fflush(NULL);

	/* so that orphaned descendants are reparented to us and we can wait() them */
	if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0)
	{
		fprintf(stderr, "### pdeathsig[%s]: SKIP, PR_SET_CHILD_SUBREAPER failed: %s\n", mode, strerror(errno));
		return 77;
	}

	if (pipe(pipefd) != 0) { perror("pipe"); return 2; }

	fflush(NULL);	/* never duplicate our own stdio buffer into the chain */
	root = fork();
	if (root < 0) { perror("fork"); return 2; }
	if (root == 0) { close(pipefd[0]); root_main(mode, pipefd[1]); }

	track(root, "root");
	close(pipefd[1]);

	deadline = now_ms() + REPORT_TIMEOUT_MS;
	for (i = 0; i < expected_reports; i++)
	{
		struct preeny_report report;

		if (read_report(pipefd[0], &report, deadline) != 0)
		{
			fprintf(stderr, "!!! FAIL: only got %d of %d chain reports\n", i, expected_reports);
			kill_everything();
			return 1;
		}
		track(report.pid, report.tag);
		printf("    chain member '%s' is pid %d\n", report.tag, (int)report.pid);
	}
	fflush(NULL);

	printf("    killing chain root %d\n", (int)root);
	fflush(NULL);
	kill(root, SIGKILL);

	if (expected_signal)
	{
		collect(now_ms() + REAP_TIMEOUT_MS, 1);
		for (i = 0; i < g_nprocs; i++)
		{
			int want = (i == 0) ? SIGKILL : expected_signal;	/* we killed the root ourselves */
			int ok = g_reaped[i] && WIFSIGNALED(g_status[i]) && WTERMSIG(g_status[i]) == want;

			printf("    %-9s pid %-7d %-24s -> %s\n", g_tags[i], (int)g_pids[i], describe(i), ok ? "OK" : "FAIL");
			if (!ok) failures++;
		}
	}
	else
	{
		collect(now_ms() + SURVIVE_GRACE_MS, 0);
		for (i = 1; i < g_nprocs; i++)	/* the root is supposed to be dead */
		{
			int ok = !g_reaped[i];

			printf("    %-9s pid %-7d %-24s -> %s\n", g_tags[i], (int)g_pids[i], describe(i), ok ? "OK" : "FAIL");
			if (!ok) failures++;
		}
	}

	kill_everything();
	collect(now_ms() + 1000, 1);
	alarm(0);

	printf("### pdeathsig[%s]: %s\n", mode, failures ? "FAILED" : "passed");
	fflush(NULL);
	return failures ? 1 : 0;
}
