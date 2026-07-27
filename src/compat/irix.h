/*
 * Platform gaps between IRIX 6.5 and the POSIX.1-2001 subset the rest of the
 * runner assumes. Include this before anything else.
 *
 * The declarations here are conditional so the same sources build and unit-test
 * on Linux, where the real libc versions are used.
 */

#ifndef SGUG_COMPAT_IRIX_H
#define SGUG_COMPAT_IRIX_H

#include <stdarg.h>
#include <stddef.h>
#include <time.h>

/*
 * Fixed-width types. IRIX only exposes the C99 set from <stdint.h> when the
 * compiler is in c99 mode, which is not true for MIPSPro's default cc driver.
 */
#if defined(__sgi) && !defined(__STDC_VERSION__)
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;
#else
#include <stdint.h>
#endif

/*
 * All wall-clock arithmetic uses this. IRIX time_t is a signed 32-bit long in
 * every ABI, including n64, and wraps on 2038-01-19. Narrow to time_t only when
 * calling into libc.
 */
typedef int64_t sgug_time_t;

/* Seconds since the epoch, from the system clock. */
sgug_time_t sgug_now(void);

/*
 * Monotonic milliseconds for timeouts and backoff. Never derived from the wall
 * clock, which on these machines drifts badly and is often corrected by hand.
 */
int64_t sgug_monotonic_ms(void);

/*
 * Parses an RFC 7231 IMF-fixdate, the format of the HTTP Date header, as UTC.
 * Returns -1 if the input is malformed.
 *
 * Used to measure our clock's offset from GitHub's. A vintage machine with a
 * dead RTC battery is common, and OAuth assertions here live five minutes, so
 * an uncorrected offset makes every token request fail with an opaque 401.
 */
sgug_time_t sgug_parse_http_date(const char *s);

/* Formats as .NET's round-trip "O": 2026-07-26T00:00:00.0000000Z, exactly
 * seven fractional digits. The Actions service rejects other precisions.
 * Needs 29 bytes plus the terminator. */
void sgug_format_iso8601(sgug_time_t t, char *out, size_t outlen);

/*
 * Bounded formatting with one contract on every platform: always NUL
 * terminates, and returns the bytes actually written, excluding the terminator.
 *
 * Neither libc gives us that. C99 snprintf returns the length it would have
 * written, and IRIX's returns -1 on truncation, so the usual measure-then-
 * allocate idiom is unusable here and the two would disagree silently.
 */
int sgug_snprintf(char *buf, size_t size, const char *fmt, ...);
int sgug_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/*
 * Formats a signed 64-bit value as decimal. Returns bytes written, or -1 if
 * out is too small; 21 bytes is always enough.
 *
 * Not printf. Under the n32 ABI `long` is 32 bits, so "%ld" silently truncates
 * anything wider: the protocol's messageId of 1234567890123 comes back as
 * 1912276171, its low 32 bits. "%lld" would be the portable spelling, but IRIX
 * libc already mishandles the C99 length modifiers badly enough that "%z"
 * corrupts varargs outright, so this does not rely on any of them.
 */
int sgug_i64toa(int64_t v, char *out, size_t outlen);

#if defined(__sgi)

/*
 * IRIX declares getaddrinfo in <netdb.h> but marks it `#pragma optional`, a
 * weak symbol. GCC ignores the pragma, so a null check on it folds to a
 * constant true and the fallback is dead code. IPv6 is also incomplete before
 * 6.5.20. The net layer therefore resolves IPv4 only, via gethostbyname.
 */

size_t strnlen(const char *s, size_t maxlen);
void *memmem(const void *hay, size_t haylen, const void *needle, size_t needlelen);
char *strcasestr(const char *hay, const char *needle);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
time_t timegm(struct tm *tm);
char *mkdtemp(char *tmpl);
void explicit_bzero(void *p, size_t n);

#endif /* __sgi */

#endif /* SGUG_COMPAT_IRIX_H */
