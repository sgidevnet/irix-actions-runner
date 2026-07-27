/*
 * Driven off test/fixtures/job-message.json, which mirrors a real message
 * captured from github.com. The shapes matter more than the values: the
 * reference documentation describes only one of the two serialisations that
 * appear in a single message, and a parser written from it reads every step
 * input as absent without erroring.
 */

#include "exec/job.h"
#include "exec/token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
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

static char *
slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);

	buf = malloc((size_t)n + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[n] = '\0';
	if (len != NULL)
		*len = (size_t)n;
	return buf;
}

/* Both spellings, since one message contains both. */
static void
test_token_forms(void)
{
	static const char *VERBOSE =
	    "{\"type\":2,\"map\":[{\"Key\":{\"type\":0,\"lit\":\"script\"},"
	    "\"Value\":{\"type\":0,\"file\":1,\"line\":21,\"lit\":\"make -j2\"}},"
	    "{\"Key\":{\"type\":0,\"lit\":\"shell\"},"
	    "\"Value\":{\"type\":0,\"lit\":\"bash\"}}]}";
	static const char *COMPACT =
	    "{\"t\":2,\"d\":[{\"k\":\"ref\",\"v\":\"refs/heads/main\"},"
	    "{\"k\":\"check_run_id\",\"v\":90005817181}]}";
	sgug_json_doc *a = sgug_json_parse(VERBOSE, strlen(VERBOSE), NULL, 0);
	sgug_json_doc *b = sgug_json_parse(COMPACT, strlen(COMPACT), NULL, 0);

	CHECK(a != NULL && b != NULL);
	if (a == NULL || b == NULL)
		return;

	CHECK_EQ_STR(sgug_token_map_str(sgug_json_root(a), "script", NULL),
	    "make -j2");
	CHECK_EQ_STR(sgug_token_map_str(sgug_json_root(a), "shell", NULL), "bash");
	CHECK(sgug_token_map_get(sgug_json_root(a), "absent") == NULL);
	CHECK(sgug_token_len(sgug_json_root(a)) == 2);

	/* Compact values are raw JSON, not tagged objects. */
	CHECK_EQ_STR(sgug_token_map_str(sgug_json_root(b), "ref", NULL),
	    "refs/heads/main");
	CHECK(sgug_token_len(sgug_json_root(b)) == 2);
	/* A number is not a string; the accessor must fall back rather than
	 * return a pointer into a non-string node. */
	CHECK_EQ_STR(sgug_token_map_str(sgug_json_root(b), "check_run_id", "x"),
	    "x");

	sgug_json_free(a);
	sgug_json_free(b);
}

/*
 * The service compiles a `run:` body containing ${{ }} into a type 3
 * BasicExpression, and an interpolated one into a format() call over the
 * literal parts. Neither carries a `lit`, so reading it as a scalar yields the
 * fallback. That must not look like an absent key: an empty script is treated
 * as a no-op and the step is reported as having succeeded without running.
 */
static void
test_expression_script_is_rejected(void)
{
	static const char *WHOLE =
	    "{\"type\":2,\"map\":[{\"Key\":{\"type\":0,\"lit\":\"script\"},"
	    "\"Value\":{\"type\":3,\"expr\":\"github.sha\"}}]}";
	static const char *INTERPOLATED =
	    "{\"type\":2,\"map\":[{\"Key\":{\"type\":0,\"lit\":\"script\"},"
	    "\"Value\":{\"type\":3,"
	    "\"expr\":\"format('echo {0}', github.sha)\"}}]}";
	static const char *LITERAL =
	    "{\"type\":2,\"map\":[{\"Key\":{\"type\":0,\"lit\":\"script\"},"
	    "\"Value\":{\"type\":0,\"lit\":\"echo hi\"}}]}";
	sgug_json_doc *a = sgug_json_parse(WHOLE, strlen(WHOLE), NULL, 0);
	sgug_json_doc *b = sgug_json_parse(INTERPOLATED, strlen(INTERPOLATED),
	    NULL, 0);
	sgug_json_doc *c = sgug_json_parse(LITERAL, strlen(LITERAL), NULL, 0);

	CHECK(a != NULL && b != NULL && c != NULL);
	if (a == NULL || b == NULL || c == NULL)
		return;

	CHECK(!sgug_token_is_literal(
	    sgug_token_map_get(sgug_json_root(a), "script")));
	CHECK(!sgug_token_is_literal(
	    sgug_token_map_get(sgug_json_root(b), "script")));
	CHECK(sgug_token_is_literal(
	    sgug_token_map_get(sgug_json_root(c), "script")));

	/* An absent key is a third case, and is not an expression. */
	CHECK(!sgug_token_is_literal(
	    sgug_token_map_get(sgug_json_root(c), "absent")));

	sgug_json_free(a);
	sgug_json_free(b);
	sgug_json_free(c);
}

static void
test_parse_fixture(void)
{
	sgug_job job;
	char err[256];
	char *text;
	size_t len;

	text = slurp("test/fixtures/job-message.json", &len);
	if (text == NULL) {
		printf("FAIL: cannot read test/fixtures/job-message.json\n");
		failures++;
		return;
	}

	err[0] = '\0';
	if (sgug_job_parse(text, len, &job, err, sizeof(err)) != 0) {
		printf("FAIL: parse: %s\n", err);
		failures++;
		free(text);
		return;
	}

	CHECK_EQ_STR(job.plan_id, "7a0b3ee8-049d-459e-9aab-a558d241b0fc");
	/* "actions", not "Build". This is the hub name in every reporting URL,
	 * so a wrong value 404s all of them. */
	CHECK_EQ_STR(job.plan_type, "actions");
	/* The service reuses planId as the timeline id. */
	CHECK_EQ_STR(job.timeline_id, job.plan_id);
	CHECK_EQ_STR(job.job_id, "0215aca7-e825-533a-9d67-e18a65311193");
	CHECK_EQ_STR(job.job_display_name, "smoke");
	CHECK_EQ_STR(job.job_name, "__default");

	CHECK_EQ_STR(job.service_url,
	    "https://run-actions-1-azure-eastus.actions.githubusercontent.com/177/");
	CHECK_EQ_STR(job.access_token, "SYNTHETIC-JOB-TOKEN");
	CHECK(strstr(job.pipelines_url, "pipelinesghubeus") != NULL);

	CHECK(job.nsteps == 2);
	if (job.nsteps >= 2) {
		CHECK(job.steps[0].kind == SGUG_STEP_SCRIPT);
		CHECK_EQ_STR(job.steps[0].display_name, "Identify the machine");
		CHECK_EQ_STR(job.steps[0].context_name, "__run");
		CHECK_EQ_STR(job.steps[0].condition, "success()");
		/* The whole point: a compact-only reader returns "" here. */
		CHECK_EQ_STR(job.steps[0].script, "uname -a\nhinv | head -5\n");
		CHECK(job.steps[0].shell == NULL);
		CHECK(job.steps[0].working_directory == NULL);

		CHECK_EQ_STR(job.steps[1].context_name, "__run_2");
		CHECK_EQ_STR(job.steps[1].shell, "bash");
		CHECK_EQ_STR(job.steps[1].working_directory, "sub/dir");
	}

	/* Literal secrets are collected, regexes skipped. */
	CHECK(job.nmasks == 1);
	if (job.nmasks == 1)
		CHECK_EQ_STR(job.masks[0], "SYNTHETIC-GITHUB-TOKEN");

	CHECK_EQ_STR(sgug_job_context(&job, "github", "repository", NULL),
	    "sgidevnet/irix-actions-runner");
	CHECK_EQ_STR(sgug_job_context(&job, "github", "sha", NULL),
	    "b8d1e6523d9961079ec9dc634035b8960498bf1b");
	CHECK_EQ_STR(sgug_job_context(&job, "github", "absent", "dflt"), "dflt");
	CHECK_EQ_STR(sgug_job_context(&job, "nosuch", "x", "dflt"), "dflt");

	sgug_job_free(&job);
	free(text);
}

static void
test_rejects_incomplete(void)
{
	static const char *NO_ENDPOINT =
	    "{\"plan\":{\"planId\":\"p\"},\"jobId\":\"j\",\"steps\":[],"
	    "\"resources\":{\"endpoints\":[]}}";
	static const char *NO_IDS = "{\"steps\":[]}";
	sgug_job job;
	char err[256];

	CHECK(sgug_job_parse(NO_ENDPOINT, strlen(NO_ENDPOINT), &job, err,
	    sizeof(err)) != 0);
	CHECK(strstr(err, "SystemVssConnection") != NULL);

	CHECK(sgug_job_parse(NO_IDS, strlen(NO_IDS), &job, err, sizeof(err)) != 0);
	CHECK(strstr(err, "planId") != NULL);

	CHECK(sgug_job_parse("not json", 8, &job, err, sizeof(err)) != 0);
}

int
main(void)
{
	test_token_forms();
	test_expression_script_is_rejected();
	test_parse_fixture();
	test_rejects_incomplete();

	if (failures != 0) {
		printf("\n%d failure(s)\n", failures);
		return 1;
	}
	printf("all job tests passed\n");
	return 0;
}
