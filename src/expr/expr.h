#ifndef SGUG_EXPR_EXPR_H
#define SGUG_EXPR_EXPR_H

#include "exec/job.h"
#include "json/json.h"
#include "proto/config.h"

#include <stddef.h>

/*
 * The GitHub Actions expression language, the thing inside ${{ }}.
 *
 * No formal grammar is published for it anywhere: not by GitHub, not in
 * actionlint, not in the docs. src/expr/grammar.y is written from the C# in
 * actions/runner, which is the only specification there is. docs/protocol.md
 * records what the wire carries.
 *
 * Two surprises worth knowing before reading any of this. There is no
 * arithmetic, so `-` is a legal character inside an identifier. And equality
 * coerces the way JavaScript's == does, so '' == 0 and null == false are both
 * true while true == 'true' is false.
 */

typedef enum {
	SGUG_EXPR_NULL,
	SGUG_EXPR_BOOL,
	SGUG_EXPR_NUMBER,
	SGUG_EXPR_STRING,
	SGUG_EXPR_OBJECT,
	SGUG_EXPR_ARRAY
} sgug_expr_kind;

typedef struct sgug_expr_arena sgug_expr_arena;
typedef struct sgug_expr_node sgug_expr_node;
typedef struct sgug_expr_array sgug_expr_array;

/*
 * Numbers are double throughout, matching the reference. Objects and arrays
 * are usually borrowed from the job document; fromJSON and the * filter build
 * their own, which is what `arr` and the owned `doc` on the context are for.
 */
typedef struct {
	sgug_expr_kind kind;
	int b;
	double n;
	const char *s;
	const sgug_json *node;
	const sgug_expr_array *arr;
	/* Produced by a * filter. Indexing one flattens a level instead of
	 * subscripting, which is how obj.*.x collects every x. */
	int filtered;
} sgug_expr_value;

struct sgug_expr_array {
	const sgug_expr_value *items;
	size_t n;
};

typedef struct sgug_expr_ctx sgug_expr_ctx;

/*
 * Binds a job's contexts. Borrows the job, which must outlive the context.
 * runner, secrets and env are assembled rather than read off the message; see
 * docs/protocol.md.
 */
sgug_expr_ctx *sgug_expr_ctx_new(const sgug_job *job, const sgug_config *cfg,
    const char *workspace, const char *temp);
void sgug_expr_ctx_free(sgug_expr_ctx *ctx);

/* Layers a step's `environment` mapping over the job's. NULL clears it. */
void sgug_expr_ctx_set_step_env(sgug_expr_ctx *ctx, const sgug_json *env);

/* Job state so far, which is all the status functions read. */
void sgug_expr_ctx_set_status(sgug_expr_ctx *ctx, int failed, int cancelled);

/*
 * Evaluates src and writes the result as a string, the same conversion the
 * reference applies when an expression lands somewhere that wants text:
 * null becomes empty, an object becomes "Object".
 *
 * Returns 0, or -1 with err filled. A syntax error, an unknown function or an
 * unknown context is an error rather than an empty result: producing nothing
 * quietly is the failure this whole thing exists to remove.
 *
 * The result is owned by the caller.
 */
int sgug_expr_eval_string(sgug_expr_ctx *ctx, const char *src, char **out,
    char *err, size_t errlen);

/* Evaluates for truth, applying the language's falsiness: null, false, 0, NaN
 * and the empty string are false, and every object or array is true even when
 * it is empty. */
int sgug_expr_eval_bool(sgug_expr_ctx *ctx, const char *src, int *out,
    char *err, size_t errlen);

/*
 * A TemplateToken that is either a literal or a type 3 expression. Interpolated
 * scalars arrive already compiled into format('...', ...), so this covers a
 * `run:` body and a `with:` value alike.
 */
int sgug_expr_eval_token(sgug_expr_ctx *ctx, const sgug_json *tok, char **out,
    char *err, size_t errlen);

/*
 * Exposed for the tests, which drive the vendored conformance corpus. The
 * corpus supplies contexts as plain JSON rather than a job message.
 *
 * A string or array in the result is arena backed and only valid until the
 * next call on this context. The three entry points above copy before
 * returning; this one does not.
 */
sgug_expr_ctx *sgug_expr_ctx_new_json(const sgug_json *contexts);
int sgug_expr_eval(sgug_expr_ctx *ctx, const char *src, sgug_expr_value *out,
    char *err, size_t errlen);
const char *sgug_expr_kind_name(sgug_expr_kind k);

#endif /* SGUG_EXPR_EXPR_H */
