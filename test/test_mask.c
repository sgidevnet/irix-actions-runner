/*
 * Secret redaction. The replacement is three bytes, so a shorter secret makes
 * the line grow; the earlier in-place implementation assumed it could only
 * shrink and wrote past its allocation for any mask under three characters.
 */

#include "proto/mask.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK_STR(got, want) \
	do { \
		char *g_ = (got); \
		if (g_ == NULL || strcmp(g_, (want)) != 0) { \
			printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", \
			    __FILE__, __LINE__, g_ != NULL ? g_ : "(null)", \
			    (want)); \
			failures++; \
		} \
		free(g_); \
	} while (0)

static void
test_no_masks(void)
{
	CHECK_STR(sgug_mask_apply(NULL, 0, "nothing to hide"),
	    "nothing to hide");
}

static void
test_absent(void)
{
	const char *m[] = { "hunter2" };

	CHECK_STR(sgug_mask_apply(m, 1, "all clear"), "all clear");
}

static void
test_basic(void)
{
	const char *m[] = { "hunter2" };

	CHECK_STR(sgug_mask_apply(m, 1, "pw=hunter2 ok"), "pw=*** ok");
}

/* The line gets longer than the input. This overflowed the old buffer. */
static void
test_one_character_mask(void)
{
	const char *m[] = { "a" };

	CHECK_STR(sgug_mask_apply(m, 1, "banana"), "b***n***n***");
}

static void
test_two_character_mask(void)
{
	const char *m[] = { "ab" };

	CHECK_STR(sgug_mask_apply(m, 1, "abcabcab"), "***c***c***");
}

static void
test_repeats(void)
{
	const char *m[] = { "s3cret" };

	CHECK_STR(sgug_mask_apply(m, 1, "s3cret and s3cret"), "*** and ***");
}

/* The longer secret must win, or the tail of it survives in the log. */
static void
test_overlapping(void)
{
	const char *m[] = { "pass", "password" };

	CHECK_STR(sgug_mask_apply(m, 2, "my password here"), "my *** here");
}

static void
test_whole_line(void)
{
	const char *m[] = { "everything" };

	CHECK_STR(sgug_mask_apply(m, 1, "everything"), "***");
}

static void
test_empty_line(void)
{
	const char *m[] = { "x" };

	CHECK_STR(sgug_mask_apply(m, 1, ""), "");
}

/* job.c drops empty mask values, but a stray one must not spin here. */
static void
test_empty_mask_ignored(void)
{
	const char *m[] = { "" };

	CHECK_STR(sgug_mask_apply(m, 1, "untouched"), "untouched");
}

int
main(void)
{
	test_no_masks();
	test_absent();
	test_basic();
	test_one_character_mask();
	test_two_character_mask();
	test_repeats();
	test_overlapping();
	test_whole_line();
	test_empty_line();
	test_empty_mask_ignored();

	if (failures != 0) {
		printf("\n%d failure(s)\n", failures);
		return 1;
	}
	printf("all mask tests passed\n");
	return 0;
}
