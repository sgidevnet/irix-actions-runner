#include "json/json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*
 * Bounds the recursion in both the parser and the free path. Job messages nest
 * about six deep through contextData; anything approaching this limit is
 * malformed or hostile, and blowing the stack on a machine with no guard page
 * behaviour worth relying on is not an acceptable failure mode.
 */
#define MAX_DEPTH 64

#define ARENA_CHUNK 16384

struct arena_chunk {
	struct arena_chunk *next;
	size_t used;
	size_t cap;
	char *base;
};

struct sgug_json {
	sgug_json_type type;
	union {
		int b;
		struct {
			double d;
			int64_t i;
			int is_int;
		} num;
		const char *str;
		struct {
			sgug_json *items;
			size_t len;
		} arr;
		struct {
			const char **keys;
			sgug_json *vals;
			size_t len;
		} obj;
	} u;
};

struct sgug_json_doc {
	struct arena_chunk *chunks;
	sgug_json *root;
};

struct parser {
	const char *p;
	const char *end;
	struct sgug_json_doc *doc;
	int depth;
	char *err;
	size_t errlen;
	const char *start;
};

static void *
arena_alloc(struct sgug_json_doc *doc, size_t n)
{
	struct arena_chunk *c = doc->chunks;
	size_t cap;
	void *p;

	/* Keep everything pointer-aligned; MIPS traps on unaligned access. */
	n = (n + 7) & ~(size_t)7;

	if (c == NULL || c->used + n > c->cap) {
		cap = n > ARENA_CHUNK ? n : ARENA_CHUNK;
		c = malloc(sizeof(*c));
		if (c == NULL)
			return NULL;
		c->base = malloc(cap);
		if (c->base == NULL) {
			free(c);
			return NULL;
		}
		c->used = 0;
		c->cap = cap;
		c->next = doc->chunks;
		doc->chunks = c;
	}

	p = c->base + c->used;
	c->used += n;
	return p;
}

static void
fail(struct parser *ps, const char *msg)
{
	if (ps->err != NULL && ps->errlen > 0 && ps->err[0] == '\0') {
		sgug_snprintf(ps->err, ps->errlen, "%s at offset %lu", msg,
		    (unsigned long)(ps->p - ps->start));
	}
}

static void
skip_ws(struct parser *ps)
{
	while (ps->p < ps->end) {
		char c = *ps->p;

		if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
			break;
		ps->p++;
	}
}

static int parse_value(struct parser *ps, sgug_json *out);

/* Encodes one code point as UTF-8. Returns bytes written. */
static int
utf8_encode(unsigned long cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

static int
hex4(const char *p, unsigned long *out)
{
	unsigned long v = 0;
	int i;

	for (i = 0; i < 4; i++) {
		char c = p[i];

		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (unsigned long)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (unsigned long)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (unsigned long)(c - 'A' + 10);
		else
			return -1;
	}
	*out = v;
	return 0;
}

/*
 * Unescapes into the arena. The decoded form is never longer than the source,
 * since every escape shrinks: \uXXXX is six bytes in and at most three out, and
 * a surrogate pair is twelve in and four out.
 */
static const char *
parse_string_raw(struct parser *ps)
{
	const char *src;
	char *dst, *out;
	size_t max;

	if (ps->p >= ps->end || *ps->p != '"') {
		fail(ps, "expected string");
		return NULL;
	}
	ps->p++;
	src = ps->p;

	while (ps->p < ps->end && *ps->p != '"') {
		if (*ps->p == '\\') {
			ps->p++;
			if (ps->p >= ps->end)
				break;
		}
		ps->p++;
	}
	if (ps->p >= ps->end) {
		fail(ps, "unterminated string");
		return NULL;
	}

	max = (size_t)(ps->p - src);
	out = arena_alloc(ps->doc, max + 1);
	if (out == NULL) {
		fail(ps, "out of memory");
		return NULL;
	}

	dst = out;
	while (src < ps->p) {
		if (*src != '\\') {
			*dst++ = *src++;
			continue;
		}

		src++;
		switch (*src) {
		case '"': *dst++ = '"'; src++; break;
		case '\\': *dst++ = '\\'; src++; break;
		case '/': *dst++ = '/'; src++; break;
		case 'b': *dst++ = '\b'; src++; break;
		case 'f': *dst++ = '\f'; src++; break;
		case 'n': *dst++ = '\n'; src++; break;
		case 'r': *dst++ = '\r'; src++; break;
		case 't': *dst++ = '\t'; src++; break;
		case 'u': {
			unsigned long cp;

			if (src + 5 > ps->p || hex4(src + 1, &cp) != 0) {
				fail(ps, "bad \\u escape");
				return NULL;
			}
			src += 5;

			/* A high surrogate must be followed by its low pair, or
			 * the code point is not representable. */
			if (cp >= 0xd800 && cp <= 0xdbff) {
				unsigned long lo;

				if (src + 6 <= ps->p && src[0] == '\\' &&
				    src[1] == 'u' && hex4(src + 2, &lo) == 0 &&
				    lo >= 0xdc00 && lo <= 0xdfff) {
					cp = 0x10000 +
					    ((cp - 0xd800) << 10) + (lo - 0xdc00);
					src += 6;
				} else {
					cp = 0xfffd;
				}
			} else if (cp >= 0xdc00 && cp <= 0xdfff) {
				/* Unpaired low surrogate. */
				cp = 0xfffd;
			}

			dst += utf8_encode(cp, dst);
			break;
		}
		default:
			fail(ps, "bad escape");
			return NULL;
		}
	}

	*dst = '\0';
	ps->p++;
	return out;
}

static int
parse_number(struct parser *ps, sgug_json *out)
{
	const char *start = ps->p;
	int negative = 0;
	int64_t ival = 0;
	int is_int = 1;
	int overflow = 0;

	if (ps->p < ps->end && (*ps->p == '-' || *ps->p == '+')) {
		negative = (*ps->p == '-');
		ps->p++;
	}

	if (ps->p >= ps->end || !isdigit((unsigned char)*ps->p)) {
		fail(ps, "expected number");
		return -1;
	}

	while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) {
		int d = *ps->p - '0';

		/* messageId is a genuine 64-bit value, so this must not narrow
		 * silently. Past the range we fall back to double. */
		if (ival > (int64_t)922337203685477580LL ||
		    (ival == (int64_t)922337203685477580LL && d > 7))
			overflow = 1;
		else
			ival = ival * 10 + d;
		ps->p++;
	}

	if (ps->p < ps->end && *ps->p == '.') {
		is_int = 0;
		ps->p++;
		while (ps->p < ps->end && isdigit((unsigned char)*ps->p))
			ps->p++;
	}
	if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
		is_int = 0;
		ps->p++;
		if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-'))
			ps->p++;
		while (ps->p < ps->end && isdigit((unsigned char)*ps->p))
			ps->p++;
	}

	out->type = SGUG_JSON_NUMBER;
	out->u.num.is_int = is_int && !overflow;
	out->u.num.i = negative ? -ival : ival;
	out->u.num.d = strtod(start, NULL);
	return 0;
}

static int
lit(struct parser *ps, const char *word)
{
	size_t n = strlen(word);

	if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0)
		return -1;
	ps->p += n;
	return 0;
}

/*
 * Members are collected into a temporary vector that doubles, then copied into
 * the arena once the final count is known. Growing inside the arena is not
 * possible because it never frees individual allocations.
 */
struct veclist {
	sgug_json *vals;
	const char **keys;
	size_t len;
	size_t cap;
};

static int
veclist_push(struct veclist *v, const char *key, const sgug_json *val)
{
	if (v->len == v->cap) {
		size_t ncap = v->cap == 0 ? 8 : v->cap * 2;
		sgug_json *nv = realloc(v->vals, ncap * sizeof(*nv));
		const char **nk;

		if (nv == NULL)
			return -1;
		v->vals = nv;

		nk = realloc(v->keys, ncap * sizeof(*nk));
		if (nk == NULL)
			return -1;
		v->keys = nk;
		v->cap = ncap;
	}

	v->keys[v->len] = key;
	v->vals[v->len] = *val;
	v->len++;
	return 0;
}

static void
veclist_done(struct veclist *v)
{
	free(v->vals);
	free(v->keys);
}

static int
parse_array(struct parser *ps, sgug_json *out)
{
	struct veclist v;

	memset(&v, 0, sizeof(v));
	ps->p++;

	out->type = SGUG_JSON_ARRAY;
	out->u.arr.items = NULL;
	out->u.arr.len = 0;

	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == ']') {
		ps->p++;
		return 0;
	}

	for (;;) {
		sgug_json item;

		skip_ws(ps);
		if (parse_value(ps, &item) != 0)
			goto fail;
		if (veclist_push(&v, NULL, &item) != 0) {
			fail(ps, "out of memory");
			goto fail;
		}

		skip_ws(ps);
		if (ps->p < ps->end && *ps->p == ',') {
			ps->p++;
			continue;
		}
		if (ps->p < ps->end && *ps->p == ']') {
			ps->p++;
			break;
		}
		fail(ps, "expected , or ] in array");
		goto fail;
	}

	if (v.len > 0) {
		out->u.arr.items = arena_alloc(ps->doc, v.len * sizeof(sgug_json));
		if (out->u.arr.items == NULL) {
			fail(ps, "out of memory");
			goto fail;
		}
		memcpy(out->u.arr.items, v.vals, v.len * sizeof(sgug_json));
		out->u.arr.len = v.len;
	}

	veclist_done(&v);
	return 0;

fail:
	veclist_done(&v);
	return -1;
}

static int
parse_object(struct parser *ps, sgug_json *out)
{
	struct veclist v;

	memset(&v, 0, sizeof(v));
	ps->p++;

	out->type = SGUG_JSON_OBJECT;
	out->u.obj.keys = NULL;
	out->u.obj.vals = NULL;
	out->u.obj.len = 0;

	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == '}') {
		ps->p++;
		return 0;
	}

	for (;;) {
		const char *key;
		sgug_json val;

		skip_ws(ps);
		key = parse_string_raw(ps);
		if (key == NULL)
			goto fail;

		skip_ws(ps);
		if (ps->p >= ps->end || *ps->p != ':') {
			fail(ps, "expected :");
			goto fail;
		}
		ps->p++;

		skip_ws(ps);
		if (parse_value(ps, &val) != 0)
			goto fail;
		if (veclist_push(&v, key, &val) != 0) {
			fail(ps, "out of memory");
			goto fail;
		}

		skip_ws(ps);
		if (ps->p < ps->end && *ps->p == ',') {
			ps->p++;
			continue;
		}
		if (ps->p < ps->end && *ps->p == '}') {
			ps->p++;
			break;
		}
		fail(ps, "expected , or } in object");
		goto fail;
	}

	if (v.len > 0) {
		out->u.obj.keys = arena_alloc(ps->doc, v.len * sizeof(char *));
		out->u.obj.vals = arena_alloc(ps->doc, v.len * sizeof(sgug_json));
		if (out->u.obj.keys == NULL || out->u.obj.vals == NULL) {
			fail(ps, "out of memory");
			goto fail;
		}
		memcpy((void *)out->u.obj.keys, v.keys, v.len * sizeof(char *));
		memcpy(out->u.obj.vals, v.vals, v.len * sizeof(sgug_json));
		out->u.obj.len = v.len;
	}

	veclist_done(&v);
	return 0;

fail:
	veclist_done(&v);
	return -1;
}

static int
parse_value(struct parser *ps, sgug_json *out)
{
	int rc;

	if (ps->depth >= MAX_DEPTH) {
		fail(ps, "nesting too deep");
		return -1;
	}
	if (ps->p >= ps->end) {
		fail(ps, "unexpected end of input");
		return -1;
	}

	memset(out, 0, sizeof(*out));

	switch (*ps->p) {
	case '{':
		ps->depth++;
		rc = parse_object(ps, out);
		ps->depth--;
		return rc;
	case '[':
		ps->depth++;
		rc = parse_array(ps, out);
		ps->depth--;
		return rc;
	case '"':
		out->type = SGUG_JSON_STRING;
		out->u.str = parse_string_raw(ps);
		return out->u.str != NULL ? 0 : -1;
	case 't':
		if (lit(ps, "true") != 0) {
			fail(ps, "bad literal");
			return -1;
		}
		out->type = SGUG_JSON_BOOL;
		out->u.b = 1;
		return 0;
	case 'f':
		if (lit(ps, "false") != 0) {
			fail(ps, "bad literal");
			return -1;
		}
		out->type = SGUG_JSON_BOOL;
		out->u.b = 0;
		return 0;
	case 'n':
		if (lit(ps, "null") != 0) {
			fail(ps, "bad literal");
			return -1;
		}
		out->type = SGUG_JSON_NULL;
		return 0;
	default:
		return parse_number(ps, out);
	}
}

sgug_json_doc *
sgug_json_parse(const char *text, size_t len, char *err, size_t errlen)
{
	struct sgug_json_doc *doc;
	struct parser ps;
	sgug_json root;

	if (err != NULL && errlen > 0)
		err[0] = '\0';

	doc = calloc(1, sizeof(*doc));
	if (doc == NULL)
		return NULL;

	ps.start = text;
	ps.p = text;
	ps.end = text + len;
	ps.doc = doc;
	ps.depth = 0;
	ps.err = err;
	ps.errlen = errlen;

	/* A UTF-8 BOM is stripped by the message decrypt path, but a body read
	 * straight off the wire can still carry one. */
	if (len >= 3 && (unsigned char)text[0] == 0xef &&
	    (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf)
		ps.p += 3;

	skip_ws(&ps);
	if (parse_value(&ps, &root) != 0) {
		sgug_json_free(doc);
		return NULL;
	}

	skip_ws(&ps);
	if (ps.p != ps.end) {
		fail(&ps, "trailing content");
		sgug_json_free(doc);
		return NULL;
	}

	doc->root = arena_alloc(doc, sizeof(root));
	if (doc->root == NULL) {
		sgug_json_free(doc);
		return NULL;
	}
	*doc->root = root;
	return doc;
}

void
sgug_json_free(sgug_json_doc *doc)
{
	struct arena_chunk *c;

	if (doc == NULL)
		return;

	c = doc->chunks;
	while (c != NULL) {
		struct arena_chunk *next = c->next;

		free(c->base);
		free(c);
		c = next;
	}
	free(doc);
}

const sgug_json *
sgug_json_root(const sgug_json_doc *doc)
{
	return doc != NULL ? doc->root : NULL;
}

sgug_json_type
sgug_json_type_of(const sgug_json *v)
{
	return v != NULL ? v->type : SGUG_JSON_NULL;
}

static int
key_eq_ci(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

const sgug_json *
sgug_json_get(const sgug_json *obj, const char *key)
{
	size_t i;

	if (obj == NULL || obj->type != SGUG_JSON_OBJECT || key == NULL)
		return NULL;

	for (i = 0; i < obj->u.obj.len; i++) {
		if (key_eq_ci(obj->u.obj.keys[i], key))
			return &obj->u.obj.vals[i];
	}
	return NULL;
}

size_t
sgug_json_len(const sgug_json *v)
{
	if (v == NULL)
		return 0;
	if (v->type == SGUG_JSON_ARRAY)
		return v->u.arr.len;
	if (v->type == SGUG_JSON_OBJECT)
		return v->u.obj.len;
	return 0;
}

const sgug_json *
sgug_json_at(const sgug_json *arr, size_t i)
{
	if (arr == NULL || arr->type != SGUG_JSON_ARRAY || i >= arr->u.arr.len)
		return NULL;
	return &arr->u.arr.items[i];
}

int
sgug_json_member(const sgug_json *obj, size_t i, const char **key,
    const sgug_json **val)
{
	if (obj == NULL || obj->type != SGUG_JSON_OBJECT || i >= obj->u.obj.len)
		return -1;

	if (key != NULL)
		*key = obj->u.obj.keys[i];
	if (val != NULL)
		*val = &obj->u.obj.vals[i];
	return 0;
}

const char *
sgug_json_string(const sgug_json *v, const char *fallback)
{
	if (v == NULL || v->type != SGUG_JSON_STRING)
		return fallback;
	return v->u.str;
}

int64_t
sgug_json_int(const sgug_json *v, int64_t fallback)
{
	if (v == NULL || v->type != SGUG_JSON_NUMBER)
		return fallback;
	if (v->u.num.is_int)
		return v->u.num.i;
	return (int64_t)v->u.num.d;
}

double
sgug_json_double(const sgug_json *v, double fallback)
{
	if (v == NULL || v->type != SGUG_JSON_NUMBER)
		return fallback;
	return v->u.num.d;
}

int
sgug_json_bool(const sgug_json *v, int fallback)
{
	if (v == NULL)
		return fallback;
	if (v->type == SGUG_JSON_BOOL)
		return v->u.b;

	/*
	 * PropertiesCollection and the .credentials Data map carry booleans as
	 * the strings "True"/"true", because they are Dictionary<string,string>
	 * on the C# side. Treating those as absent would silently disable FIPS
	 * signing and produce an unexplained 401.
	 */
	if (v->type == SGUG_JSON_STRING)
		return key_eq_ci(v->u.str, "true");
	return fallback;
}

const sgug_json *
sgug_json_path(const sgug_json *root, const char *path)
{
	const sgug_json *cur = root;
	char seg[128];

	while (*path != '\0' && cur != NULL) {
		const char *dot = strchr(path, '.');
		size_t n = dot != NULL ? (size_t)(dot - path) : strlen(path);

		if (n == 0 || n >= sizeof(seg))
			return NULL;

		memcpy(seg, path, n);
		seg[n] = '\0';

		cur = sgug_json_get(cur, seg);
		path += n;
		if (*path == '.')
			path++;
	}
	return cur;
}

/* Writer. */

struct sgug_jsonw {
	char *buf;
	size_t len;
	size_t cap;
	int depth;
	/* Bit per level: 1 once the current container has a member, so the next
	 * one is preceded by a comma. */
	unsigned long seen[MAX_DEPTH / 32 + 1];
	int need_comma;
	int failed;
	int after_key;
};

sgug_jsonw *
sgug_jsonw_new(void)
{
	sgug_jsonw *w = calloc(1, sizeof(*w));

	if (w == NULL)
		return NULL;
	w->cap = 512;
	w->buf = malloc(w->cap);
	if (w->buf == NULL) {
		free(w);
		return NULL;
	}
	w->buf[0] = '\0';
	return w;
}

void
sgug_jsonw_free(sgug_jsonw *w)
{
	if (w == NULL)
		return;
	free(w->buf);
	free(w);
}

static void
wput(sgug_jsonw *w, const char *s, size_t n)
{
	if (w->failed)
		return;

	if (w->len + n + 1 > w->cap) {
		size_t ncap = w->cap;
		char *nb;

		while (ncap < w->len + n + 1)
			ncap *= 2;
		nb = realloc(w->buf, ncap);
		if (nb == NULL) {
			w->failed = 1;
			return;
		}
		w->buf = nb;
		w->cap = ncap;
	}

	memcpy(w->buf + w->len, s, n);
	w->len += n;
	w->buf[w->len] = '\0';
}

static void
wsep(sgug_jsonw *w)
{
	if (w->after_key) {
		w->after_key = 0;
		return;
	}
	if (w->need_comma)
		wput(w, ",", 1);
	w->need_comma = 1;
}

static void
wstr(sgug_jsonw *w, const char *s)
{
	static const char HEX[] = "0123456789abcdef";
	char esc[8];

	wput(w, "\"", 1);
	for (; *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;

		switch (c) {
		case '"': wput(w, "\\\"", 2); break;
		case '\\': wput(w, "\\\\", 2); break;
		case '\b': wput(w, "\\b", 2); break;
		case '\f': wput(w, "\\f", 2); break;
		case '\n': wput(w, "\\n", 2); break;
		case '\r': wput(w, "\\r", 2); break;
		case '\t': wput(w, "\\t", 2); break;
		default:
			if (c < 0x20) {
				esc[0] = '\\';
				esc[1] = 'u';
				esc[2] = '0';
				esc[3] = '0';
				esc[4] = HEX[c >> 4];
				esc[5] = HEX[c & 0x0f];
				wput(w, esc, 6);
			} else {
				/* Bytes at or above 0x80 pass through: the
				 * input is already UTF-8 and JSON permits it
				 * unescaped. */
				wput(w, (const char *)&c, 1);
			}
		}
	}
	wput(w, "\"", 1);
}

static void
push_depth(sgug_jsonw *w)
{
	if (w->depth >= MAX_DEPTH) {
		w->failed = 1;
		return;
	}
	w->seen[w->depth / 32] &= ~(1UL << (w->depth % 32));
	if (w->need_comma)
		w->seen[w->depth / 32] |= 1UL << (w->depth % 32);
	w->depth++;
	w->need_comma = 0;
}

static void
pop_depth(sgug_jsonw *w)
{
	if (w->depth <= 0) {
		w->failed = 1;
		return;
	}
	w->depth--;
	w->need_comma =
	    (w->seen[w->depth / 32] & (1UL << (w->depth % 32))) != 0;
}

void
sgug_jsonw_obj_begin(sgug_jsonw *w)
{
	wsep(w);
	wput(w, "{", 1);
	push_depth(w);
}

void
sgug_jsonw_obj_end(sgug_jsonw *w)
{
	wput(w, "}", 1);
	pop_depth(w);
	w->need_comma = 1;
}

void
sgug_jsonw_arr_begin(sgug_jsonw *w)
{
	wsep(w);
	wput(w, "[", 1);
	push_depth(w);
}

void
sgug_jsonw_arr_end(sgug_jsonw *w)
{
	wput(w, "]", 1);
	pop_depth(w);
	w->need_comma = 1;
}

void
sgug_jsonw_key(sgug_jsonw *w, const char *key)
{
	wsep(w);
	wstr(w, key);
	wput(w, ":", 1);
	w->after_key = 1;
}

void
sgug_jsonw_str(sgug_jsonw *w, const char *s)
{
	wsep(w);
	if (s == NULL)
		wput(w, "null", 4);
	else
		wstr(w, s);
}

void
sgug_jsonw_int(sgug_jsonw *w, int64_t n)
{
	char buf[24];
	int len;

	wsep(w);
	/* Not printf: long is 32 bits under n32, so "%ld" truncates messageId. */
	len = sgug_i64toa(n, buf, sizeof(buf));
	if (len < 0) {
		w->failed = 1;
		return;
	}
	wput(w, buf, (size_t)len);
}

void
sgug_jsonw_bool(sgug_jsonw *w, int b)
{
	wsep(w);
	if (b)
		wput(w, "true", 4);
	else
		wput(w, "false", 5);
}

void
sgug_jsonw_null(sgug_jsonw *w)
{
	wsep(w);
	wput(w, "null", 4);
}

void
sgug_jsonw_raw(sgug_jsonw *w, const char *json)
{
	wsep(w);
	wput(w, json, strlen(json));
}

const char *
sgug_jsonw_done(sgug_jsonw *w, size_t *len)
{
	if (w == NULL || w->failed || w->depth != 0)
		return NULL;
	if (len != NULL)
		*len = w->len;
	return w->buf;
}
