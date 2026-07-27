/*
 * Runs on Linux and on IRIX. The point of running both is that a delta between
 * hosts localises a big-endian or 32-bit bug to the function that diverged.
 */

#include "compat/irix.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++; \
		} \
	} while (0)

#define CHECK_EQ_I64(got, want) \
	do { \
		long long g = (long long)(got); \
		long long w = (long long)(want); \
		if (g != w) { \
			printf("FAIL %s:%d: %s = %lld, want %lld\n", \
			    __FILE__, __LINE__, #got, g, w); \
			failures++; \
		} \
	} while (0)

#define CHECK_EQ_STR(got, want) \
	do { \
		const char *g = (got); \
		if (strcmp(g, (want)) != 0) { \
			printf("FAIL %s:%d: %s = \"%s\", want \"%s\"\n", \
			    __FILE__, __LINE__, #got, g, (want)); \
			failures++; \
		} \
	} while (0)

static void
test_parse_http_date(void)
{
	/* Values cross-checked against `date -u -d ... +%s`. */
	CHECK_EQ_I64(sgug_parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT"), 0);
	CHECK_EQ_I64(sgug_parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT"), 784111777);
	CHECK_EQ_I64(sgug_parse_http_date("Mon, 27 Jul 2026 02:13:01 GMT"), 1785118381);

	/* Past the 32-bit time_t wrap. Parsing must stay exact in int64. */
	CHECK_EQ_I64(sgug_parse_http_date("Tue, 19 Jan 2038 03:14:08 GMT"), 2147483648LL);
	CHECK_EQ_I64(sgug_parse_http_date("Fri, 01 Jan 2100 00:00:00 GMT"), 4102444800LL);

	/* Leap day, and the century rule that 2000 is a leap year. */
	CHECK_EQ_I64(sgug_parse_http_date("Tue, 29 Feb 2000 12:00:00 GMT"), 951825600);

	CHECK_EQ_I64(sgug_parse_http_date("garbage"), -1);
	CHECK_EQ_I64(sgug_parse_http_date("Mon, 27 Xxx 2026 02:13:01 GMT"), -1);
	CHECK_EQ_I64(sgug_parse_http_date(NULL), -1);
}

static void
test_format_iso8601(void)
{
	char buf[40];

	/* Seven fractional digits exactly; the service rejects other widths. */
	sgug_format_iso8601(0, buf, sizeof(buf));
	CHECK_EQ_STR(buf, "1970-01-01T00:00:00.0000000Z");

	sgug_format_iso8601(1785118381, buf, sizeof(buf));
	CHECK_EQ_STR(buf, "2026-07-27T02:13:01.0000000Z");

	CHECK(strlen(buf) == 28);
}

static void
test_snprintf_truncation(void)
{
	char buf[8];
	int n;

	/* IRIX vsnprintf returns -1 on truncation. The wrapper must report the
	 * bytes actually written and always terminate. */
	n = sgug_snprintf(buf, sizeof(buf), "%s", "0123456789");
	CHECK_EQ_I64(n, 7);
	CHECK_EQ_STR(buf, "0123456");

	n = sgug_snprintf(buf, sizeof(buf), "%s", "abc");
	CHECK_EQ_I64(n, 3);
	CHECK_EQ_STR(buf, "abc");
}

static void
test_i64toa(void)
{
	char buf[24];

	/* long is 32 bits under the n32 ABI, so anything routed through "%ld"
	 * loses the high word. messageId 1234567890123 came back as its low 32
	 * bits, 1912276171, until this replaced printf. */
	CHECK(sgug_i64toa(1234567890123LL, buf, sizeof(buf)) == 13);
	CHECK_EQ_STR(buf, "1234567890123");

	CHECK(sgug_i64toa(0, buf, sizeof(buf)) == 1);
	CHECK_EQ_STR(buf, "0");

	CHECK(sgug_i64toa(-1, buf, sizeof(buf)) == 2);
	CHECK_EQ_STR(buf, "-1");

	CHECK(sgug_i64toa(2147483648LL, buf, sizeof(buf)) == 10);
	CHECK_EQ_STR(buf, "2147483648");

	CHECK(sgug_i64toa(9223372036854775807LL, buf, sizeof(buf)) == 19);
	CHECK_EQ_STR(buf, "9223372036854775807");

	/* Negating INT64_MIN overflows, so the magnitude is accumulated
	 * unsigned. */
	CHECK(sgug_i64toa((-9223372036854775807LL - 1), buf, sizeof(buf)) == 20);
	CHECK_EQ_STR(buf, "-9223372036854775808");

	CHECK(sgug_i64toa(12345, buf, 5) == -1);
}

static void
test_monotonic(void)
{
	int64_t a = sgug_monotonic_ms();
	int64_t b = sgug_monotonic_ms();

	CHECK(a > 0);
	CHECK(b >= a);
}

int
main(void)
{
	test_parse_http_date();
	test_format_iso8601();
	test_snprintf_truncation();
	test_i64toa();
	test_monotonic();

	if (failures != 0) {
		printf("\n%d failure(s)\n", failures);
		return 1;
	}
	printf("all compat tests passed\n");
	return 0;
}
