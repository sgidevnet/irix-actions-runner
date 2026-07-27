#ifndef SGUG_PROTO_MASK_H
#define SGUG_PROTO_MASK_H

#include <stddef.h>

/*
 * Replaces every listed secret with "***".
 *
 * Separate from the reporter so it can be tested directly. The replacement is
 * longer than a short secret, so this cannot be done by shifting within the
 * input; the earlier in-place version wrote past its own allocation for any
 * mask under three bytes.
 *
 * Returns a new string the caller frees, or NULL if out of memory.
 */
char *sgug_mask_apply(const char *const *masks, size_t nmasks,
    const char *line);

#endif /* SGUG_PROTO_MASK_H */
