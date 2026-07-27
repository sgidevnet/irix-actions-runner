#ifndef SGUG_EXEC_TOKEN_H
#define SGUG_EXEC_TOKEN_H

#include "json/json.h"

/*
 * Readers for the pipeline data model.
 *
 * A single job message carries two different serialisations of what is
 * conceptually the same thing, and the reference documentation describes only
 * one of them. Steps arrive as TemplateToken:
 *
 *   {"type": 2, "map": [{"Key":   {"type": 0, "lit": "script"},
 *                        "Value": {"type": 0, "lit": "make"}}]}
 *
 * while contextData arrives as PipelineContextData, whose values are raw JSON
 * rather than tagged objects:
 *
 *   {"t": 2, "d": [{"k": "ref", "v": "refs/heads/main"},
 *                  {"k": "check_run_id", "v": 90005817181}]}
 *
 * Reading a step's inputs with the compact spelling returns nothing at all,
 * with no error: the script comes back empty and the step silently does
 * nothing. These accept either form.
 */

/*
 * String value of a scalar token. Handles the verbose `lit`, the compact `s`,
 * and a bare JSON string. Returns fallback for anything else, including a
 * number or a container.
 */
const char *sgug_token_str(const sgug_json *tok, const char *fallback);

/*
 * Whether a scalar token carries a literal value that sgug_token_str can read.
 *
 * False for the expression form the service emits when the YAML contained
 * `${{ }}`: a whole scalar becomes an expression token, and an interpolated one
 * becomes a format() call over the literal parts. Callers must distinguish that
 * from an absent key, because the fallback for an expression is indistinguishable
 * from "not set" and silently produces an empty script.
 */
int sgug_token_is_literal(const sgug_json *tok);

/*
 * Value for a key inside a mapping token, matched case-insensitively.
 * Returns NULL when absent or when tok is not a mapping.
 */
const sgug_json *sgug_token_map_get(const sgug_json *tok, const char *key);

/* Convenience: string value at a key of a mapping token. */
const char *sgug_token_map_str(const sgug_json *tok, const char *key,
    const char *fallback);

/* Number of entries in a mapping or sequence token. */
size_t sgug_token_len(const sgug_json *tok);

/*
 * Nth entry of a mapping token. Either output may be NULL. Keys are returned
 * as strings regardless of which form wrapped them.
 */
int sgug_token_map_at(const sgug_json *tok, size_t i, const char **key,
    const sgug_json **val);

#endif /* SGUG_EXEC_TOKEN_H */
