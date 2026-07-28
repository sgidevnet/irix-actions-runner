#ifndef SGUG_EXPR_PARSE_H
#define SGUG_EXPR_PARSE_H

#include "expr/expr.h"

/* From ExpressionConstants.cs. Exceeding any of them is a parse error. */
#define SGUG_EXPR_MAX_LENGTH 21000
#define SGUG_EXPR_MAX_DEPTH 50
#define SGUG_EXPR_MAX_ARGS 255

/*
 * Stack protection, not a language limit: the depth the language cares about is
 * checked while parsing. A left-nested chain of && is one level to the language
 * and one frame per link here, so this has to be the looser of the two.
 */
#define SGUG_EXPR_EVAL_DEPTH 512

typedef enum {
	SGUG_EXPR_OP_OR,
	SGUG_EXPR_OP_AND,
	SGUG_EXPR_OP_EQ,
	SGUG_EXPR_OP_NE,
	SGUG_EXPR_OP_LT,
	SGUG_EXPR_OP_GT,
	SGUG_EXPR_OP_LE,
	SGUG_EXPR_OP_GE,
	SGUG_EXPR_OP_NOT,
	SGUG_EXPR_OP_INDEX
} sgug_expr_op;

typedef enum {
	SGUG_EXPR_N_LITERAL,
	SGUG_EXPR_N_NAMED,
	SGUG_EXPR_N_CALL,
	SGUG_EXPR_N_OP,
	SGUG_EXPR_N_WILDCARD
} sgug_expr_ntype;

struct sgug_expr_node {
	sgug_expr_ntype type;
	sgug_expr_op op;
	sgug_expr_value lit;
	const char *name;		/* named value, or function name */
	struct sgug_expr_node *a;
	struct sgug_expr_node *b;
	struct sgug_expr_node *args;	/* chained through next */
	struct sgug_expr_node *next;
	size_t nargs;
	int col;
};

/*
 * Parser state, threaded through bison as %param.
 *
 * The lexer lives here rather than in flex because this language decides a
 * token's kind from the one before it: `(` after a name opens an argument
 * list, `*` is only legal after `[` or `.`, and a leading `.` is part of a
 * number only where a dereference cannot be.
 */
struct sgug_expr_parse {
	sgug_expr_arena *arena;
	const char *src;
	const char *p;
	const char *end;
	struct sgug_expr_node *root;
	int prev;			/* previous token, for the lexer */
	int failed;
	int col;			/* where the failure was */
	char err[256];
};

struct sgug_expr_ctx {
	sgug_expr_arena *arena;
	const sgug_job *job;
	sgug_json_doc *synth;		/* runner and secrets */
	const sgug_json *contexts;	/* test entry point only */
	const sgug_json *step_env;
	/* Rebuilt when the step changes, not on every reference. */
	const sgug_json *env;
	int job_failed;
	int job_cancelled;
	int depth;
	/* Documents fromJSON built. Values point into them, so they live until
	 * the context does; a fixed cap here freed one still in use. */
	sgug_json_doc **owned;
	size_t nowned;
	size_t owned_cap;
};

/*
 * Keeps a document alive for as long as the values pointing into it. Returns
 * -1 and frees doc if it cannot, since the caller is about to hand out
 * pointers into it.
 */
int sgug_expr_ctx_own(sgug_expr_ctx *ctx, sgug_json_doc *doc);
const sgug_json *sgug_expr_context_lookup(sgug_expr_ctx *ctx,
    const char *name);

/* All arena backed, so a failed parse is undone by dropping the arena. */
sgug_expr_arena *sgug_expr_arena_new(void);
void sgug_expr_arena_free(sgug_expr_arena *a);
void sgug_expr_arena_reset(sgug_expr_arena *a);
void *sgug_expr_arena_alloc(sgug_expr_arena *a, size_t n);
char *sgug_expr_arena_strdup(sgug_expr_arena *a, const char *s, size_t n);

struct sgug_expr_node *sgug_expr_binary(struct sgug_expr_parse *p,
    sgug_expr_op op, struct sgug_expr_node *a, struct sgug_expr_node *b);
struct sgug_expr_node *sgug_expr_unary(struct sgug_expr_parse *p,
    sgug_expr_op op, struct sgug_expr_node *a);
struct sgug_expr_node *sgug_expr_index_lit(struct sgug_expr_parse *p,
    struct sgug_expr_node *obj, const char *name);
struct sgug_expr_node *sgug_expr_wildcard(struct sgug_expr_parse *p,
    struct sgug_expr_node *obj);
struct sgug_expr_node *sgug_expr_call(struct sgug_expr_parse *p,
    const char *name, struct sgug_expr_node *args, int col);
struct sgug_expr_node *sgug_expr_named(struct sgug_expr_parse *p,
    const char *name, int col);
struct sgug_expr_node *sgug_expr_number(struct sgug_expr_parse *p, double n);
struct sgug_expr_node *sgug_expr_string(struct sgug_expr_parse *p,
    const char *s);
struct sgug_expr_node *sgug_expr_bool(struct sgug_expr_parse *p, int b);
struct sgug_expr_node *sgug_expr_null(struct sgug_expr_parse *p);
struct sgug_expr_node *sgug_expr_arg(struct sgug_expr_parse *p,
    struct sgug_expr_node *list, struct sgug_expr_node *item);

void sgug_expr_parse_error(struct sgug_expr_parse *p, int col,
    const char *msg);

int sgug_expr_check_arity(const char *name, size_t nargs, char *err,
    size_t errlen);

/* Hex and octal into a double. strtol is 32 bits under n32, which silently
 * clamps 0x1FFFFFFFF to 2147483647 on the target and not on the machine the
 * tests run on. */
double sgug_expr_radix(const char *s, int base, const char **end);

/* NaN, without asking libc for it. */
double sgug_expr_nan(void);

/* Parses src into an AST owned by the arena. Returns NULL with p->err set. */
struct sgug_expr_node *sgug_expr_parse(sgug_expr_arena *arena, const char *src,
    char *err, size_t errlen);

#endif /* SGUG_EXPR_PARSE_H */
