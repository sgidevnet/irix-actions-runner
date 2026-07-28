#include "expr/parse.h"
#include "expr/grammar.tab.h"

#include "compat/irix.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_CHUNK 4096

/*
 * strtod("nan") is a C99 addition. A libc that predates it returns 0 with the
 * end pointer unmoved, which would make NaN == NaN true and 'abc' == 0 true.
 * volatile so the division is not folded at compile time.
 */
double
sgug_expr_nan(void)
{
	static volatile double zero = 0.0;

	return zero / zero;
}

struct chunk {
	struct chunk *next;
	size_t used;
	size_t cap;
	char *base;
};

struct sgug_expr_arena {
	struct chunk *chunks;
};

sgug_expr_arena *
sgug_expr_arena_new(void)
{
	return calloc(1, sizeof(sgug_expr_arena));
}

void
sgug_expr_arena_reset(sgug_expr_arena *a)
{
	struct chunk *c, *next;

	for (c = a->chunks; c != NULL; c = next) {
		next = c->next;
		free(c->base);
		free(c);
	}
	a->chunks = NULL;
}

void
sgug_expr_arena_free(sgug_expr_arena *a)
{
	if (a == NULL)
		return;
	sgug_expr_arena_reset(a);
	free(a);
}

void *
sgug_expr_arena_alloc(sgug_expr_arena *a, size_t n)
{
	struct chunk *c = a->chunks;
	void *p;

	/* MIPS traps on unaligned access, so round every block up. */
	n = (n + 7) & ~(size_t)7;

	if (c == NULL || c->used + n > c->cap) {
		size_t cap = n > ARENA_CHUNK ? n : ARENA_CHUNK;

		c = calloc(1, sizeof(*c));
		if (c == NULL)
			return NULL;
		c->base = malloc(cap);
		if (c->base == NULL) {
			free(c);
			return NULL;
		}
		c->cap = cap;
		c->next = a->chunks;
		a->chunks = c;
	}

	p = c->base + c->used;
	c->used += n;
	memset(p, 0, n);
	return p;
}

char *
sgug_expr_arena_strdup(sgug_expr_arena *a, const char *s, size_t n)
{
	char *out = sgug_expr_arena_alloc(a, n + 1);

	if (out == NULL)
		return NULL;
	memcpy(out, s, n);
	out[n] = '\0';
	return out;
}

static struct sgug_expr_node *
node(struct sgug_expr_parse *p, sgug_expr_ntype t)
{
	struct sgug_expr_node *n = sgug_expr_arena_alloc(p->arena, sizeof(*n));

	if (n == NULL) {
		sgug_expr_parse_error(p, 0, "out of memory");
		return NULL;
	}
	n->type = t;
	return n;
}

struct sgug_expr_node *
sgug_expr_binary(struct sgug_expr_parse *p, sgug_expr_op op,
    struct sgug_expr_node *a, struct sgug_expr_node *b)
{
	struct sgug_expr_node *n = node(p, SGUG_EXPR_N_OP);

	if (n == NULL)
		return NULL;
	n->op = op;
	n->a = a;
	n->b = b;
	return n;
}

struct sgug_expr_node *
sgug_expr_unary(struct sgug_expr_parse *p, sgug_expr_op op,
    struct sgug_expr_node *a)
{
	struct sgug_expr_node *n = node(p, SGUG_EXPR_N_OP);

	if (n == NULL)
		return NULL;
	n->op = op;
	n->a = a;
	return n;
}

/* `a.b` is Index(a, 'b'): the reference compiles a dereference and a subscript
 * to the same node, which is why a missing key yields null rather than an
 * error in both spellings. */
struct sgug_expr_node *
sgug_expr_index_lit(struct sgug_expr_parse *p, struct sgug_expr_node *obj,
    const char *name)
{
	struct sgug_expr_node *key = node(p, SGUG_EXPR_N_LITERAL);

	if (key == NULL)
		return NULL;
	key->lit.kind = SGUG_EXPR_STRING;
	key->lit.s = name;
	return sgug_expr_binary(p, SGUG_EXPR_OP_INDEX, obj, key);
}

struct sgug_expr_node *
sgug_expr_wildcard(struct sgug_expr_parse *p, struct sgug_expr_node *obj)
{
	struct sgug_expr_node *n = node(p, SGUG_EXPR_N_WILDCARD);

	if (n == NULL)
		return NULL;
	n->a = obj;
	return n;
}

struct sgug_expr_node *
sgug_expr_call(struct sgug_expr_parse *p, const char *name,
    struct sgug_expr_node *args, int col)
{
	struct sgug_expr_node *n = node(p, SGUG_EXPR_N_CALL);
	struct sgug_expr_node *a;
	char msg[128];

	if (n == NULL)
		return NULL;
	n->name = name;
	n->args = args;
	n->col = col;
	for (a = args; a != NULL; a = a->next)
		n->nargs++;
	if (n->nargs > SGUG_EXPR_MAX_ARGS) {
		sgug_expr_parse_error(p, col, "too many parameters");
		return n;
	}

	msg[0] = '\0';
	if (sgug_expr_check_arity(name, n->nargs, msg, sizeof(msg)) != 0)
		sgug_expr_parse_error(p, col, msg);
	return n;
}

struct sgug_expr_node *
sgug_expr_named(struct sgug_expr_parse *p, const char *name, int col)
{
	struct sgug_expr_node *n = node(p, SGUG_EXPR_N_NAMED);

	if (n == NULL)
		return NULL;
	n->name = name;
	n->col = col;
	return n;
}

struct sgug_expr_node *
sgug_expr_number(struct sgug_expr_parse *p, double v)
{
	struct sgug_expr_node *n = node(p, SGUG_EXPR_N_LITERAL);

	if (n == NULL)
		return NULL;
	n->lit.kind = SGUG_EXPR_NUMBER;
	n->lit.n = v;
	return n;
}

struct sgug_expr_node *
sgug_expr_string(struct sgug_expr_parse *p, const char *s)
{
	struct sgug_expr_node *n = node(p, SGUG_EXPR_N_LITERAL);

	if (n == NULL)
		return NULL;
	n->lit.kind = SGUG_EXPR_STRING;
	n->lit.s = s;
	return n;
}

struct sgug_expr_node *
sgug_expr_bool(struct sgug_expr_parse *p, int b)
{
	struct sgug_expr_node *n = node(p, SGUG_EXPR_N_LITERAL);

	if (n == NULL)
		return NULL;
	n->lit.kind = SGUG_EXPR_BOOL;
	n->lit.b = b;
	return n;
}

struct sgug_expr_node *
sgug_expr_null(struct sgug_expr_parse *p)
{
	return node(p, SGUG_EXPR_N_LITERAL);
}

struct sgug_expr_node *
sgug_expr_arg(struct sgug_expr_parse *p, struct sgug_expr_node *list,
    struct sgug_expr_node *item)
{
	struct sgug_expr_node *a;

	(void)p;

	if (list == NULL)
		return item;
	for (a = list; a->next != NULL; a = a->next)
		;
	a->next = item;
	return list;
}

void
sgug_expr_parse_error(struct sgug_expr_parse *p, int col, const char *msg)
{
	if (p->failed)
		return;
	p->failed = 1;
	p->col = col;
	sgug_snprintf(p->err, sizeof(p->err), "%s", msg);
}

/* Identifiers may contain '-', which is the reason this language has no
 * arithmetic: `a-b` is one name, not a subtraction. */
static int
ident_start(int c)
{
	return isalpha((unsigned char)c) || c == '_';
}

static int
ident_char(int c)
{
	return isalnum((unsigned char)c) || c == '_' || c == '-';
}

/*
 * A number may begin with '.' or a sign, but only where a dereference or an
 * operand cannot already be in progress. Everywhere else a leading '.' is the
 * dereference operator.
 */
static int
number_can_start(int prev)
{
	switch (prev) {
	case NAME:
	case STRING:
	case NUMBER:
	case TRUE:
	case FALSE:
	case NUL:
	case ')':
	case ']':
	case STAR:
		return 0;
	default:
		return 1;
	}
}

double
sgug_expr_radix(const char *s, int base, const char **end)
{
	double v = 0;

	for (; *s != '\0'; s++) {
		int d;

		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (*s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			break;
		if (d >= base)
			break;
		v = v * base + d;
	}
	*end = s;
	return v;
}

static int
read_number(struct sgug_expr_parse *p, double *out)
{
	const char *s = p->p;
	const char *end = NULL;
	double v;

	if ((s[0] == '-' || s[0] == '+') &&
	    s[1] == '0' && (s[2] == 'x' || s[2] == 'X' || s[2] == 'o' ||
	    s[2] == 'O'))
		return -1;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		v = sgug_expr_radix(s + 2, 16, &end);
		if (end == s + 2)
			return -1;
	} else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
		v = sgug_expr_radix(s + 2, 8, &end);
		if (end == s + 2)
			return -1;
	} else if (strncmp(s, "Infinity", 8) == 0) {
		*out = HUGE_VAL;
		p->p = s + 8;
		return 0;
	} else if (strncmp(s, "-Infinity", 9) == 0) {
		*out = -HUGE_VAL;
		p->p = s + 9;
		return 0;
	} else if (strncmp(s, "+Infinity", 9) == 0) {
		*out = HUGE_VAL;
		p->p = s + 9;
		return 0;
	} else if (strncmp(s, "NaN", 3) == 0) {
		*out = sgug_expr_nan();
		p->p = s + 3;
		return 0;
	} else {
		/* strtod would take inf, INF and infinity; the language takes
		 * only the exact spelling handled above. */
		const char *t = s + (s[0] == '-' || s[0] == '+' ? 1 : 0);
		char *stop = NULL;

		if (!(*t >= '0' && *t <= '9') && *t != '.')
			return -1;
		v = strtod(s, &stop);
		end = stop;
	}

	if (end == NULL || end == s)
		return -1;

	/*
	 * The reference consumes to a token boundary and then rejects the whole
	 * run, so 0xFZ is an error rather than 0xF followed by a name.
	 */
	if (ident_char((unsigned char)*end) || *end == '.')
		return -1;

	*out = v;
	p->p = end;
	return 0;
}

int
sgug_expr_yylex(SGUG_EXPR_YYSTYPE *lval, SGUG_EXPR_YYLTYPE *loc,
    struct sgug_expr_parse *p)
{
	int tok;

	while (p->p < p->end && isspace((unsigned char)*p->p))
		p->p++;

	loc->first_column = (int)(p->p - p->src) + 1;
	loc->last_column = loc->first_column;

	if (p->p >= p->end)
		return 0;

	if (*p->p == '\'') {
		const char *s = ++p->p;
		char *out;
		size_t n = 0;

		/* '' is the only escape there is. */
		while (p->p < p->end) {
			if (*p->p == '\'') {
				if (p->p + 1 < p->end && p->p[1] == '\'') {
					p->p += 2;
					n++;
					continue;
				}
				break;
			}
			p->p++;
			n++;
		}
		if (p->p >= p->end) {
			sgug_expr_parse_error(p, loc->first_column,
			    "unterminated string");
			return 0;
		}

		out = sgug_expr_arena_alloc(p->arena, n + 1);
		if (out != NULL) {
			const char *q = s;
			size_t i = 0;

			while (q < p->p) {
				if (*q == '\'' && q + 1 < p->p && q[1] == '\'')
					q++;
				out[i++] = *q++;
			}
			out[i] = '\0';
		}
		p->p++;
		lval->str = out;
		p->prev = STRING;
		return STRING;
	}

	if (ident_start((unsigned char)*p->p) &&
	    (p->prev == '.' ||
	    (strncmp(p->p, "NaN", 3) != 0 &&
	    strncmp(p->p, "Infinity", 8) != 0))) {
		const char *s = p->p;

		while (p->p < p->end && ident_char((unsigned char)*p->p))
			p->p++;

		{
			size_t n = (size_t)(p->p - s);

			if (p->prev == '.') {
				lval->str = sgug_expr_arena_strdup(p->arena, s,
				    n);
				if (lval->str == NULL)
					return 0;
				tok = NAME;
			} else if (n == 4 && strncmp(s, "true", 4) == 0)
				tok = TRUE;
			else if (n == 5 && strncmp(s, "false", 5) == 0)
				tok = FALSE;
			else if (n == 4 && strncmp(s, "null", 4) == 0)
				tok = NUL;
			else {
				lval->str = sgug_expr_arena_strdup(p->arena, s,
				    n);
				if (lval->str == NULL)
					return 0;
				tok = NAME;
			}
		}
		p->prev = tok;
		return tok;
	}

	if (isdigit((unsigned char)*p->p) ||
	    ((*p->p == '.' || *p->p == '-' || *p->p == '+' ||
	    *p->p == 'N' || *p->p == 'I') && number_can_start(p->prev))) {
		double v;

		if (read_number(p, &v) != 0) {
			sgug_expr_parse_error(p, loc->first_column,
			    "unrecognized number");
			return 0;
		}
		lval->num = v;
		p->prev = NUMBER;
		return NUMBER;
	}

	if (p->p + 1 < p->end) {
		if (p->p[0] == '&' && p->p[1] == '&') { p->p += 2; tok = AND; goto two; }
		if (p->p[0] == '|' && p->p[1] == '|') { p->p += 2; tok = OR; goto two; }
		if (p->p[0] == '=' && p->p[1] == '=') { p->p += 2; tok = EQ; goto two; }
		if (p->p[0] == '!' && p->p[1] == '=') { p->p += 2; tok = NE; goto two; }
		if (p->p[0] == '<' && p->p[1] == '=') { p->p += 2; tok = LE; goto two; }
		if (p->p[0] == '>' && p->p[1] == '=') { p->p += 2; tok = GE; goto two; }
	}

	/* '*' is a wildcard, and legal only straight after '[' or '.'. */
	if (*p->p == '*') {
		if (p->prev != '[' && p->prev != '.') {
			sgug_expr_parse_error(p, loc->first_column,
			    "unexpected symbol: *");
			return 0;
		}
		p->p++;
		p->prev = STAR;
		return STAR;
	}

	tok = (unsigned char)*p->p++;
	p->prev = tok;
	return tok;

two:
	p->prev = tok;
	return tok;
}

/* Iterative: the tree can be deeper than the stack tolerates, which is the
 * whole reason the parser is not recursive either. */
static int
depth_of(const struct sgug_expr_node *root)
{
	/* Wider than the depth limit: a flattened && or || chain stays one
	 * level deep but still queues an operand per link. */
	const struct sgug_expr_node *stack[1024];
	int depths[1024];
	int top = 0, max = 0;

	stack[top] = root;
	depths[top] = 1;
	top++;

	while (top > 0) {
		const struct sgug_expr_node *n;
		int d;

		top--;
		n = stack[top];
		d = depths[top];
		if (n == NULL)
			continue;
		if (d > max)
			max = d;
		if (d > SGUG_EXPR_MAX_DEPTH)
			return d;

		{
			const struct sgug_expr_node *kids[3];
			const struct sgug_expr_node *arg;
			size_t i;

			kids[0] = n->a;
			kids[1] = n->b;
			kids[2] = NULL;
			for (i = 0; i < 2; i++) {
				int kd = d + 1;

				if (kids[i] == NULL)
					continue;
				if (top >= (int)(sizeof(stack) / sizeof(stack[0])))
					return SGUG_EXPR_MAX_DEPTH + 1;
				/* The reference merges a chain of && or || into
				 * one n-ary node, so `a || b || c` is one level
				 * however long the chain runs. */
				if (n->type == SGUG_EXPR_N_OP &&
				    (n->op == SGUG_EXPR_OP_AND ||
				    n->op == SGUG_EXPR_OP_OR) &&
				    kids[i]->type == SGUG_EXPR_N_OP &&
				    kids[i]->op == n->op)
					kd = d;
				stack[top] = kids[i];
				depths[top] = kd;
				top++;
			}
			for (arg = n->args; arg != NULL; arg = arg->next) {
				if (top >= (int)(sizeof(stack) / sizeof(stack[0])))
					return SGUG_EXPR_MAX_DEPTH + 1;
				stack[top] = arg;
				depths[top] = d + 1;
				top++;
			}
		}
	}
	return max;
}

struct sgug_expr_node *
sgug_expr_parse(sgug_expr_arena *arena, const char *src, char *err,
    size_t errlen)
{
	struct sgug_expr_parse p;
	size_t len = strlen(src);

	memset(&p, 0, sizeof(p));
	p.arena = arena;
	p.src = src;
	p.p = src;
	p.end = src + len;

	if (len == 0) {
		struct sgug_expr_node *n = sgug_expr_arena_alloc(arena,
		    sizeof(*n));

		if (n != NULL)
			n->type = SGUG_EXPR_N_LITERAL;
		return n;
	}

	if (len > SGUG_EXPR_MAX_LENGTH) {
		sgug_snprintf(err, errlen, "expression exceeds %d characters",
		    SGUG_EXPR_MAX_LENGTH);
		return NULL;
	}

	if (sgug_expr_yyparse(&p) == 0 && !p.failed && p.root != NULL &&
	    depth_of(p.root) > SGUG_EXPR_MAX_DEPTH)
		sgug_expr_parse_error(&p, 1, "expression is nested too deeply");

	if (p.failed || p.root == NULL) {
		sgug_snprintf(err, errlen, "%s at position %d in: %.80s",
		    p.err[0] != '\0' ? p.err : "syntax error", p.col, src);
		return NULL;
	}
	return p.root;
}
