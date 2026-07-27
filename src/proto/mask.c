#include "proto/mask.h"

#include <stdlib.h>
#include <string.h>

#define REPLACEMENT "***"

/* Longest mask that matches at p, or 0. Longest wins so that a secret which
 * contains a shorter secret is not left partly visible. */
static size_t
match_at(const char *const *masks, size_t nmasks, const char *p)
{
	size_t best = 0, i;

	for (i = 0; i < nmasks; i++) {
		size_t len;

		if (masks[i] == NULL)
			continue;
		len = strlen(masks[i]);
		if (len > best && strncmp(p, masks[i], len) == 0)
			best = len;
	}
	return best;
}

char *
sgug_mask_apply(const char *const *masks, size_t nmasks, const char *line)
{
	const char *p = line;
	size_t cap, used = 0;
	char *out;

	if (line == NULL)
		return NULL;

	cap = strlen(line) + 1;
	out = malloc(cap);
	if (out == NULL)
		return NULL;

	while (*p != '\0') {
		size_t hit = nmasks > 0 ? match_at(masks, nmasks, p) : 0;
		const char *add;
		size_t addlen;

		if (hit > 0) {
			add = REPLACEMENT;
			addlen = sizeof(REPLACEMENT) - 1;
			p += hit;
		} else {
			add = p;
			addlen = 1;
			p++;
		}

		if (used + addlen + 1 > cap) {
			char *bigger;

			cap = (used + addlen + 1) * 2;
			bigger = realloc(out, cap);
			if (bigger == NULL) {
				free(out);
				return NULL;
			}
			out = bigger;
		}

		memcpy(out + used, add, addlen);
		used += addlen;
	}

	out[used] = '\0';
	return out;
}
