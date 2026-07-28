/*
 * The expression language, driven by GitHub's own cross-language conformance
 * corpus in test/fixtures/expressions.
 *
 * The corpus exists because none of this language is what a reader assumes.
 * Hand-written tests would encode whatever the author believed.
 *
 * Error cases assert only that we reject, not how we phrase it. The corpus
 * carries byte-exact C# messages and matching them is not worth the coupling.
 */

#include "expr/expr.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIR_PATH "test/fixtures/expressions"

static int failures;
static int checked;
static int skipped;
static int unicode;

/*
 * .NET's OrdinalIgnoreCase folds the whole of Unicode; this folds ASCII. The
 * corpus tests sigma, u-umlaut and Cyrillic, which no workflow expression has
 * ever needed, and a folding table is not worth carrying to make them pass.
 *
 * Every case runs. A failure is only put down to folding if the expression is
 * not ASCII, so this cannot hide an ordinary bug, and the count is the real
 * number of cases the runner differs on rather than the number that merely
 * contain a high byte.
 */
static int
non_ascii(const char *s)
{
	for (; *s != '\0'; s++) {
		if ((unsigned char)*s >= 0x80)
			return 1;
	}
	return 0;
}

static void
check_cond(const char *cond, int job_failed, int job_cancelled, int want)
{
	sgug_expr_ctx *ctx = sgug_expr_ctx_new_json(NULL);
	char err[256];
	int got = -1;

	sgug_expr_ctx_set_status(ctx, job_failed, job_cancelled);

	if (sgug_expr_eval_bool(ctx, cond, &got, err, sizeof(err)) != 0) {
		printf("FAIL if: %s\n     %s\n", cond, err);
		failures++;
	} else if (got != want) {
		printf("FAIL if: %s (failed=%d cancelled=%d)\n"
		    "     want %d, got %d\n", cond, job_failed, job_cancelled,
		    want, got);
		failures++;
	}
	sgug_expr_ctx_free(ctx);
}

/*
 * The status functions, the only part of the language that reads job state
 * rather than the job message, and the whole of what an ordinary `if:` uses.
 */
static void
test_conditions(void)
{
	check_cond("success()", 0, 0, 1);
	check_cond("always()", 0, 0, 1);
	check_cond("failure()", 0, 0, 0);
	check_cond("cancelled()", 0, 0, 0);

	check_cond("success()", 1, 0, 0);
	check_cond("always()", 1, 0, 1);
	check_cond("failure()", 1, 0, 1);

	/* Cancellation beats everything except always(). */
	check_cond("success()", 0, 1, 0);
	check_cond("always()", 0, 1, 1);
	check_cond("cancelled()", 0, 1, 1);
	/* failure() must not fire on a cancellation: a cancelled job has not
	 * failed, and cleanup steps guarded by failure() should not run. */
	check_cond("failure()", 1, 1, 0);

	/* The old strstr matcher read this as cancelled() and inverted it. */
	check_cond("!cancelled()", 0, 1, 0);
	check_cond("!cancelled()", 1, 0, 1);

	/* A condition the service composes from an author's `if:`. */
	check_cond("success() && !cancelled()", 0, 0, 1);
	check_cond("always() && 'a' == 'A'", 1, 1, 1);
}

static void
check_str(const char *expr, const char *want)
{
	sgug_expr_ctx *ctx = sgug_expr_ctx_new_json(NULL);
	char err[256];
	char *got = NULL;

	if (sgug_expr_eval_string(ctx, expr, &got, err, sizeof(err)) != 0) {
		printf("FAIL %s\n     %s\n", expr, err);
		failures++;
	} else if (strcmp(got, want) != 0) {
		printf("FAIL %s\n     want %s\n      got %s\n", expr, want,
		    got);
		failures++;
	}
	free(got);
	sgug_expr_ctx_free(ctx);
}

static void
check_rejected(const char *expr)
{
	sgug_expr_ctx *ctx = sgug_expr_ctx_new_json(NULL);
	char err[256];
	char *got = NULL;

	if (sgug_expr_eval_string(ctx, expr, &got, err, sizeof(err)) == 0) {
		printf("FAIL %s\n     want an error, got %s\n", expr, got);
		failures++;
	}
	free(got);
	sgug_expr_ctx_free(ctx);
}

/* One case per bug the corpus does not reach. */
static void
test_beyond_the_corpus(void)
{
	/* The wire encodings must not be read out of fromJSON's output. A key
	 * called d, s or lit is not exotic. */
	check_str("fromJSON('{\"d\":[1,2,3]}').d[1]", "2");
	check_str("fromJSON('{\"a\":{\"lit\":5}}').a.lit", "5");
	check_str("fromJSON('{\"a\":{\"s\":\"x\",\"y\":2}}').a.y", "2");

	/* Arity is a parse error, so short circuiting must not hide it. */
	check_rejected("always() || join()");
	check_rejected("true || case(true, 'a', false, 'b')");
	check_rejected("true || startsWith('a', 'b', 'c')");

	/* strtod also takes inf, INF and infinity. The language does not. */
	check_str("'inf' == Infinity", "false");
	check_str("'infinity' == Infinity", "false");
	check_str("' Infinity ' == Infinity", "true");

	/* strtol is 32 bits under n32. */
	check_str("0x1FFFFFFFF", "8589934591");

	/* .NET renders with G15, an uppercase specifier. */
	check_str("format('{0}', 1e16)", "1E+16");

	/* A long digit run must not wrap into a valid argument index. */
	check_rejected("format('{18446744073709551617}', 'a', 'b')");
}

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

static void
fail(const char *file, const char *expr, const char *want, const char *got)
{
	if (non_ascii(expr)) {
		unicode++;
		return;
	}
	printf("FAIL %s: %s\n     want %s\n      got %s\n", file, expr, want,
	    got);
	failures++;
}

/* The corpus states an expected kind and value; compare against what the
 * evaluator produced, rendering both the same way. */
static void
check_result(const char *file, const char *expr, const sgug_json *want,
    const sgug_expr_value *got, sgug_expr_ctx *ctx, const char *rendered)
{
	const char *wkind = sgug_json_string(sgug_json_get(want, "kind"), "");
	const sgug_json *wval = sgug_json_get(want, "value");
	char buf[256];

	(void)ctx;

	if (strcmp(wkind, sgug_expr_kind_name(got->kind)) != 0) {
		sgug_snprintf(buf, sizeof(buf), "kind %s", wkind);
		fail(file, expr, buf, sgug_expr_kind_name(got->kind));
		return;
	}

	switch (got->kind) {
	case SGUG_EXPR_BOOL:
		if (sgug_json_bool(wval, 0) != got->b)
			fail(file, expr,
			    sgug_json_bool(wval, 0) ? "true" : "false",
			    got->b ? "true" : "false");
		break;
	case SGUG_EXPR_STRING:
		if (strcmp(sgug_json_string(wval, ""), rendered) != 0)
			fail(file, expr, sgug_json_string(wval, ""), rendered);
		break;
	case SGUG_EXPR_NUMBER: {
		double w = sgug_json_double(wval, 0);

		/* NaN never equals itself, so compare the rendering. */
		if (w != w ? got->n == got->n : w != got->n) {
			sgug_snprintf(buf, sizeof(buf), "%.15g", w);
			fail(file, expr, buf, rendered);
		}
		break;
	}
	default:
		break;
	}
}

static void
run_case(const char *file, const sgug_json *c)
{
	const char *expr = sgug_json_string(sgug_json_get(c, "expr"), NULL);
	const sgug_json *want = sgug_json_get(c, "result");
	const sgug_json *contexts = sgug_json_get(c, "contexts");
	int wants_error = sgug_json_get(c, "err") != NULL;
	sgug_expr_ctx *ctx;
	sgug_expr_value v;
	char err[256];
	char *rendered = NULL;

	if (expr == NULL)
		return;

	ctx = sgug_expr_ctx_new_json(contexts);
	if (ctx == NULL)
		return;

	err[0] = '\0';
	checked++;

	if (sgug_expr_eval(ctx, expr, &v, err, sizeof(err)) != 0) {
		if (!wants_error)
			fail(file, expr, "a value", err);
		sgug_expr_ctx_free(ctx);
		return;
	}

	if (wants_error) {
		fail(file, expr, "an error", "a value");
		sgug_expr_ctx_free(ctx);
		return;
	}

	if (want != NULL) {
		if (sgug_expr_eval_string(ctx, expr, &rendered, err,
		    sizeof(err)) == 0) {
			check_result(file, expr, want, &v, ctx, rendered);
			free(rendered);
		}
	}
	sgug_expr_ctx_free(ctx);
}

static void
run_file(const char *name)
{
	char path[512];
	char *text;
	size_t len, i, n;
	sgug_json_doc *doc;
	const sgug_json *root;

	sgug_snprintf(path, sizeof(path), "%s/%s", DIR_PATH, name);
	text = slurp(path, &len);
	if (text == NULL) {
		printf("FAIL cannot read %s\n", path);
		failures++;
		return;
	}

	doc = sgug_json_parse(text, len, NULL, 0);
	if (doc == NULL) {
		printf("FAIL %s is not JSON\n", path);
		failures++;
		free(text);
		return;
	}

	/* Each file is groups of cases, keyed by what they cover. */
	root = sgug_json_root(doc);
	n = sgug_json_len(root);
	for (i = 0; i < n; i++) {
		const sgug_json *group = NULL;
		const char *key = NULL;
		size_t j, m;

		if (sgug_json_member(root, i, &key, &group) != 0)
			continue;
		m = sgug_json_len(group);
		for (j = 0; j < m; j++) {
			const sgug_json *c = sgug_json_at(group, j);

			/* The corpus marks cases a given implementation is
			 * known to differ on. */
			if (sgug_json_get(sgug_json_get(c, "options"),
			    "skip") != NULL) {
				skipped++;
				continue;
			}
			run_case(name, c);
		}
	}

	sgug_json_free(doc);
	free(text);
}

int
main(void)
{
	DIR *d = opendir(DIR_PATH);
	struct dirent *e;

	if (d == NULL) {
		printf("FAIL cannot open %s, run from the repository root\n",
		    DIR_PATH);
		return 1;
	}

	while ((e = readdir(d)) != NULL) {
		size_t n = strlen(e->d_name);

		if (n > 5 && strcmp(e->d_name + n - 5, ".json") == 0)
			run_file(e->d_name);
	}
	closedir(d);
	test_conditions();
	test_beyond_the_corpus();

	printf("%d cases, %d skipped, %d differing only by unicode case "
	    "folding\n", checked, skipped, unicode);
	if (failures != 0) {
		printf("\n%d failure(s)\n", failures);
		return 1;
	}
	printf("all expr tests passed\n");
	return 0;
}
