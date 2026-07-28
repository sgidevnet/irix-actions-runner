#include "expr/parse.h"

#include "compat/irix.h"
#include "exec/token.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Evaluation, including the coercion table. The comparison rules are
 * JavaScript's abstract equality with two changes: strings compare
 * case-insensitively, and objects are never coerced to primitives.
 */

const char *
sgug_expr_kind_name(sgug_expr_kind k)
{
	switch (k) {
	case SGUG_EXPR_NULL: return "Null";
	case SGUG_EXPR_BOOL: return "Boolean";
	case SGUG_EXPR_NUMBER: return "Number";
	case SGUG_EXPR_STRING: return "String";
	case SGUG_EXPR_OBJECT: return "Object";
	default: return "Array";
	}
}

/*
 * One accessor for every encoding a container arrives in: a plain JSON object,
 * the compact PipelineContextData dictionary with `d`, and the verbose
 * TemplateToken mapping with `map` and Key/Value. contextData uses the second,
 * environmentVariables the third, and fromJSON the first.
 *
 * The type tag has to be there, because these also run over user data.
 * fromJSON('{"d":[1,2,3]}') is an object with a key called d, not a dictionary
 * of three unnamed entries, and unwrapping it silently produced
 * {"": null,"": null,"": null}.
 */
static int
tagged(const sgug_json *o)
{
	return sgug_json_get(o, "t") != NULL ||
	    sgug_json_get(o, "type") != NULL;
}

static const sgug_json *
entries_of(const sgug_json *o, int *verbose)
{
	const sgug_json *e;

	*verbose = 0;
	if (o == NULL || !tagged(o))
		return NULL;

	e = sgug_json_get(o, "d");
	if (sgug_json_type_of(e) == SGUG_JSON_ARRAY)
		return e;

	e = sgug_json_get(o, "map");
	if (sgug_json_type_of(e) == SGUG_JSON_ARRAY) {
		*verbose = 1;
		return e;
	}
	return NULL;
}

static const sgug_json *
scalar_of(const sgug_json *v)
{
	const sgug_json *inner;

	if (v == NULL)
		return NULL;
	if (sgug_json_type_of(v) != SGUG_JSON_OBJECT || !tagged(v))
		return v;

	/* A TemplateToken wraps its payload; PipelineContextData does not. */
	inner = sgug_json_get(v, "lit");
	if (inner == NULL)
		inner = sgug_json_get(v, "s");
	return inner != NULL ? inner : v;
}

/*
 * OrdinalIgnoreCase folds to upper, not lower, and a byte above 0x7f must not
 * sign extend: as a signed char it sorts below every ASCII byte, which had
 * 'i' < 'İ' coming out false.
 */
static int
fold(unsigned char c)
{
	return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static int
key_eq(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		int ca = fold((unsigned char)*a);
		int cb = fold((unsigned char)*b);

		if (ca != cb)
			return 0;
		a++;
		b++;
	}
	return *a == *b;
}

/* Key lookup is case insensitive, which is why github.SHA resolves. */
static const sgug_json *
obj_get(const sgug_json *o, const char *key)
{
	const sgug_json *entries;
	size_t i, n;
	int verbose;

	entries = entries_of(o, &verbose);
	if (entries == NULL)
		return scalar_of(sgug_json_get(o, key));

	n = sgug_json_len(entries);
	for (i = 0; i < n; i++) {
		const sgug_json *e = sgug_json_at(entries, i);
		const sgug_json *k = sgug_json_get(e, verbose ? "Key" : "k");
		const char *ks = sgug_json_string(scalar_of(k), NULL);

		if (ks != NULL && key_eq(ks, key))
			return scalar_of(sgug_json_get(e,
			    verbose ? "Value" : "v"));
	}
	return NULL;
}

static size_t
obj_len(const sgug_json *o)
{
	const sgug_json *entries;
	int verbose;

	entries = entries_of(o, &verbose);
	if (entries != NULL)
		return sgug_json_len(entries);
	return sgug_json_type_of(o) == SGUG_JSON_OBJECT ? sgug_json_len(o) : 0;
}

static const sgug_json *
obj_at(const sgug_json *o, size_t i, const char **key)
{
	const sgug_json *entries;
	int verbose;

	entries = entries_of(o, &verbose);
	if (entries != NULL) {
		const sgug_json *e = sgug_json_at(entries, i);

		if (key != NULL)
			*key = sgug_json_string(scalar_of(sgug_json_get(e,
			    verbose ? "Key" : "k")), "");
		return scalar_of(sgug_json_get(e, verbose ? "Value" : "v"));
	}

	{
		const sgug_json *val = NULL;

		sgug_json_member(o, i, key, &val);
		return val;
	}
}

static sgug_expr_value
from_json(const sgug_json *v)
{
	sgug_expr_value out;
	int verbose;

	memset(&out, 0, sizeof(out));
	out.node = v;

	if (v == NULL) {
		out.kind = SGUG_EXPR_NULL;
		return out;
	}

	if (entries_of(v, &verbose) != NULL) {
		out.kind = SGUG_EXPR_OBJECT;
		return out;
	}

	switch (sgug_json_type_of(v)) {
	case SGUG_JSON_BOOL:
		out.kind = SGUG_EXPR_BOOL;
		out.b = sgug_json_bool(v, 0);
		break;
	case SGUG_JSON_NUMBER:
		out.kind = SGUG_EXPR_NUMBER;
		out.n = sgug_json_double(v, 0);
		break;
	case SGUG_JSON_STRING:
		out.kind = SGUG_EXPR_STRING;
		out.s = sgug_json_string(v, "");
		break;
	case SGUG_JSON_OBJECT:
		out.kind = SGUG_EXPR_OBJECT;
		break;
	case SGUG_JSON_ARRAY:
		out.kind = SGUG_EXPR_ARRAY;
		break;
	default:
		out.kind = SGUG_EXPR_NULL;
		break;
	}
	return out;
}

static sgug_expr_value
mkstr(const char *s)
{
	sgug_expr_value v;

	memset(&v, 0, sizeof(v));
	v.kind = SGUG_EXPR_STRING;
	v.s = s != NULL ? s : "";
	return v;
}

static sgug_expr_value
mknum(double n)
{
	sgug_expr_value v;

	memset(&v, 0, sizeof(v));
	v.kind = SGUG_EXPR_NUMBER;
	v.n = n;
	return v;
}

static sgug_expr_value
mkbool(int b)
{
	sgug_expr_value v;

	memset(&v, 0, sizeof(v));
	v.kind = SGUG_EXPR_BOOL;
	v.b = b != 0;
	return v;
}

static sgug_expr_value
mknull(void)
{
	sgug_expr_value v;

	memset(&v, 0, sizeof(v));
	return v;
}

/* G15 invariant, matching EvaluationResult.ConvertToString, and -0 prints as
 * 0 the way .NET Core 3.0 and later do. */
static const char *
num_to_string(sgug_expr_ctx *ctx, double n)
{
	char buf[64];
	char *p;

	if (n == 0)
		n = 0;
	if (n != n)
		return "NaN";
	if (n == HUGE_VAL)
		return "Infinity";
	if (n == -HUGE_VAL)
		return "-Infinity";

	sgug_snprintf(buf, sizeof(buf), "%.15g", n);
	/* G15 is an uppercase specifier. */
	for (p = buf; *p != '\0'; p++) {
		if (*p == 'e')
			*p = 'E';
	}
	return sgug_expr_arena_strdup(ctx->arena, buf, strlen(buf));
}

static const char *
to_string(sgug_expr_ctx *ctx, const sgug_expr_value *v)
{
	switch (v->kind) {
	case SGUG_EXPR_NULL: return "";
	case SGUG_EXPR_BOOL: return v->b ? "true" : "false";
	case SGUG_EXPR_NUMBER: return num_to_string(ctx, v->n);
	case SGUG_EXPR_STRING: return v->s != NULL ? v->s : "";
	case SGUG_EXPR_OBJECT: return "Object";
	default: return "Array";
	}
}

/* An empty or blank string is 0; anything unparseable is NaN. */
static double
str_to_number(const char *s)
{
	const char *end = NULL;
	double v;

	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	if (*s == '\0')
		return 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		v = sgug_expr_radix(s + 2, 16, &end);
	} else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
		v = sgug_expr_radix(s + 2, 8, &end);
	} else if (strncmp(s, "Infinity", 8) == 0) {
		v = HUGE_VAL;
		end = s + 8;
	} else if (strncmp(s, "-Infinity", 9) == 0) {
		v = -HUGE_VAL;
		end = s + 9;
	} else if (strncmp(s, "NaN", 3) == 0) {
		v = sgug_expr_nan();
		end = s + 3;
	} else {
		/* strtod also takes inf, INF and infinity; the language takes
		 * only the spellings handled above. */
		const char *t = s + (s[0] == '-' || s[0] == '+' ? 1 : 0);
		char *stop = NULL;

		if (!(*t >= '0' && *t <= '9') && *t != '.')
			return sgug_expr_nan();
		v = strtod(s, &stop);
		end = stop;
	}

	if (end == NULL || end == s)
		return sgug_expr_nan();
	while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
		end++;
	return *end == '\0' ? v : sgug_expr_nan();
}

static double
to_number(const sgug_expr_value *v)
{
	switch (v->kind) {
	case SGUG_EXPR_NULL: return 0;
	case SGUG_EXPR_BOOL: return v->b ? 1 : 0;
	case SGUG_EXPR_NUMBER: return v->n;
	case SGUG_EXPR_STRING: return str_to_number(v->s != NULL ? v->s : "");
	default: return sgug_expr_nan();
	}
}

/* Every object and array is truthy, even an empty one. */
static int
is_falsy(const sgug_expr_value *v)
{
	switch (v->kind) {
	case SGUG_EXPR_NULL: return 1;
	case SGUG_EXPR_BOOL: return !v->b;
	case SGUG_EXPR_NUMBER: return v->n == 0 || v->n != v->n;
	case SGUG_EXPR_STRING: return v->s == NULL || v->s[0] == '\0';
	default: return 0;
	}
}

static int
casecmp(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		int ca = fold((unsigned char)*a);
		int cb = fold((unsigned char)*b);

		if (ca != cb)
			return ca < cb ? -1 : 1;
		a++;
		b++;
	}
	if (*a == *b)
		return 0;
	return *a == '\0' ? -1 : 1;
}

static int
casecmp_n(const char *a, const char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		int ca = fold((unsigned char)a[i]);
		int cb = fold((unsigned char)b[i]);

		if (ca != cb)
			return ca < cb ? -1 : 1;
		if (a[i] == '\0')
			return 0;
	}
	return 0;
}

static int
is_primitive(const sgug_expr_value *v)
{
	return v->kind != SGUG_EXPR_OBJECT && v->kind != SGUG_EXPR_ARRAY;
}

/*
 * CoerceTypes: a string meeting a number becomes a number, and a boolean or
 * null meeting anything becomes a number and tries again. Kinds that still
 * differ afterwards never compare equal.
 */
static int
coerce(sgug_expr_value *l, sgug_expr_value *r)
{
	int i;

	for (i = 0; i < 4; i++) {
		if (l->kind == r->kind)
			return 1;
		if (l->kind == SGUG_EXPR_NUMBER && r->kind == SGUG_EXPR_STRING)
			*r = mknum(to_number(r));
		else if (l->kind == SGUG_EXPR_STRING &&
		    r->kind == SGUG_EXPR_NUMBER)
			*l = mknum(to_number(l));
		else if (l->kind == SGUG_EXPR_BOOL || l->kind == SGUG_EXPR_NULL)
			*l = mknum(to_number(l));
		else if (r->kind == SGUG_EXPR_BOOL || r->kind == SGUG_EXPR_NULL)
			*r = mknum(to_number(r));
		else
			return 0;
	}
	return l->kind == r->kind;
}

static int
abstract_equal(sgug_expr_value a, sgug_expr_value b)
{
	if (!coerce(&a, &b))
		return 0;

	switch (a.kind) {
	case SGUG_EXPR_NULL:
		return 1;
	case SGUG_EXPR_NUMBER:
		if (a.n != a.n || b.n != b.n)
			return 0;
		return a.n == b.n;
	case SGUG_EXPR_STRING:
		return casecmp(a.s != NULL ? a.s : "",
		    b.s != NULL ? b.s : "") == 0;
	case SGUG_EXPR_BOOL:
		return a.b == b.b;
	default:
		/* Reference equality: two identical objects are not equal. */
		return a.node != NULL && a.node == b.node;
	}
}

static int
abstract_greater(sgug_expr_value a, sgug_expr_value b)
{
	if (!coerce(&a, &b))
		return 0;

	switch (a.kind) {
	case SGUG_EXPR_NUMBER:
		if (a.n != a.n || b.n != b.n)
			return 0;
		return a.n > b.n;
	case SGUG_EXPR_STRING:
		return casecmp(a.s != NULL ? a.s : "",
		    b.s != NULL ? b.s : "") > 0;
	case SGUG_EXPR_BOOL:
		return a.b && !b.b;
	default:
		return 0;
	}
}

static int eval_node(sgug_expr_ctx *ctx, const struct sgug_expr_node *n,
    sgug_expr_value *out, char *err, size_t errlen);

static void
seterr(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (err == NULL || errlen == 0)
		return;

	va_start(ap, fmt);
	sgug_vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

static char *
grow(char *buf, size_t *cap, size_t need)
{
	char *out;

	if (need <= *cap)
		return buf;
	while (*cap < need)
		*cap = *cap != 0 ? *cap * 2 : 128;

	/* Callers return -1 on NULL without freeing, so drop it here. */
	out = realloc(buf, *cap);
	if (out == NULL)
		free(buf);
	return out;
}

/*
 * format('...', ...). {N} substitutes, {{ and }} are literal braces, and a
 * non-empty format specifier is an error rather than being ignored.
 */
static int
fn_format(sgug_expr_ctx *ctx, const sgug_expr_value *args, size_t nargs,
    sgug_expr_value *out, char *err, size_t errlen)
{
	const char *f = to_string(ctx, &args[0]);
	char *buf = NULL;
	size_t cap = 0, used = 0;

	while (*f != '\0') {
		if (f[0] == '{' && f[1] == '{') {
			buf = grow(buf, &cap, used + 2);
			if (buf == NULL)
				return -1;
			buf[used++] = '{';
			f += 2;
			continue;
		}
		if (f[0] == '}' && f[1] == '}') {
			buf = grow(buf, &cap, used + 2);
			if (buf == NULL)
				return -1;
			buf[used++] = '}';
			f += 2;
			continue;
		}
		if (f[0] == '{') {
			size_t idx = 0;
			const char *s;
			size_t n;

			f++;
			if (*f < '0' || *f > '9') {
				free(buf);
				seterr(err, errlen, "invalid format string");
				return -1;
			}
			while (*f >= '0' && *f <= '9') {
				if (idx > SGUG_EXPR_MAX_ARGS) {
					free(buf);
					seterr(err, errlen,
					    "format argument index out of "
					    "range");
					return -1;
				}
				idx = idx * 10 + (size_t)(*f++ - '0');
			}
			if (*f != '}') {
				free(buf);
				seterr(err, errlen,
				    "format specifiers are not supported");
				return -1;
			}
			f++;
			if (idx + 1 >= nargs) {
				free(buf);
				seterr(err, errlen,
				    "format argument index out of range");
				return -1;
			}
			s = to_string(ctx, &args[idx + 1]);
			n = strlen(s);
			buf = grow(buf, &cap, used + n + 1);
			if (buf == NULL)
				return -1;
			memcpy(buf + used, s, n);
			used += n;
			continue;
		}
		if (f[0] == '}') {
			free(buf);
			seterr(err, errlen, "invalid format string");
			return -1;
		}
		buf = grow(buf, &cap, used + 2);
		if (buf == NULL)
			return -1;
		buf[used++] = *f++;
	}

	buf = grow(buf, &cap, used + 1);
	if (buf == NULL)
		return -1;
	buf[used] = '\0';
	*out = mkstr(sgug_expr_arena_strdup(ctx->arena, buf, used));
	free(buf);
	return 0;
}

struct outbuf {
	char *p;
	size_t used;
	size_t cap;
	int failed;
};

static void
out_raw(struct outbuf *b, const char *s, size_t n)
{
	if (b->failed)
		return;
	if (b->used + n + 1 > b->cap) {
		size_t cap = b->cap != 0 ? b->cap : 256;
		char *grown;

		while (cap < b->used + n + 1)
			cap *= 2;
		grown = realloc(b->p, cap);
		if (grown == NULL) {
			b->failed = 1;
			return;
		}
		b->p = grown;
		b->cap = cap;
	}
	memcpy(b->p + b->used, s, n);
	b->used += n;
	b->p[b->used] = '\0';
}

static void
out_str(struct outbuf *b, const char *s)
{
	out_raw(b, s, strlen(s));
}

static void
out_indent(struct outbuf *b, int depth)
{
	int i;

	out_str(b, "\n");
	for (i = 0; i < depth; i++)
		out_str(b, "  ");
}

static void
out_quoted(struct outbuf *b, const char *s)
{
	out_str(b, "\"");
	for (; *s != '\0'; s++) {
		switch (*s) {
		case '"': out_str(b, "\\\""); break;
		case '\\': out_str(b, "\\\\"); break;
		case '\n': out_str(b, "\\n"); break;
		case '\r': out_str(b, "\\r"); break;
		case '\t': out_str(b, "\\t"); break;
		default:
			if ((unsigned char)*s < 0x20) {
				char esc[8];

				sgug_snprintf(esc, sizeof(esc), "\\u%04x",
				    (unsigned)(unsigned char)*s);
				out_str(b, esc);
			} else {
				out_raw(b, s, 1);
			}
			break;
		}
	}
	out_str(b, "\"");
}

static void json_emit(sgug_expr_ctx *ctx, struct outbuf *b,
    const sgug_expr_value *v, int depth);

static void
json_emit_node(sgug_expr_ctx *ctx, struct outbuf *b, const sgug_json *node,
    int depth)
{
	sgug_expr_value v = from_json(node);

	json_emit(ctx, b, &v, depth);
}

static void
json_emit(sgug_expr_ctx *ctx, struct outbuf *b, const sgug_expr_value *v,
    int depth)
{
	size_t i, n;

	switch (v->kind) {
	case SGUG_EXPR_NULL:
		out_str(b, "null");
		return;
	case SGUG_EXPR_BOOL:
		out_str(b, v->b ? "true" : "false");
		return;
	case SGUG_EXPR_NUMBER:
		out_str(b, num_to_string(ctx, v->n));
		return;
	case SGUG_EXPR_STRING:
		out_quoted(b, v->s != NULL ? v->s : "");
		return;
	case SGUG_EXPR_ARRAY:
		n = v->arr != NULL ? v->arr->n : sgug_json_len(v->node);
		if (n == 0) {
			out_str(b, "[]");
			return;
		}
		out_str(b, "[");
		for (i = 0; i < n; i++) {
			if (i > 0)
				out_str(b, ",");
			out_indent(b, depth + 1);
			if (v->arr != NULL)
				json_emit(ctx, b, &v->arr->items[i], depth + 1);
			else
				json_emit_node(ctx, b,
				    sgug_json_at(v->node, i), depth + 1);
		}
		out_indent(b, depth);
		out_str(b, "]");
		return;
	default:
		n = obj_len(v->node);
		if (n == 0) {
			out_str(b, "{}");
			return;
		}
		out_str(b, "{");
		for (i = 0; i < n; i++) {
			const char *k = NULL;
			const sgug_json *val = obj_at(v->node, i, &k);

			if (i > 0)
				out_str(b, ",");
			out_indent(b, depth + 1);
			out_quoted(b, k != NULL ? k : "");
			out_str(b, ": ");
			json_emit_node(ctx, b, val, depth + 1);
		}
		out_indent(b, depth);
		out_str(b, "}");
		return;
	}
}

static void
json_append(sgug_expr_ctx *ctx, sgug_jsonw *w, const sgug_expr_value *v);

static void
json_append_node(sgug_expr_ctx *ctx, sgug_jsonw *w, const sgug_json *node)
{
	sgug_expr_value v = from_json(node);

	json_append(ctx, w, &v);
}

static void
json_append(sgug_expr_ctx *ctx, sgug_jsonw *w, const sgug_expr_value *v)
{
	size_t i, n;

	switch (v->kind) {
	case SGUG_EXPR_NULL:
		sgug_jsonw_null(w);
		return;
	case SGUG_EXPR_BOOL:
		sgug_jsonw_bool(w, v->b);
		return;
	case SGUG_EXPR_NUMBER:
		sgug_jsonw_raw(w, num_to_string(ctx, v->n));
		return;
	case SGUG_EXPR_STRING:
		sgug_jsonw_str(w, v->s);
		return;
	case SGUG_EXPR_ARRAY:
		sgug_jsonw_arr_begin(w);
		if (v->arr != NULL) {
			for (i = 0; i < v->arr->n; i++)
				json_append(ctx, w, &v->arr->items[i]);
		} else {
			n = sgug_json_len(v->node);
			for (i = 0; i < n; i++)
				json_append_node(ctx, w,
				    sgug_json_at(v->node, i));
		}
		sgug_jsonw_arr_end(w);
		return;
	default:
		sgug_jsonw_obj_begin(w);
		n = obj_len(v->node);
		for (i = 0; i < n; i++) {
			const char *k = NULL;
			const sgug_json *val = obj_at(v->node, i, &k);

			sgug_jsonw_key(w, k != NULL ? k : "");
			json_append_node(ctx, w, val);
		}
		sgug_jsonw_obj_end(w);
		return;
	}
}

static const struct {
	const char *name;
	size_t min;
	size_t max;
} ARITY[] = {
	{ "contains", 2, 2 }, { "startsWith", 2, 2 }, { "endsWith", 2, 2 },
	{ "format", 1, SGUG_EXPR_MAX_ARGS }, { "join", 1, 2 },
	{ "toJSON", 1, 1 }, { "fromJSON", 1, 1 },
	{ "case", 3, SGUG_EXPR_MAX_ARGS }, { "hashFiles", 1, SGUG_EXPR_MAX_ARGS },
	{ "success", 0, 0 }, { "always", 0, 0 }, { "cancelled", 0, 0 },
	{ "failure", 0, 0 }
};

/*
 * Arity is a parse error, not an evaluation error, which is what the corpus
 * pins and it matters: `always() || join()` must be rejected, and it never
 * reaches the evaluator because || short circuits.
 */
int
sgug_expr_check_arity(const char *name, size_t nargs, char *err, size_t errlen)
{
	size_t i;

	for (i = 0; i < sizeof(ARITY) / sizeof(ARITY[0]); i++) {
		if (!key_eq(name, ARITY[i].name))
			continue;
		if (nargs < ARITY[i].min) {
			seterr(err, errlen, "too few parameters for %s", name);
			return -1;
		}
		if (nargs > ARITY[i].max) {
			seterr(err, errlen, "too many parameters for %s", name);
			return -1;
		}
		if (key_eq(name, "case") && nargs % 2 == 0) {
			seterr(err, errlen, "case needs an odd count of "
			    "parameters");
			return -1;
		}
		return 0;
	}
	seterr(err, errlen, "unrecognized function: %s", name);
	return -1;
}

static int
call_builtin(sgug_expr_ctx *ctx, const char *name, const sgug_expr_value *args,
    size_t nargs, sgug_expr_value *out, char *err, size_t errlen)
{
	if (key_eq(name, "format"))
		return fn_format(ctx, args, nargs, out, err, errlen);

	if (key_eq(name, "contains")) {
		if (args[0].kind == SGUG_EXPR_ARRAY) {
			size_t i, n = args[0].arr != NULL ? args[0].arr->n
			    : sgug_json_len(args[0].node);

			for (i = 0; i < n; i++) {
				sgug_expr_value it = args[0].arr != NULL
				    ? args[0].arr->items[i]
				    : from_json(sgug_json_at(args[0].node, i));

				if (abstract_equal(it, args[1])) {
					*out = mkbool(1);
					return 0;
				}
			}
			*out = mkbool(0);
			return 0;
		}
		if (!is_primitive(&args[0]) || !is_primitive(&args[1])) {
			*out = mkbool(0);
			return 0;
		}
		{
			const char *h = to_string(ctx, &args[0]);
			const char *nd = to_string(ctx, &args[1]);
			size_t hl = strlen(h), nl = strlen(nd), i;

			for (i = 0; nl <= hl && i + nl <= hl; i++) {
				if (casecmp_n(h + i, nd, nl) == 0) {
					*out = mkbool(1);
					return 0;
				}
			}
			*out = mkbool(nl == 0);
			return 0;
		}
	}

	if (key_eq(name, "startsWith") || key_eq(name, "endsWith")) {
		const char *h, *nd;
		size_t hl, nl;

		if (!is_primitive(&args[0]) || !is_primitive(&args[1])) {
			*out = mkbool(0);
			return 0;
		}
		h = to_string(ctx, &args[0]);
		nd = to_string(ctx, &args[1]);
		hl = strlen(h);
		nl = strlen(nd);
		if (nl > hl) {
			*out = mkbool(0);
			return 0;
		}
		*out = mkbool(key_eq(name, "startsWith")
		    ? casecmp_n(h, nd, nl) == 0
		    : casecmp_n(h + hl - nl, nd, nl) == 0);
		return 0;
	}

	if (key_eq(name, "join")) {
		const char *sep = nargs > 1 && is_primitive(&args[1])
		    ? to_string(ctx, &args[1]) : ",";
		char *buf = NULL;
		size_t cap = 0, used = 0, i, n;

		if (args[0].kind != SGUG_EXPR_ARRAY) {
			*out = mkstr(is_primitive(&args[0])
			    ? to_string(ctx, &args[0]) : "");
			return 0;
		}
		n = args[0].arr != NULL ? args[0].arr->n
		    : sgug_json_len(args[0].node);
		for (i = 0; i < n; i++) {
			sgug_expr_value it = args[0].arr != NULL
			    ? args[0].arr->items[i]
			    : from_json(sgug_json_at(args[0].node, i));
			const char *s = to_string(ctx, &it);
			size_t sl = strlen(s), pl = i > 0 ? strlen(sep) : 0;

			buf = grow(buf, &cap, used + sl + pl + 1);
			if (buf == NULL)
				return -1;
			if (i > 0) {
				memcpy(buf + used, sep, pl);
				used += pl;
			}
			memcpy(buf + used, s, sl);
			used += sl;
		}
		buf = grow(buf, &cap, used + 1);
		if (buf == NULL)
			return -1;
		buf[used] = '\0';
		*out = mkstr(sgug_expr_arena_strdup(ctx->arena, buf, used));
		free(buf);
		return 0;
	}

	if (key_eq(name, "toJSON") || key_eq(name, "toJson")) {
		struct outbuf b;

		memset(&b, 0, sizeof(b));
		json_emit(ctx, &b, &args[0], 0);
		if (b.failed) {
			free(b.p);
			return -1;
		}
		*out = mkstr(b.p != NULL
		    ? sgug_expr_arena_strdup(ctx->arena, b.p, b.used) : "");
		free(b.p);
		return 0;
	}

	if (key_eq(name, "fromJSON") || key_eq(name, "fromJson")) {
		const char *text = to_string(ctx, &args[0]);
		sgug_json_doc *doc = sgug_json_parse(text, strlen(text), NULL,
		    0);

		if (doc == NULL) {
			seterr(err, errlen, "fromJSON could not parse %.60s",
			    text);
			return -1;
		}
		/* The document has to outlive the value that points into it. */
		if (sgug_expr_ctx_own(ctx, doc) != 0) {
			seterr(err, errlen, "out of memory");
			return -1;
		}
		*out = from_json(sgug_json_root(doc));
		return 0;
	}

	if (key_eq(name, "success")) {
		*out = mkbool(!ctx->job_failed &&
		    !ctx->job_cancelled);
		return 0;
	}
	if (key_eq(name, "always")) {
		*out = mkbool(1);
		return 0;
	}
	if (key_eq(name, "cancelled")) {
		*out = mkbool(ctx->job_cancelled);
		return 0;
	}
	if (key_eq(name, "failure")) {
		*out = mkbool(ctx->job_failed &&
		    !ctx->job_cancelled);
		return 0;
	}

	if (key_eq(name, "case")) {
		size_t i;

		for (i = 0; i + 1 < nargs; i += 2) {
			if (args[i].kind != SGUG_EXPR_BOOL) {
				seterr(err, errlen,
				    "case predicates must be boolean");
				return -1;
			}
			if (args[i].b) {
				*out = args[i + 1];
				return 0;
			}
		}
		*out = args[nargs - 1];
		return 0;
	}

	if (key_eq(name, "hashFiles")) {
		seterr(err, errlen, "hashFiles is not implemented on this "
		    "runner");
		return -1;
	}

	seterr(err, errlen, "unrecognized function: %s", name);
	return -1;
}

/*
 * Indexing a filtered array takes one step down into every element and drops
 * the ones that cannot follow it, so obj.*.x yields only the x values that
 * exist rather than a null per missing key.
 */
static int
flatten(sgug_expr_ctx *ctx, const sgug_expr_value *obj,
    const sgug_expr_value *key, sgug_expr_value *out)
{
	sgug_expr_array *arr = sgug_expr_arena_alloc(ctx->arena, sizeof(*arr));
	sgug_expr_value *items;
	size_t i, n = obj->arr != NULL ? obj->arr->n : 0;
	size_t kept = 0;

	memset(out, 0, sizeof(*out));
	out->kind = SGUG_EXPR_ARRAY;
	out->filtered = 1;
	out->arr = arr;
	if (arr == NULL)
		return -1;

	items = sgug_expr_arena_alloc(ctx->arena, n * sizeof(*items) + 1);
	if (items == NULL)
		return -1;

	for (i = 0; i < n; i++) {
		const sgug_expr_value *it = &obj->arr->items[i];
		const sgug_json *v = NULL;

		if (it->kind == SGUG_EXPR_OBJECT && is_primitive(key))
			v = obj_get(it->node, to_string(ctx, key));
		else if (it->kind == SGUG_EXPR_ARRAY) {
			double d = to_number(key);

			if (d == d && d >= 0 && (size_t)d < sgug_json_len(it->node))
				v = sgug_json_at(it->node, (size_t)d);
		}
		if (v != NULL)
			items[kept++] = from_json(v);
	}
	arr->items = items;
	arr->n = kept;
	return 0;
}

static int
eval_index(sgug_expr_ctx *ctx, const sgug_expr_value *obj,
    const sgug_expr_value *key, sgug_expr_value *out)
{
	*out = mknull();

	if (obj->kind == SGUG_EXPR_OBJECT) {
		if (!is_primitive(key))
			return 0;
		*out = from_json(obj_get(obj->node, to_string(ctx, key)));
		return 0;
	}

	if (obj->kind == SGUG_EXPR_ARRAY && obj->filtered)
		return flatten(ctx, obj, key, out);

	if (obj->kind == SGUG_EXPR_ARRAY) {
		double d = to_number(key);
		size_t i, n;

		if (d != d || d < 0 || d > 2147483647.0)
			return 0;
		i = (size_t)d;
		n = obj->arr != NULL ? obj->arr->n : sgug_json_len(obj->node);
		if (i >= n)
			return 0;
		*out = obj->arr != NULL ? obj->arr->items[i]
		    : from_json(sgug_json_at(obj->node, i));
		return 0;
	}
	return 0;
}

/* obj.* collects every value; a non-collection yields an empty array, not
 * null. Items lacking the key are dropped rather than becoming null. */
static int
eval_wildcard(sgug_expr_ctx *ctx, const sgug_expr_value *obj,
    sgug_expr_value *out)
{
	sgug_expr_array *arr = sgug_expr_arena_alloc(ctx->arena, sizeof(*arr));
	sgug_expr_value *items;
	size_t i, n = 0;

	memset(out, 0, sizeof(*out));
	out->kind = SGUG_EXPR_ARRAY;
	out->filtered = 1;
	out->arr = arr;
	if (arr == NULL)
		return -1;

	if (obj->kind == SGUG_EXPR_OBJECT)
		n = obj_len(obj->node);
	else if (obj->kind == SGUG_EXPR_ARRAY)
		n = obj->arr != NULL ? obj->arr->n : sgug_json_len(obj->node);
	else
		return 0;

	items = sgug_expr_arena_alloc(ctx->arena, n * sizeof(*items) + 1);
	if (items == NULL)
		return -1;

	for (i = 0; i < n; i++) {
		if (obj->kind == SGUG_EXPR_OBJECT)
			items[i] = from_json(obj_at(obj->node, i, NULL));
		else if (obj->arr != NULL)
			items[i] = obj->arr->items[i];
		else
			items[i] = from_json(sgug_json_at(obj->node, i));
	}
	arr->items = items;
	arr->n = n;
	return 0;
}

static int
eval_node(sgug_expr_ctx *ctx, const struct sgug_expr_node *n,
    sgug_expr_value *out, char *err, size_t errlen)
{
	sgug_expr_value a, b;
	int rc = -1;

	if (n == NULL) {
		*out = mknull();
		return 0;
	}

	if (++ctx->depth > SGUG_EXPR_EVAL_DEPTH) {
		seterr(err, errlen, "expression is nested too deeply");
		goto out;
	}

	switch (n->type) {
	case SGUG_EXPR_N_LITERAL:
		*out = n->lit;
		break;

	case SGUG_EXPR_N_NAMED: {
		const sgug_json *v = sgug_expr_context_lookup(ctx, n->name);

		if (v == NULL) {
			seterr(err, errlen, "unrecognized named-value: %s",
			    n->name);
			goto out;
		}
		*out = from_json(v);
		break;
	}

	case SGUG_EXPR_N_WILDCARD:
		if (eval_node(ctx, n->a, &a, err, errlen) != 0)
			goto out;
		if (eval_wildcard(ctx, &a, out) != 0)
			goto out;
		break;

	case SGUG_EXPR_N_CALL: {
		/* From the arena, not the stack. 255 values is twelve kilobytes
		 * a frame, and the compiler reserves it for every node kind, so
		 * on the stack it would exhaust one long before the depth guard
		 * fired. */
		sgug_expr_value *args = sgug_expr_arena_alloc(ctx->arena,
		    (n->nargs + 1) * sizeof(*args));
		const struct sgug_expr_node *arg;
		size_t i = 0;

		if (args == NULL) {
			seterr(err, errlen, "out of memory");
			goto out;
		}

		for (arg = n->args; arg != NULL; arg = arg->next) {
			if (i >= n->nargs)
				break;
			if (eval_node(ctx, arg, &args[i], err, errlen) != 0)
				goto out;
			i++;
		}
		if (call_builtin(ctx, n->name, args, i, out, err, errlen) != 0)
			goto out;
		break;
	}

	case SGUG_EXPR_N_OP:
		if (n->op == SGUG_EXPR_OP_NOT) {
			if (eval_node(ctx, n->a, &a, err, errlen) != 0)
				goto out;
			*out = mkbool(is_falsy(&a));
			break;
		}

		if (eval_node(ctx, n->a, &a, err, errlen) != 0)
			goto out;

		/* && and || yield an operand, not a boolean, and short
		 * circuit before the right side is evaluated. */
		if (n->op == SGUG_EXPR_OP_AND) {
			if (is_falsy(&a))
				*out = a;
			else if (eval_node(ctx, n->b, out, err, errlen) != 0)
				goto out;
			break;
		}
		if (n->op == SGUG_EXPR_OP_OR) {
			if (!is_falsy(&a))
				*out = a;
			else if (eval_node(ctx, n->b, out, err, errlen) != 0)
				goto out;
			break;
		}

		if (eval_node(ctx, n->b, &b, err, errlen) != 0)
			goto out;

		switch (n->op) {
		case SGUG_EXPR_OP_EQ: *out = mkbool(abstract_equal(a, b)); break;
		case SGUG_EXPR_OP_NE: *out = mkbool(!abstract_equal(a, b)); break;
		case SGUG_EXPR_OP_GT: *out = mkbool(abstract_greater(a, b)); break;
		case SGUG_EXPR_OP_LT: *out = mkbool(abstract_greater(b, a)); break;
		case SGUG_EXPR_OP_GE:
			*out = mkbool(abstract_equal(a, b) ||
			    abstract_greater(a, b));
			break;
		case SGUG_EXPR_OP_LE:
			*out = mkbool(abstract_equal(a, b) ||
			    abstract_greater(b, a));
			break;
		default:
			if (eval_index(ctx, &a, &b, out) != 0)
				goto out;
			break;
		}
		break;
	}

	rc = 0;
out:
	ctx->depth--;
	return rc;
}

int
sgug_expr_eval(sgug_expr_ctx *ctx, const char *src, sgug_expr_value *out,
    char *err, size_t errlen)
{
	struct sgug_expr_node *ast;

	/*
	 * The AST and every string it produces live here, so without this a
	 * job's worth of steps accumulates in one arena. Invalidates whatever
	 * the previous call returned.
	 */
	sgug_expr_arena_reset(ctx->arena);
	ctx->depth = 0;
	ast = sgug_expr_parse(ctx->arena, src, err, errlen);
	if (ast == NULL)
		return -1;
	return eval_node(ctx, ast, out, err, errlen);
}

int
sgug_expr_eval_string(sgug_expr_ctx *ctx, const char *src, char **out,
    char *err, size_t errlen)
{
	sgug_expr_value v;
	const char *s;

	if (sgug_expr_eval(ctx, src, &v, err, errlen) != 0)
		return -1;

	s = to_string(ctx, &v);
	*out = malloc(strlen(s) + 1);
	if (*out == NULL) {
		seterr(err, errlen, "out of memory");
		return -1;
	}
	strcpy(*out, s);
	return 0;
}

int
sgug_expr_eval_bool(sgug_expr_ctx *ctx, const char *src, int *out, char *err,
    size_t errlen)
{
	sgug_expr_value v;

	if (sgug_expr_eval(ctx, src, &v, err, errlen) != 0)
		return -1;
	*out = !is_falsy(&v);
	return 0;
}

int
sgug_expr_eval_token(sgug_expr_ctx *ctx, const sgug_json *tok, char **out,
    char *err, size_t errlen)
{
	const sgug_json *e;
	const char *lit;

	*out = NULL;
	if (tok == NULL)
		return 0;

	e = sgug_json_get(tok, "expr");
	if (e != NULL)
		return sgug_expr_eval_string(ctx, sgug_json_string(e, ""), out,
		    err, errlen);

	/* A mapping or a sequence reads as absent, which is all a handler can
	 * do with one anyway. */
	lit = sgug_token_str(tok, NULL);
	if (lit == NULL)
		return 0;

	*out = malloc(strlen(lit) + 1);
	if (*out == NULL) {
		seterr(err, errlen, "out of memory");
		return -1;
	}
	strcpy(*out, lit);
	return 0;
}
