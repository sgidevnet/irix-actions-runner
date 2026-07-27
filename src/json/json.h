#ifndef SGUG_JSON_JSON_H
#define SGUG_JSON_JSON_H

#include "compat/irix.h"

#include <stddef.h>

/*
 * A read-only DOM parser plus a streaming writer, sized for this protocol.
 *
 * Job messages run to tens of kilobytes and are parsed once, read many times,
 * then discarded whole, so nodes come from an arena and are freed in one call.
 * On a 400MHz R12000 that matters more than it would elsewhere.
 */

typedef enum {
	SGUG_JSON_NULL,
	SGUG_JSON_BOOL,
	SGUG_JSON_NUMBER,
	SGUG_JSON_STRING,
	SGUG_JSON_ARRAY,
	SGUG_JSON_OBJECT
} sgug_json_type;

typedef struct sgug_json sgug_json;
typedef struct sgug_json_doc sgug_json_doc;

/*
 * Parses text of len bytes. On failure returns NULL and writes a message
 * naming the byte offset into err.
 */
sgug_json_doc *sgug_json_parse(const char *text, size_t len,
    char *err, size_t errlen);
void sgug_json_free(sgug_json_doc *doc);
const sgug_json *sgug_json_root(const sgug_json_doc *doc);

sgug_json_type sgug_json_type_of(const sgug_json *v);

/*
 * Object lookup, case-insensitive.
 *
 * This is not a convenience. The Actions service serialises with camelCase on
 * the wire while the C# contracts that document it are PascalCase, and
 * individual fields disagree: `.runner` files use PascalCase, message bodies
 * use camelCase, and TimelineRecord declares [DataMember(Name = "Type")] while
 * emitting "type". The reference implementations get away with it because both
 * .NET and Go match member names case-insensitively by default. Matching
 * exactly would silently return NULL on fields that are present.
 */
const sgug_json *sgug_json_get(const sgug_json *obj, const char *key);

/* Number of elements in an array or members in an object; 0 for anything else. */
size_t sgug_json_len(const sgug_json *v);
const sgug_json *sgug_json_at(const sgug_json *arr, size_t i);

/* Nth member of an object, for iteration. Either output may be NULL. */
int sgug_json_member(const sgug_json *obj, size_t i,
    const char **key, const sgug_json **val);

/*
 * Accessors. Each returns fallback when v is NULL or of another type, so a
 * missing optional field needs no separate check. Strings are NUL terminated
 * and remain valid until the document is freed.
 */
const char *sgug_json_string(const sgug_json *v, const char *fallback);
int64_t sgug_json_int(const sgug_json *v, int64_t fallback);
double sgug_json_double(const sgug_json *v, double fallback);
int sgug_json_bool(const sgug_json *v, int fallback);

/*
 * Dotted path lookup: sgug_json_path(root, "authorization.publicKey.modulus").
 * Each segment is matched case-insensitively. Returns NULL if any segment is
 * missing or names a non-object.
 */
const sgug_json *sgug_json_path(const sgug_json *root, const char *path);

/*
 * Streaming writer. Grows as needed; every call is a no-op once an allocation
 * has failed, so only the final sgug_jsonw_done needs checking.
 */
typedef struct sgug_jsonw sgug_jsonw;

sgug_jsonw *sgug_jsonw_new(void);
void sgug_jsonw_free(sgug_jsonw *w);

void sgug_jsonw_obj_begin(sgug_jsonw *w);
void sgug_jsonw_obj_end(sgug_jsonw *w);
void sgug_jsonw_arr_begin(sgug_jsonw *w);
void sgug_jsonw_arr_end(sgug_jsonw *w);

/* Member name for the next value. Only valid inside an object. */
void sgug_jsonw_key(sgug_jsonw *w, const char *key);

void sgug_jsonw_str(sgug_jsonw *w, const char *s);
void sgug_jsonw_int(sgug_jsonw *w, int64_t n);
void sgug_jsonw_bool(sgug_jsonw *w, int b);
void sgug_jsonw_null(sgug_jsonw *w);

/* Emits pre-encoded JSON verbatim. The caller owns its validity. */
void sgug_jsonw_raw(sgug_jsonw *w, const char *json);

/*
 * Finished document, NUL terminated, owned by the writer. Returns NULL if any
 * allocation failed or the structure is unbalanced. *len is optional.
 */
const char *sgug_jsonw_done(sgug_jsonw *w, size_t *len);

#endif /* SGUG_JSON_JSON_H */
