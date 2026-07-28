#include "exec/token.h"

#include <string.h>

const char *
sgug_token_str(const sgug_json *tok, const char *fallback)
{
	const sgug_json *v;

	if (tok == NULL)
		return fallback;

	/* PipelineContextData stores scalars unwrapped. */
	if (sgug_json_type_of(tok) == SGUG_JSON_STRING)
		return sgug_json_string(tok, fallback);

	v = sgug_json_get(tok, "lit");
	if (v == NULL)
		v = sgug_json_get(tok, "s");
	if (v == NULL)
		v = sgug_json_get(tok, "value");

	return sgug_json_string(v, fallback);
}

/* The entry list of a mapping token, whichever spelling was used. */
static const sgug_json *
map_entries(const sgug_json *tok)
{
	const sgug_json *e;

	if (tok == NULL)
		return NULL;

	e = sgug_json_get(tok, "map");
	if (e == NULL)
		e = sgug_json_get(tok, "d");

	return sgug_json_type_of(e) == SGUG_JSON_ARRAY ? e : NULL;
}

size_t
sgug_token_len(const sgug_json *tok)
{
	const sgug_json *e = map_entries(tok);

	if (e != NULL)
		return sgug_json_len(e);

	e = sgug_json_get(tok, "seq");
	if (e == NULL)
		e = sgug_json_get(tok, "a");
	if (sgug_json_type_of(e) == SGUG_JSON_ARRAY)
		return sgug_json_len(e);

	if (sgug_json_type_of(tok) == SGUG_JSON_ARRAY)
		return sgug_json_len(tok);
	return 0;
}

int
sgug_token_map_at(const sgug_json *tok, size_t i, const char **key,
    const sgug_json **val)
{
	const sgug_json *entries = map_entries(tok);
	const sgug_json *entry, *k, *v;

	if (entries == NULL)
		return -1;

	entry = sgug_json_at(entries, i);
	if (entry == NULL)
		return -1;

	/* "Key"/"Value" in the verbose form, "k"/"v" in the compact one. The
	 * lookup is case-insensitive, so "Key" also matches "key". */
	k = sgug_json_get(entry, "key");
	if (k == NULL)
		k = sgug_json_get(entry, "k");

	v = sgug_json_get(entry, "value");
	if (v == NULL)
		v = sgug_json_get(entry, "v");

	if (key != NULL)
		*key = sgug_token_str(k, "");
	if (val != NULL)
		*val = v;
	return 0;
}

const sgug_json *
sgug_token_map_get(const sgug_json *tok, const char *key)
{
	size_t i, n;

	if (key == NULL)
		return NULL;

	n = sgug_token_len(tok);
	for (i = 0; i < n; i++) {
		const char *k = NULL;
		const sgug_json *v = NULL;

		if (sgug_token_map_at(tok, i, &k, &v) != 0)
			continue;
		if (k != NULL && strcmp(k, key) == 0)
			return v;
	}
	return NULL;
}

const char *
sgug_token_map_str(const sgug_json *tok, const char *key, const char *fallback)
{
	return sgug_token_str(sgug_token_map_get(tok, key), fallback);
}
