#include "expr/parse.h"

#include "compat/irix.h"
#include "exec/token.h"
#include "proto/config.h"
#include "version.h"

#include <stdlib.h>
#include <string.h>

/*
 * Context binding. Three of these are not where the documentation implies, and
 * were read off a live job message rather than inferred; docs/protocol.md
 * records the wire shapes.
 */

int
sgug_expr_ctx_own(sgug_expr_ctx *ctx, sgug_json_doc *doc)
{
	if (ctx->nowned == ctx->owned_cap) {
		size_t cap = ctx->owned_cap != 0 ? ctx->owned_cap * 2 : 8;
		sgug_json_doc **grown = realloc(ctx->owned,
		    cap * sizeof(*grown));

		if (grown == NULL) {
			sgug_json_free(doc);
			return -1;
		}
		ctx->owned = grown;
		ctx->owned_cap = cap;
	}
	ctx->owned[ctx->nowned++] = doc;
	return 0;
}

static void
put_str(sgug_jsonw *w, const char *k, const char *v)
{
	sgug_jsonw_key(w, k);
	sgug_jsonw_str(w, v != NULL ? v : "");
}

/*
 * Builds runner, env and secrets as one document. Going through the JSON
 * writer and parser rather than inventing a second container type keeps every
 * value in the evaluator a plain sgug_json node.
 */
static sgug_json_doc *
build_synth(const sgug_job *job, const sgug_config *cfg, const char *workspace,
    const char *temp)
{
	sgug_jsonw *w = sgug_jsonw_new();
	sgug_json_doc *doc = NULL;
	const char *text;
	size_t len, i, n;

	if (w == NULL)
		return NULL;

	sgug_jsonw_obj_begin(w);

	sgug_jsonw_key(w, "runner");
	sgug_jsonw_obj_begin(w);
	put_str(w, "os", sgug_runner_os());
	put_str(w, "arch", "X64");
	put_str(w, "name", cfg != NULL ? cfg->agent_name : "");
	put_str(w, "temp", temp);
	put_str(w, "tool_cache", temp);
	put_str(w, "workspace", workspace);
	sgug_jsonw_obj_end(w);

	sgug_jsonw_key(w, "secrets");
	sgug_jsonw_obj_begin(w);
	if (job != NULL && job->doc != NULL) {
		const sgug_json *vars = sgug_json_get(
		    sgug_json_root(job->doc), "variables");

		n = sgug_json_len(vars);
		for (i = 0; i < n; i++) {
			const char *k = NULL;
			const sgug_json *v = NULL;

			if (sgug_json_member(vars, i, &k, &v) != 0)
				continue;
			if (!sgug_json_bool(sgug_json_get(v, "isSecret"), 0))
				continue;
			put_str(w, k,
			    sgug_json_string(sgug_json_get(v, "value"), ""));
		}
	}
	sgug_jsonw_obj_end(w);

	sgug_jsonw_obj_end(w);

	text = sgug_jsonw_done(w, &len);
	if (text != NULL)
		doc = sgug_json_parse(text, len, NULL, 0);
	sgug_jsonw_free(w);
	return doc;
}

sgug_expr_ctx *
sgug_expr_ctx_new(const sgug_job *job, const sgug_config *cfg,
    const char *workspace, const char *temp)
{
	sgug_expr_ctx *ctx = calloc(1, sizeof(*ctx));

	if (ctx == NULL)
		return NULL;

	ctx->arena = sgug_expr_arena_new();
	if (ctx->arena == NULL) {
		free(ctx);
		return NULL;
	}
	ctx->job = job;
	ctx->synth = build_synth(job, cfg, workspace, temp);
	return ctx;
}

sgug_expr_ctx *
sgug_expr_ctx_new_json(const sgug_json *contexts)
{
	sgug_expr_ctx *ctx = calloc(1, sizeof(*ctx));

	if (ctx == NULL)
		return NULL;

	ctx->arena = sgug_expr_arena_new();
	if (ctx->arena == NULL) {
		free(ctx);
		return NULL;
	}
	ctx->contexts = contexts;
	return ctx;
}

void
sgug_expr_ctx_free(sgug_expr_ctx *ctx)
{
	size_t i;

	if (ctx == NULL)
		return;
	for (i = 0; i < ctx->nowned; i++)
		sgug_json_free(ctx->owned[i]);
	free(ctx->owned);
	sgug_json_free(ctx->synth);
	sgug_expr_arena_free(ctx->arena);
	free(ctx);
}

void
sgug_expr_ctx_set_step_env(sgug_expr_ctx *ctx, const sgug_json *env)
{
	ctx->step_env = env;
	ctx->env = NULL;
}

void
sgug_expr_ctx_set_status(sgug_expr_ctx *ctx, int failed, int cancelled)
{
	ctx->job_failed = failed;
	ctx->job_cancelled = cancelled;
}

/*
 * env has to be flattened because it arrives as several scopes. Later scopes
 * win, so the step's own block is appended last.
 */
static const sgug_json *
build_env(sgug_expr_ctx *ctx)
{
	sgug_jsonw *w = sgug_jsonw_new();
	sgug_json_doc *doc = NULL;
	const sgug_json *scopes;
	const char *text;
	size_t len, i, n;

	if (w == NULL)
		return NULL;

	sgug_jsonw_obj_begin(w);

	scopes = ctx->job != NULL && ctx->job->doc != NULL
	    ? sgug_json_get(sgug_json_root(ctx->job->doc),
	    "environmentVariables") : NULL;

	n = sgug_json_len(scopes);
	for (i = 0; i < n; i++) {
		const sgug_json *scope = sgug_json_at(scopes, i);
		size_t j, m = sgug_token_len(scope);

		for (j = 0; j < m; j++) {
			const char *k = NULL;
			const sgug_json *v = NULL;

			if (sgug_token_map_at(scope, j, &k, &v) != 0)
				continue;
			put_str(w, k, sgug_token_str(v, ""));
		}
	}

	if (ctx->step_env != NULL) {
		size_t j, m = sgug_token_len(ctx->step_env);

		for (j = 0; j < m; j++) {
			const char *k = NULL;
			const sgug_json *v = NULL;

			if (sgug_token_map_at(ctx->step_env, j, &k, &v) != 0)
				continue;
			put_str(w, k, sgug_token_str(v, ""));
		}
	}

	sgug_jsonw_obj_end(w);

	text = sgug_jsonw_done(w, &len);
	if (text != NULL) {
		doc = sgug_json_parse(text, len, NULL, 0);
		if (doc != NULL && sgug_expr_ctx_own(ctx, doc) != 0)
			doc = NULL;
	}
	sgug_jsonw_free(w);

	ctx->env = doc != NULL ? sgug_json_root(doc) : NULL;
	return ctx->env;
}

const sgug_json *
sgug_expr_context_lookup(sgug_expr_ctx *ctx, const char *name)
{
	const sgug_json *v;

	/* The test entry point supplies contexts directly. */
	if (ctx->contexts != NULL)
		return sgug_json_get(ctx->contexts, name);

	if (ctx->synth != NULL) {
		v = sgug_json_get(sgug_json_root(ctx->synth), name);
		if (v != NULL)
			return v;
	}

	if (strcmp(name, "env") == 0)
		return ctx->env != NULL ? ctx->env : build_env(ctx);

	if (ctx->job != NULL && ctx->job->doc != NULL) {
		v = sgug_json_get(sgug_json_get(sgug_json_root(ctx->job->doc),
		    "contextData"), name);
		if (v != NULL)
			return v;
	}
	return NULL;
}
