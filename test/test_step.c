/*
 * Runs real child processes, so it exercises the same fork, pipe and poll path
 * on IRIX as in production. Nothing here touches the network.
 */

#include "exec/step.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++; \
		} \
	} while (0)

#define CHECK_EQ_INT(got, want) \
	do { \
		long g = (long)(got); \
		long w = (long)(want); \
		if (g != w) { \
			printf("FAIL %s:%d: %s = %ld, want %ld\n", \
			    __FILE__, __LINE__, #got, g, w); \
			failures++; \
		} \
	} while (0)

#define CHECK_EQ_STR(got, want) \
	do { \
		const char *g = (got); \
		if (g == NULL || strcmp(g, (want)) != 0) { \
			printf("FAIL %s:%d: %s = \"%s\", want \"%s\"\n", \
			    __FILE__, __LINE__, #got, g ? g : "(null)", (want)); \
			failures++; \
		} \
	} while (0)

#define MAX_CAPTURED 64

struct capture {
	char *lines[MAX_CAPTURED];
	size_t n;
};

static void
collect(void *ctx, const char *line)
{
	struct capture *c = ctx;

	if (c->n < MAX_CAPTURED)
		c->lines[c->n++] = strdup(line);
}

static void
capture_free(struct capture *c)
{
	size_t i;

	for (i = 0; i < c->n; i++)
		free(c->lines[i]);
	c->n = 0;
}

static void
run(const char *script, struct capture *cap, int *status, const char *shell)
{
	sgug_step st;
	sgug_step_opts o;
	char err[256];

	memset(&st, 0, sizeof(st));
	st.script = script;
	st.shell = shell;

	memset(&o, 0, sizeof(o));
	o.work_dir = "/tmp";
	o.temp_dir = "/tmp";
	o.timeout_seconds = 30;

	err[0] = '\0';
	*status = sgug_step_run(&st, &o, collect, cap, err, sizeof(err));
	if (*status < 0 && err[0] != '\0')
		printf("  (step error: %s)\n", err);
}

static void
test_output_and_status(void)
{
	struct capture c;
	int status;

	memset(&c, 0, sizeof(c));
	run("echo one\necho two\n", &c, &status, NULL);
	CHECK_EQ_INT(status, 0);
	CHECK_EQ_INT((long)c.n, 2);
	if (c.n >= 2) {
		CHECK_EQ_STR(c.lines[0], "one");
		CHECK_EQ_STR(c.lines[1], "two");
	}
	capture_free(&c);

	/* Exit status propagates, which is what fails a step. */
	memset(&c, 0, sizeof(c));
	run("echo before\nexit 3\n", &c, &status, NULL);
	CHECK_EQ_INT(status, 3);
	CHECK_EQ_INT((long)c.n, 1);
	capture_free(&c);

	/* stderr is interleaved into the same stream, as the Actions log shows
	 * it. */
	memset(&c, 0, sizeof(c));
	run("echo out\necho err 1>&2\n", &c, &status, NULL);
	CHECK_EQ_INT(status, 0);
	CHECK_EQ_INT((long)c.n, 2);
	capture_free(&c);

	/* A final line without a trailing newline must still be emitted. */
	memset(&c, 0, sizeof(c));
	run("printf 'no newline'\n", &c, &status, NULL);
	CHECK_EQ_INT(status, 0);
	CHECK_EQ_INT((long)c.n, 1);
	if (c.n >= 1)
		CHECK_EQ_STR(c.lines[0], "no newline");
	capture_free(&c);

	/* An empty script is a no-op, not a failure. */
	memset(&c, 0, sizeof(c));
	run("", &c, &status, NULL);
	CHECK_EQ_INT(status, 0);
	CHECK_EQ_INT((long)c.n, 0);
	capture_free(&c);
}

static void
test_multiline_and_quoting(void)
{
	struct capture c;
	int status;

	/*
	 * The script goes to a file rather than through -c precisely so that
	 * quotes, newlines and backslashes survive untouched.
	 */
	memset(&c, 0, sizeof(c));
	run("echo \"double 'single' quotes\"\necho 'back\\\\slash'\n", &c,
	    &status, NULL);
	CHECK_EQ_INT(status, 0);
	CHECK_EQ_INT((long)c.n, 2);
	if (c.n >= 2)
		CHECK_EQ_STR(c.lines[0], "double 'single' quotes");
	capture_free(&c);

	/* Shell state persists across lines within one step. */
	memset(&c, 0, sizeof(c));
	run("X=hello\necho $X world\n", &c, &status, NULL);
	CHECK_EQ_INT(status, 0);
	if (c.n >= 1)
		CHECK_EQ_STR(c.lines[0], "hello world");
	capture_free(&c);
}

static void
test_no_stdin(void)
{
	struct capture c;
	int status;

	/* stdin is /dev/null, so a step that reads it gets EOF rather than
	 * blocking forever on an inherited terminal. */
	memset(&c, 0, sizeof(c));
	run("cat; echo done\n", &c, &status, NULL);
	CHECK_EQ_INT(status, 0);
	CHECK(c.n >= 1);
	if (c.n >= 1)
		CHECK_EQ_STR(c.lines[c.n - 1], "done");
	capture_free(&c);
}

static int always_abort(void *ctx) { (void)ctx; return 1; }

static void
test_abort(void)
{
	sgug_step st;
	sgug_step_opts o;
	struct capture c;
	char err[256];
	int status;

	memset(&c, 0, sizeof(c));
	memset(&st, 0, sizeof(st));
	st.script = "sleep 30\n";

	memset(&o, 0, sizeof(o));
	o.work_dir = "/tmp";
	o.temp_dir = "/tmp";
	o.timeout_seconds = 30;
	o.abort_cb = always_abort;

	err[0] = '\0';
	status = sgug_step_run(&st, &o, collect, &c, err, sizeof(err));
	CHECK_EQ_INT(status, SGUG_STEP_ABORTED);
	capture_free(&c);
}

static void
test_timeout(void)
{
	sgug_step st;
	sgug_step_opts o;
	struct capture c;
	char err[256];
	int status;

	memset(&c, 0, sizeof(c));
	memset(&st, 0, sizeof(st));
	st.script = "sleep 30\n";

	memset(&o, 0, sizeof(o));
	o.work_dir = "/tmp";
	o.temp_dir = "/tmp";
	o.timeout_seconds = 1;

	err[0] = '\0';
	status = sgug_step_run(&st, &o, collect, &c, err, sizeof(err));
	CHECK_EQ_INT(status, SGUG_STEP_ABORTED);
	capture_free(&c);
}

/*
 * A failing command must end the script. Without -e the status is the last
 * command's, so a step that checks something and then prints a summary passes
 * no matter what the check found, which makes every gate in a workflow
 * decorative.
 */
static void
test_stops_on_first_failure(void)
{
	sgug_step st;
	sgug_step_opts o;
	struct capture c;
	char err[256];
	int status;

	memset(&c, 0, sizeof(c));
	memset(&st, 0, sizeof(st));
	st.script = "echo first\nfalse\necho unreachable\n";

	memset(&o, 0, sizeof(o));
	o.work_dir = "/tmp";
	o.temp_dir = "/tmp";
	o.timeout_seconds = 30;

	err[0] = '\0';
	status = sgug_step_run(&st, &o, collect, &c, err, sizeof(err));
	CHECK(status != 0);
	CHECK_EQ_INT((int)c.n, 1);
	if (c.n >= 1)
		CHECK_EQ_STR(c.lines[0], "first");
	capture_free(&c);
}

/* pipefail: a failure upstream of a pipe must not be hidden by a good tail. */
static void
test_pipefail(void)
{
	sgug_step st;
	sgug_step_opts o;
	struct capture c;
	char err[256];
	int status;

	memset(&c, 0, sizeof(c));
	memset(&st, 0, sizeof(st));
	st.shell = "bash";
	st.script = "false | cat\n";

	memset(&o, 0, sizeof(o));
	o.work_dir = "/tmp";
	o.temp_dir = "/tmp";
	o.timeout_seconds = 30;

	err[0] = '\0';
	status = sgug_step_run(&st, &o, collect, &c, err, sizeof(err));
	CHECK(status != 0);
	capture_free(&c);
}

static void
test_working_directory(void)
{
	sgug_step st;
	sgug_step_opts o;
	struct capture c;
	char err[256];
	int status;

	mkdir("/tmp/sgug-step-wd", 0755);

	memset(&c, 0, sizeof(c));
	memset(&st, 0, sizeof(st));
	st.script = "pwd\n";
	st.working_directory = "/tmp/sgug-step-wd";

	memset(&o, 0, sizeof(o));
	o.work_dir = "/tmp";
	o.temp_dir = "/tmp";
	o.timeout_seconds = 30;

	err[0] = '\0';
	status = sgug_step_run(&st, &o, collect, &c, err, sizeof(err));
	CHECK_EQ_INT(status, 0);
	if (c.n >= 1)
		CHECK_EQ_STR(c.lines[0], "/tmp/sgug-step-wd");
	capture_free(&c);

	rmdir("/tmp/sgug-step-wd");
}

static void
test_script_file_removed(void)
{
	struct capture c;
	int status;
	char path[512];

	/* The temp script must not survive the step. */
	memset(&c, 0, sizeof(c));
	run("echo hi\n", &c, &status, NULL);
	CHECK_EQ_INT(status, 0);
	capture_free(&c);

	sgug_snprintf(path, sizeof(path), "/tmp/step-%ld.sh", (long)getpid());
	CHECK(access(path, F_OK) != 0);
}

int
main(void)
{
	test_output_and_status();
	test_multiline_and_quoting();
	test_no_stdin();
	test_abort();
	test_timeout();
	test_stops_on_first_failure();
	test_pipefail();
	test_working_directory();
	test_script_file_removed();

	if (failures != 0) {
		printf("\n%d failure(s)\n", failures);
		return 1;
	}
	printf("all step tests passed\n");
	return 0;
}
