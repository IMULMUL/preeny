#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Test for preeny's "nowrite" module.
 *
 * nowrite.so downgrades every open() to O_RDONLY, so underneath it a program
 * can still read a file it asked to open for writing, but every write() fails
 * and a file that does not exist yet is never created.
 *
 * The fixture is created with a raw openat(2) rather than with open(3), because
 * open() is the very symbol under test -- going through it would leave us with
 * no way to write the fixture in the first place. That also keeps this test
 * from touching the source tree: it used to open its own nowrite.c for writing,
 * which meant that running it *without* the preload quietly filled the file
 * with a kilobyte of garbage, and running it from anywhere but test/ failed
 * outright because nowrite.c was not in the working directory.
 *
 * Exits 0 if every case behaved, 1 otherwise.
 */

#define FIXTURE "/tmp/preeny_nowrite_test"
#define ABSENT  "/tmp/preeny_nowrite_test_absent"

static const char fixture_contents[] = "preeny nowrite test fixture\n";

static int failures = 0;

static void check(int ok, const char *what)
{
	printf("    %-56s -> %s\n", what, ok ? "OK" : "FAIL");
	if (!ok) failures++;
}

/* Bypasses the module on purpose -- see above. */
static int real_open(const char *path, int flags, mode_t mode)
{
	return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

static int make_fixture(void)
{
	int fd = real_open(FIXTURE, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0) return -1;
	if (write(fd, fixture_contents, sizeof(fixture_contents) - 1) != (ssize_t)(sizeof(fixture_contents) - 1))
	{
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/*
 * Open the fixture with the given flags and report what we could do with the
 * result. Every case wants the same thing of the module: a readable fd that
 * cannot be written to.
 */
static void case_downgraded(const char *what, int flags)
{
	char buffer[64];
	int fd = open(FIXTURE, flags, 0644);
	ssize_t got, wrote;

	if (fd < 0)
	{
		check(0, what);
		printf("        open() failed outright: %s\n", strerror(errno));
		return;
	}

	got = read(fd, buffer, sizeof(buffer));
	wrote = write(fd, "clobber", 7);
	close(fd);

	check(got > 0 && wrote == -1, what);
	if (got <= 0) printf("        expected the fd to still be readable, read() returned %zd\n", got);
	if (wrote != -1) printf("        expected write() to fail, it wrote %zd bytes\n", wrote);
}

int main()
{
	struct stat st;
	int fd;

	printf("### nowrite:\n");

	if (make_fixture() != 0)
	{
		fprintf(stderr, "!!! nowrite: could not create %s: %s\n", FIXTURE, strerror(errno));
		return 2;
	}
	unlink(ABSENT);

	// every flavor of "open this for writing" has to come back read-only
	case_downgraded("O_RDONLY stays readable", O_RDONLY);
	case_downgraded("O_WRONLY is downgraded to read-only", O_WRONLY);
	case_downgraded("O_RDWR is downgraded to read-only", O_RDWR);
	case_downgraded("O_WRONLY|O_TRUNC neither truncates nor writes", O_WRONLY | O_TRUNC);
	case_downgraded("O_WRONLY|O_TMPFILE is downgraded to read-only", O_WRONLY | O_TMPFILE);

	// O_TRUNC is stripped along with the rest, so the contents survive
	check(stat(FIXTURE, &st) == 0 && st.st_size == (off_t)(sizeof(fixture_contents) - 1),
	      "the fixture still has its original contents");

	// O_CREAT is stripped too, so a file that isn't there is never made
	fd = open(ABSENT, O_WRONLY | O_CREAT, 0644);
	if (fd >= 0) close(fd);
	check(fd < 0 && stat(ABSENT, &st) != 0, "O_WRONLY|O_CREAT does not create a new file");

	unlink(ABSENT);
	unlink(FIXTURE);

	printf("### nowrite: %s\n", failures ? "FAILED" : "passed");
	return failures ? 1 : 0;
}
